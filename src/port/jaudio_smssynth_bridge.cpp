#include "jaudio_smssynth_bridge.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// smssynth is currently shipped as an executable source rather than a reusable
// renderer library. Include it into this translation unit and rename its CLI
// entry point; all decoding/rendering classes then remain in-process and we can
// feed their PCM into Pikmin's own SDL3 stream.
#define main pikmin_embedded_smssynth_cli_main_unused
#include "../../extern/resource_dasm/src/Audio/smssynth.cc"
#undef main

namespace {
constexpr std::size_t kChannels = 2;
constexpr std::size_t kSampleRate = 48000;
constexpr std::size_t kMaxPortQueue = 64;

std::shared_ptr<const ResourceDASM::Audio::SoundEnvironment> g_environment;
float g_bgmVolume = 0.8f;
float g_seVolume = 0.8f;

uint16_t reg_value(const std::shared_ptr<BMSTrack>& track, uint8_t index)
{
    const auto it = track->registers.find(index);
    return it == track->registers.end() ? 0 : static_cast<uint16_t>(it->second);
}

uint32_t read_u24_at(const std::string& data, std::size_t offset)
{
    if (offset + 3 > data.size()) {
        throw std::out_of_range("BMS table read outside sequence");
    }
    const auto* p = reinterpret_cast<const uint8_t*>(data.data() + offset);
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

class PikminBMSRenderer final : public BMSRenderer {
public:
    explicit PikminBMSRenderer(std::shared_ptr<ResourceDASM::Audio::SequenceProgram> sequence)
        : BMSRenderer(sequence,
                      kSampleRate,
                      ResourceDASM::Audio::ResampleMethod::LINEAR_INTERPOLATE,
                      g_environment,
                      empty_tracks_, empty_tracks_, empty_tracks_,
                      1.0, 1.0, 1.0)
    {
    }

    void queue_port(uint32_t connectionId, uint8_t port, uint16_t value)
    {
        if (port >= 16) return;
        auto& queue = pending_ports_[PortKey{connectionId, 0xff, port}];
        if (queue.size() >= kMaxPortQueue) queue.pop_front();
        queue.emplace_back(value);
        service_pending_ports();
    }

    void queue_child_port(uint32_t connectionId, uint8_t child, uint8_t port, uint16_t value)
    {
        if (child >= 16 || port >= 16) return;
        auto& queue = pending_ports_[PortKey{connectionId, child, port}];
        if (queue.size() >= kMaxPortQueue) queue.pop_front();
        queue.emplace_back(value);
        service_pending_ports();
    }

    void service_pending_ports()
    {
        for (auto it = pending_ports_.begin(); it != pending_ports_.end();) {
            auto& queue = it->second;
            if (queue.empty()) {
                it = pending_ports_.erase(it);
                continue;
            }

            std::shared_ptr<BMSTrack> track = connected_track(it->first.connectionId);
            if (track && it->first.child != 0xff) {
                const auto st = states_.find(track.get());
                if (st == states_.end()) {
                    track.reset();
                } else {
                    track = st->second.children[it->first.child].lock();
                }
            }

            if (track) {
                auto& port = state(track).ports[it->first.port];
                if (!port.importFlag) {
                    port.value = queue.front();
                    port.importFlag = true;
                    queue.pop_front();
                }
            }

            if (queue.empty()) {
                it = pending_ports_.erase(it);
            } else {
                ++it;
            }
        }
    }

protected:
    struct PortState {
        uint16_t value = 0;
        bool importFlag = false;
        bool exportFlag = false;
    };

    struct TrackState {
        uint32_t pendingConnectionId = 0;
        std::weak_ptr<BMSTrack> parent;
        std::array<std::weak_ptr<BMSTrack>, 16> children{};
        std::array<PortState, 16> ports{};
    };

    struct PortKey {
        uint32_t connectionId;
        uint8_t child;
        uint8_t port;

        bool operator==(const PortKey& other) const noexcept
        {
            return connectionId == other.connectionId && child == other.child && port == other.port;
        }
    };

    struct PortKeyHash {
        std::size_t operator()(const PortKey& key) const noexcept
        {
            return (static_cast<std::size_t>(key.connectionId) * 1315423911u)
                 ^ (static_cast<std::size_t>(key.child) << 8)
                 ^ key.port;
        }
    };

    TrackState& state(const std::shared_ptr<BMSTrack>& track)
    {
        return states_[track.get()];
    }

    std::shared_ptr<BMSTrack> connected_track(uint32_t id)
    {
        const auto it = connections_.find(id);
        if (it == connections_.end()) return {};
        auto ret = it->second.lock();
        if (!ret) connections_.erase(it);
        return ret;
    }

    bool condition(const std::shared_ptr<BMSTrack>& track, uint8_t code) const
    {
        const uint16_t value = reg_value(track, 3);
        switch (code & 0x0f) {
        case 0: return true;
        case 1: return value == 0;
        case 2: return value != 0;
        case 3: return value == 1;
        case 4: return value >= 0x8000;
        case 5: return value < 0x8000;
        default: return false;
        }
    }

    void write_port(const std::shared_ptr<BMSTrack>& track, uint8_t portIndex, uint16_t value, bool imported)
    {
        if (!track || portIndex >= 16) return;
        auto& port = state(track).ports[portIndex];
        port.value = value;
        if (imported) port.importFlag = true;
        else port.exportFlag = true;
    }

    void execute_call_or_jump_f(const std::shared_ptr<BMSTrack>& track, bool call)
    {
        const uint8_t flags = track->r.get_u8();
        uint32_t targetPc = 0;
        if (flags & 0x80) {
            const uint8_t regIndex = track->r.get_u8();
            targetPc = reg_value(track, regIndex);
            if (flags & 0x40) {
                uint32_t tableBase;
                if (flags & 0x20) {
                    tableBase = reg_value(track, track->r.get_u8());
                } else {
                    tableBase = track->r.get_u24b();
                }
                targetPc = read_u24_at(this->seq->data, tableBase + targetPc * 3);
            }
        } else {
            targetPc = track->r.get_u24b();
        }

        if (targetPc >= track->r.size()) {
            throw std::invalid_argument("conditional BMS branch outside sequence");
        }
        if (condition(track, flags)) {
            if (call) track->call_stack.emplace_back(track->r.where());
            track->r.go(targetPc);
        }
    }

    void execute_opcode(std::multimap<uint64_t, std::shared_ptr<BMSTrack>>::iterator trackIt) override
    {
        service_pending_ports();
        const std::shared_ptr<BMSTrack> track = trackIt->second;
        const std::size_t opcodePc = track->r.where();
        const uint8_t opcode = track->r.get_u8();
        track->r.go(opcodePc);

        switch (opcode) {
        // Start child track. Upstream smssynth already implements this, but we
        // retain the hierarchy so Pikmin can address event tracks beneath the
        // registered 0x20000 connection just like Jam_GetTrackHandle did.
        case 0xC1: {
            track->r.get_u8();
            const uint8_t childId = track->r.get_u8();
            const uint32_t offset = track->r.get_u24b();
            if (offset >= track->r.size()) {
                throw std::invalid_argument("cannot start BMS child outside sequence");
            }
            if ((this->solo_tracks.empty() || this->solo_tracks.count(childId)) && !this->disable_tracks.count(childId)) {
                auto child = std::make_shared<BMSTrack>(childId, this->seq->data, offset, this->seq->index);
                this->tracks.emplace(child);
                this->next_event_to_track.emplace(this->current_time, child);
                child->freq_mult = this->freq_bias;
                state(child).parent = track;
                if (childId < 16) state(track).children[childId] = child;
            }
            return;
        }

        // Pikmin's JAudio command encoding is C0 + command index. These port
        // commands are essential to pikise.jam (system/player/event SFX), but
        // upstream smssynth currently treats several of them as unknown.
        case 0xCB: { // ReadPort(port:u8, reg:u8)
            track->r.get_u8();
            const uint8_t portId = track->r.get_u8();
            const uint8_t reg = track->r.get_u8();
            if (portId < 16) {
                auto& port = state(track).ports[portId];
                track->registers[reg] = static_cast<int16_t>(port.value);
                port.importFlag = false;
            }
            return;
        }
        case 0xCC: { // WritePort(port:u8, reg:u8)
            track->r.get_u8();
            const uint8_t portId = track->r.get_u8();
            const uint8_t reg = track->r.get_u8();
            write_port(track, portId, reg_value(track, reg), false);
            return;
        }
        case 0xCD: { // CheckPortImport(port:u8) -> r3
            track->r.get_u8();
            const uint8_t portId = track->r.get_u8();
            const bool pending = portId < 16 && state(track).ports[portId].importFlag;
            track->registers[3] = pending ? 1 : 0;
            return;
        }
        case 0xCE: { // CheckPortExport(port:u8) -> r3
            track->r.get_u8();
            const uint8_t portId = track->r.get_u8();
            const bool pending = portId < 16 && state(track).ports[portId].exportFlag;
            track->registers[3] = pending ? 1 : 0;
            return;
        }
        case 0xCF: { // WaitReg(reg:u8)
            track->r.get_u8();
            const uint16_t wait = reg_value(track, track->r.get_u8());
            if (wait) {
                this->next_event_to_track.erase(trackIt);
                this->next_event_to_track.emplace(this->current_time + wait, track);
            }
            return;
        }
        case 0xD0: { // ConnectName(hi:u16, lo:u16)
            track->r.get_u8();
            const uint32_t hi = track->r.get_u16b();
            const uint32_t lo = track->r.get_u16b();
            state(track).pendingConnectionId = (hi << 16) | lo;
            return;
        }
        case 0xD1: { // ParentWritePort(packed:u8, reg:u8)
            track->r.get_u8();
            const uint8_t packed = track->r.get_u8();
            const uint8_t reg = track->r.get_u8();
            write_port(state(track).parent.lock(), packed & 0x0f, reg_value(track, reg), true);
            return;
        }
        case 0xD2: { // ChildWritePort(packed:u8, reg:u8)
            track->r.get_u8();
            const uint8_t packed = track->r.get_u8();
            const uint8_t reg = track->r.get_u8();
            const uint8_t childId = packed >> 4;
            auto child = childId < 16 ? state(track).children[childId].lock() : std::shared_ptr<BMSTrack>{};
            write_port(child, packed & 0x0f, reg_value(track, reg), true);
            return;
        }
        case 0xE5: { // ConnectOpen
            track->r.get_u8();
            const uint32_t id = state(track).pendingConnectionId;
            if (id) connections_[id] = track;
            service_pending_ports();
            return;
        }
        case 0xE6: { // ConnectClose
            track->r.get_u8();
            for (auto it = connections_.begin(); it != connections_.end();) {
                if (it->second.lock() == track) it = connections_.erase(it);
                else ++it;
            }
            return;
        }

        // Upstream intentionally doesn't evaluate BMS conditions yet. Pikmin's
        // SFX controller checks port-import state through r3, so implement the
        // original JAudio condition semantics for conditional call/jump/ret.
        case 0xC4: // CallF
            track->r.get_u8();
            execute_call_or_jump_f(track, true);
            return;
        case 0xC8: // JmpF
            track->r.get_u8();
            execute_call_or_jump_f(track, false);
            return;
        case 0xC6: { // RetF(condition:u8)
            track->r.get_u8();
            const uint8_t cond = track->r.get_u8();
            if (condition(track, cond) && !track->call_stack.empty()) {
                track->r.go(track->call_stack.back());
                track->call_stack.pop_back();
            }
            return;
        }
        default:
            track->r.go(opcodePc);
            BMSRenderer::execute_opcode(trackIt);
            return;
        }
    }

private:
    std::unordered_map<const BMSTrack*, TrackState> states_;
    std::unordered_map<uint32_t, std::weak_ptr<BMSTrack>> connections_;
    std::unordered_map<PortKey, std::deque<uint16_t>, PortKeyHash> pending_ports_;
    inline static const std::unordered_set<int16_t> empty_tracks_{};
};

struct RendererSlot {
    std::shared_ptr<PikminBMSRenderer> renderer;
    std::vector<float> pending;
    std::size_t pendingFrame = 0;

    void clear()
    {
        renderer.reset();
        pending.clear();
        pendingFrame = 0;
    }
};

RendererSlot g_coreSe;
RendererSlot g_bgm;
RendererSlot g_demo;

std::shared_ptr<ResourceDASM::Audio::SequenceProgram> sequence_by_name(const char* name)
{
    if (!g_environment || !name) return {};
    try {
        return std::make_shared<ResourceDASM::Audio::SequenceProgram>(g_environment->sequence_programs.at(name));
    } catch (const std::out_of_range&) {
        std::fprintf(stderr, "[pikmin::jaudio] sequence not found: %s\n", name);
        return {};
    }
}

bool start_slot(RendererSlot& slot, const char* name)
{
    try {
        auto sequence = sequence_by_name(name);
        if (!sequence) return false;
        slot.clear();
        slot.renderer = std::make_shared<PikminBMSRenderer>(std::move(sequence));
        std::fprintf(stderr, "[pikmin::jaudio] start sequence=%s\n", name);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[pikmin::jaudio] start sequence=%s failed: %s\n", name ? name : "(null)", e.what());
        slot.clear();
        return false;
    }
}

std::size_t render_slot(RendererSlot& slot, float* out, std::size_t frames, float gain)
{
    if (!slot.renderer || !frames) return 0;
    std::size_t produced = 0;
    try {
        while (produced < frames) {
            if (slot.pendingFrame * kChannels >= slot.pending.size()) {
                slot.pending.clear();
                slot.pendingFrame = 0;
                slot.renderer->service_pending_ports();
                if (!slot.renderer->can_render()) {
                    slot.renderer.reset();
                    break;
                }
                slot.pending = slot.renderer->render_time_step();
                if (slot.pending.empty()) continue;
            }

            const std::size_t availableFrames = slot.pending.size() / kChannels - slot.pendingFrame;
            const std::size_t copyFrames = std::min(frames - produced, availableFrames);
            for (std::size_t i = 0; i < copyFrames; ++i) {
                out[(produced + i) * 2 + 0] += slot.pending[(slot.pendingFrame + i) * 2 + 0] * gain;
                out[(produced + i) * 2 + 1] += slot.pending[(slot.pendingFrame + i) * 2 + 1] * gain;
            }
            slot.pendingFrame += copyFrames;
            produced += copyFrames;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[pikmin::jaudio] renderer stopped after error: %s\n", e.what());
        slot.clear();
    }
    return produced;
}

void clamp_mix(float* out, std::size_t frames)
{
    for (std::size_t i = 0; i < frames * kChannels; ++i) {
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
    }
}
} // namespace

extern "C" bool pikmin_jaudio_synth_init(const char* audioresDirectory)
{
    pikmin_jaudio_synth_shutdown();
    if (!audioresDirectory || !*audioresDirectory) return false;
    try {
        // Keep smssynth quiet in an embedded game. Missing/unimplemented notes
        // are still reported by our wrapper if they terminate a renderer.
        debug_flags = 0;
        g_environment = std::make_shared<ResourceDASM::Audio::SoundEnvironment>(
            ResourceDASM::Audio::load_sound_environment(audioresDirectory));
        std::fprintf(stderr,
                     "[pikmin::jaudio] environment ready dir=%s banks=%zu sequences=%zu\n",
                     audioresDirectory,
                     g_environment->instrument_banks.size(),
                     g_environment->sequence_programs.size());
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[pikmin::jaudio] environment init failed: %s\n", e.what());
        pikmin_jaudio_synth_shutdown();
        return false;
    }
}

extern "C" void pikmin_jaudio_synth_shutdown()
{
    g_coreSe.clear();
    g_bgm.clear();
    g_demo.clear();
    g_environment.reset();
}

extern "C" bool pikmin_jaudio_synth_start_core_se()
{
    return start_slot(g_coreSe, "pikise.jam");
}

extern "C" bool pikmin_jaudio_synth_start_bgm(const char* sequenceName)
{
    return start_slot(g_bgm, sequenceName);
}

extern "C" bool pikmin_jaudio_synth_start_demo_bgm()
{
    return start_slot(g_demo, "demobgm.jam");
}

extern "C" void pikmin_jaudio_synth_stop_bgm()
{
    g_bgm.clear();
}

extern "C" void pikmin_jaudio_synth_stop_demo_bgm()
{
    g_demo.clear();
}

extern "C" void pikmin_jaudio_synth_set_bgm_volume(float volume)
{
    g_bgmVolume = std::clamp(volume, 0.0f, 1.0f);
}

extern "C" void pikmin_jaudio_synth_set_se_volume(float volume)
{
    g_seVolume = std::clamp(volume, 0.0f, 1.0f);
}

extern "C" bool pikmin_jaudio_synth_has_audio()
{
    return static_cast<bool>(g_coreSe.renderer || g_bgm.renderer || g_demo.renderer);
}

extern "C" void pikmin_jaudio_synth_write_core_port(uint32_t connectionId, uint8_t port, uint16_t value)
{
    if (g_coreSe.renderer) g_coreSe.renderer->queue_port(connectionId, port, value);
}

extern "C" void pikmin_jaudio_synth_write_demo_port(uint32_t connectionId, uint8_t port, uint16_t value)
{
    if (g_demo.renderer) g_demo.renderer->queue_port(connectionId, port, value);
}

extern "C" void pikmin_jaudio_synth_write_core_child_port(uint32_t connectionId, uint8_t child,
                                                            uint8_t port, uint16_t value)
{
    if (g_coreSe.renderer) g_coreSe.renderer->queue_child_port(connectionId, child, port, value);
}

extern "C" std::size_t pikmin_jaudio_synth_render(float* out, std::size_t frames)
{
    if (!out || !frames) return 0;
    std::memset(out, 0, frames * kChannels * sizeof(float));
    if (!pikmin_jaudio_synth_has_audio()) return 0;
    render_slot(g_coreSe, out, frames, g_seVolume);
    render_slot(g_bgm, out, frames, g_bgmVolume);
    render_slot(g_demo, out, frames, g_bgmVolume);
    clamp_mix(out, frames);
    return frames;
}
