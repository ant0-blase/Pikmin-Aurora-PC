#include "types.h"
#include "GlobalGameOptions.h"
#include "Dolphin/OS/OSError.h"
#include "Dolphin/dvd.h"
#include "jaudio/app_inter.h"
#include "jaudio/interface.h"
#include "jaudio/piki_bgm.h"
#include "jaudio/piki_player.h"
#include "jaudio/pikidemo.h"
#include "jaudio/pikiinter.h"
#include "jaudio/piki_scene.h"
#include "jaudio/pikiseq.h"
#include "jaudio_smssynth_bridge.h"
#include "jaudio/verysimple.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
constexpr int kOutputSampleRate = 48000;
constexpr int kChannels         = 2;
constexpr float kPi             = 3.14159265358979323846f;
constexpr size_t kMaxToneFrames = 8192;
// Keep each STX decode chunk close to one host audio quantum. 0x480 is an
// exact multiple of both the 18-byte stereo ADPCM block and the 36-byte 4x
// block, and represents about 23 ms at 44.1 kHz.
constexpr size_t kStreamChunkBytes = 0x480;
constexpr size_t kMaxStreamFrames  = 8192;
constexpr size_t kTargetQueuedMs   = 45;

SDL_AudioStream* g_audioStream = nullptr;
SDL_AudioStream* g_jaudioStream = nullptr;
float g_seVolume  = 0.8f;
float g_bgmVolume = 0.8f;
bool g_frozen     = false;
bool g_hostPaused = false;
bool g_placeholderTones = false;
bool g_audioPolicyReady  = false;
bool g_jaudioAttempted   = false;
bool g_jaudioReady       = false;
bool g_challengeMode     = false;
u32 g_currentScene       = static_cast<u32>(SCENE_NULL);
u32 g_currentStage       = 0;
u32 g_currentBgm         = BGM_PikiSE;
int g_streamInputRate = kOutputSampleRate;
float g_toneBuffer[kMaxToneFrames * kChannels]{};
constexpr size_t kJAudioChunkFrames = 512;
constexpr size_t kJAudioTargetQueuedMs = 50;
float g_jaudioFloat[kJAudioChunkFrames * kChannels]{};
std::string g_hostAudioDir;

// Native replacement for JAudio's DVD/DSP stream player. The game thread reads
// and decodes a small amount each Jac_Gsync; SDL only consumes already-decoded
// PCM and never calls back into Pikmin state.
struct HostStxStream {
    DVDFileInfo file{};
    bool open = false;
    u32 dataOffset = 0x20;
    u32 remainingBytes = 0;
    u32 totalSamples = 0;
    u16 sampleRate = 0;
    u16 format = 0;
    u16 frameRate = 0;
    int streamId = -1;
    s16 adpcmStateA[4]{};
    s16 adpcmStateB[4]{};
};

HostStxStream g_stx;
alignas(32) u8 g_stxRead[kStreamChunkBytes + 32]{};
s16 g_stxLeft[kMaxStreamFrames]{};
s16 g_stxRight[kMaxStreamFrames]{};
s16 g_stxLeftB[kMaxStreamFrames]{};
s16 g_stxRightB[kMaxStreamFrames]{};
float g_stxFloat[kMaxStreamFrames * kChannels]{};
char g_audioRoot[128] = "/dataDir/SndData/";

static const char* kStreamFiles[] = {
    "piki.stx", "o_dead.stx", "d_end1.stx", "gyoku.stx", "d_end3.stx", "fanf5.stx", "badend0.stx",
    "badend1.stx", "opening.stx", "happyend1.stx", "compend1.stx", "compend0.stx", "badend2.stx", "onion.stx"
};

// mAudioConfig from the original DEMO_STATUS table. Bit 7 means the low nibble
// selects one of the streamed .stx files above; other values are sequence/BGM.
static const u8 kDemoAudioConfig[] = {
    131,168,192,0,141,141,141,0,0,2,2,2,0,0,0,0,7,9,1,1,6,6,6,6,6,1,10,0,162,162,162,162,
    192,192,192,192,37,37,37,37,0,0,0,0,0,0,129,132,132,132,132,129,132,132,132,132,0,0,0,0,0,0,0,0,
    0,0,0,0,0,134,134,134,134,135,140,171,170,192,6,6,1,0,0,0,0,0,0,162,192,0,132,132,0,0,134,0,0,0,
    0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,192,169
};
static_assert(sizeof(kDemoAudioConfig) == 115, "Pikmin demo audio table size changed");

inline u16 read_be16(const u8* p)
{
    return static_cast<u16>((static_cast<u16>(p[0]) << 8) | p[1]);
}

inline u32 read_be32(const u8* p)
{
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
}

inline s16 clamp_s16(int value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return static_cast<s16>(value);
}


const char* bgm_sequence_name(u32 bgmId)
{
    static const char* names[] = {
        "pikise.jam", "sysevent.jam", nullptr, nullptr, "tutorial.jam", "play3.jam", "d_end2.jam", "jungle.jam",
        nullptr, "yaku.jam", "cave.jam", "boss2.jam", "map.jam", "demobgm.jam", nullptr, nullptr,
        "boss3.jam", "flow.jam", "select.jam", "char.jam", "cresult.jam", "fresult.jam"
    };
    return bgmId < sizeof(names) / sizeof(names[0]) ? names[bgmId] : nullptr;
}

std::string dvd_audio_path(const char* relative)
{
    std::string path = g_audioRoot;
    if (!path.empty() && path.back() != '/') path.push_back('/');
    path += relative;
    return path;
}

std::filesystem::path host_audio_cache_path()
{
    if (const char* overridePath = std::getenv("PIKMIN_AUDIO_CACHE"); overridePath && *overridePath) {
        return std::filesystem::path(overridePath) / "GPIE01_01";
    }
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "pikmin-aurora" / "jaudio-GPIE01_01";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".cache" / "pikmin-aurora" / "jaudio-GPIE01_01";
    }
    return std::filesystem::path("/tmp") / "pikmin-aurora-jaudio-GPIE01_01";
}

bool dump_dvd_audio_file(const char* relative, const std::filesystem::path& output)
{
    DVDFileInfo file{};
    const std::string dvdPath = dvd_audio_path(relative);
    if (!DVDOpen(dvdPath.c_str(), &file)) {
        OSReport("[pikmin::jaudio] DVD asset missing: %s\n", dvdPath.c_str());
        return false;
    }

    const u32 fileLength = file.length;
    try {
        std::error_code ec;
        const auto existingSize = std::filesystem::file_size(output, ec);
        if (!ec && existingSize == fileLength) {
            DVDClose(&file);
            return true;
        }
        std::filesystem::create_directories(output.parent_path());
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out) {
            OSReport("[pikmin::jaudio] cannot create host cache file: %s\n", output.c_str());
            DVDClose(&file);
            return false;
        }

        alignas(32) static u8 buffer[0x20000];
        u32 offset = 0;
        while (offset < fileLength) {
            const u32 amount = std::min<u32>(sizeof(buffer), fileLength - offset);
            if (DVDReadPrio(&file, buffer, static_cast<s32>(amount), static_cast<s32>(offset), 2) < 0) {
                OSReport("[pikmin::jaudio] DVD read failed: %s offset=%u bytes=%u\n", dvdPath.c_str(), offset, amount);
                DVDClose(&file);
                return false;
            }
            out.write(reinterpret_cast<const char*>(buffer), amount);
            if (!out) {
                OSReport("[pikmin::jaudio] host cache write failed: %s\n", output.c_str());
                DVDClose(&file);
                return false;
            }
            offset += amount;
        }
        DVDClose(&file);
        OSReport("[pikmin::jaudio] cached %s (%u bytes)\n", relative, fileLength);
        return true;
    } catch (const std::exception& e) {
        DVDClose(&file);
        OSReport("[pikmin::jaudio] cache failure %s: %s\n", relative, e.what());
        return false;
    }
}

bool read_host_file(const std::filesystem::path& path, std::vector<u8>& data)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto end = in.tellg();
    if (end <= 0) return false;
    data.resize(static_cast<size_t>(end));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(in);
}

std::vector<std::string> aw_files_from_pikibank(const std::filesystem::path& bankPath)
{
    std::vector<u8> data;
    std::vector<std::string> result;
    if (!read_host_file(bankPath, data) || data.size() < 16) return result;

    const u32 wsysTableOffset = read_be32(data.data() + 0x00);
    const u32 wsysCount = read_be32(data.data() + 0x04);
    if (wsysTableOffset > data.size() || static_cast<u64>(wsysTableOffset) + static_cast<u64>(wsysCount) * 8 > data.size()) {
        OSReport("[pikmin::jaudio] malformed WSYS table in pikibank.bx\n");
        return result;
    }

    for (u32 i = 0; i < wsysCount; ++i) {
        const u32 wsysOffset = read_be32(data.data() + wsysTableOffset + i * 8);
        if (static_cast<u64>(wsysOffset) + 24 > data.size()) continue;
        if (std::memcmp(data.data() + wsysOffset, "WSYS", 4) != 0) continue;
        const u32 winfRelative = read_be32(data.data() + wsysOffset + 0x10);
        const u64 winfOffset64 = static_cast<u64>(wsysOffset) + winfRelative;
        if (winfOffset64 + 8 > data.size()) continue;
        const u32 winfOffset = static_cast<u32>(winfOffset64);
        if (std::memcmp(data.data() + winfOffset, "WINF", 4) != 0) continue;
        const u32 awCount = read_be32(data.data() + winfOffset + 4);
        if (static_cast<u64>(winfOffset) + 8 + static_cast<u64>(awCount) * 4 > data.size()) continue;

        for (u32 aw = 0; aw < awCount; ++aw) {
            const u32 entryRelative = read_be32(data.data() + winfOffset + 8 + aw * 4);
            const u64 entryOffset = static_cast<u64>(wsysOffset) + entryRelative;
            if (entryOffset + 112 > data.size()) continue;
            const char* name = reinterpret_cast<const char*>(data.data() + entryOffset);
            size_t length = 0;
            while (length < 112 && name[length]) ++length;
            if (!length || length == 112) continue;
            std::string filename(name, length);
            if (std::find(result.begin(), result.end(), filename) == result.end()) result.emplace_back(std::move(filename));
        }
    }
    return result;
}

bool write_pikiseq_barc(const std::filesystem::path& path)
{
    // HEAD_pikiseq is the original pikiseq.hed/BARC blob embedded in Pikmin's
    // DOL. Its on-disc sibling is pikiseq.arc; resource_dasm calls this header
    // sequence.barc when constructing the JAudio environment.
    const u32 count = read_be32(HEAD_pikiseq + 0x0c);
    const size_t bytes = 0x20 + static_cast<size_t>(count) * 0x20;
    if (count == 0 || count > 128) return false;
    try {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(HEAD_pikiseq), static_cast<std::streamsize>(bytes));
        return static_cast<bool>(out);
    } catch (...) {
        return false;
    }
}

bool prepare_jaudio_assets()
{
    const std::filesystem::path root = host_audio_cache_path();
    const std::filesystem::path banks = root / "Banks";
    const std::filesystem::path seqs = root / "Seqs";

    if (!dump_dvd_audio_file("Banks/pikibank.bx", banks / "pikibank.bx")) return false;
    if (!dump_dvd_audio_file("Seqs/pikiseq.arc", seqs / "pikiseq.arc")) return false;
    if (!write_pikiseq_barc(seqs / "sequence.barc")) {
        OSReport("[pikmin::jaudio] failed to create sequence.barc from embedded pikiseq header\n");
        return false;
    }

    const auto awFiles = aw_files_from_pikibank(banks / "pikibank.bx");
    if (awFiles.empty()) {
        OSReport("[pikmin::jaudio] pikibank.bx contains no usable AW references\n");
        return false;
    }
    size_t cached = 0;
    for (const auto& filename : awFiles) {
        const std::string relative = std::string("Banks/") + filename;
        if (dump_dvd_audio_file(relative.c_str(), banks / filename)) ++cached;
    }
    if (cached != awFiles.size()) {
        OSReport("[pikmin::jaudio] only %zu/%zu AW banks cached; synth will use available samples\n", cached, awFiles.size());
    }

    g_hostAudioDir = root.string();
    OSReport("[pikmin::jaudio] audio resources ready dir=%s aw=%zu\n", g_hostAudioDir.c_str(), cached);
    return cached != 0;
}

bool env_enabled(const char* name, bool fallback = false)
{
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 && std::strcmp(value, "off") != 0;
}

void ensure_audio_policy()
{
    if (g_audioPolicyReady) return;
    g_audioPolicyReady = true;
    g_placeholderTones = env_enabled("PIKMIN_AUDIO_PLACEHOLDER_TONES", false);
}

void ensure_audio()
{
    if (g_audioStream) {
        return;
    }

    ensure_audio_policy();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        OSReport("[pikmin::audio] SDL audio init failed: %s\n", SDL_GetError());
        return;
    }

    // SDL_OpenAudioDeviceStream() receives the application-side format. SDL
    // converts this to the actual playback device format. STX files can be
    // either 44.1 or 48 kHz, so start at 48 kHz and update the stream input
    // format whenever a movie/stream track is started.
    SDL_AudioSpec spec{};
    spec.format   = SDL_AUDIO_F32;
    spec.channels = kChannels;
    spec.freq     = kOutputSampleRate;

    g_audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_audioStream) {
        OSReport("[pikmin::audio] open playback stream failed: %s\n", SDL_GetError());
        return;
    }
    g_streamInputRate = kOutputSampleRate;

    if (!g_hostPaused && !g_frozen && !SDL_ResumeAudioStreamDevice(g_audioStream)) {
        OSReport("[pikmin::audio] resume playback failed: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_audioStream);
        g_audioStream = nullptr;
        return;
    }

    OSReport("[pikmin::audio] SDL3 playback ready: app=%d Hz stereo float targetQueue=%zu ms chunk=0x%zx placeholders=%s\n",
             kOutputSampleRate, kTargetQueuedMs, kStreamChunkBytes, g_placeholderTones ? "on" : "off");
}

void ensure_jaudio_audio()
{
    if (g_jaudioStream || !g_jaudioReady) return;
    ensure_audio();
    if (!g_audioStream) return;

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = kChannels;
    spec.freq = kOutputSampleRate;
    g_jaudioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_jaudioStream) {
        OSReport("[pikmin::jaudio] SDL stream open failed: %s\n", SDL_GetError());
        return;
    }
    if (!g_hostPaused && !g_frozen && !SDL_ResumeAudioStreamDevice(g_jaudioStream)) {
        OSReport("[pikmin::jaudio] SDL stream resume failed: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_jaudioStream);
        g_jaudioStream = nullptr;
        return;
    }
    OSReport("[pikmin::jaudio] SDL3 synth stream ready: %d Hz stereo float targetQueue=%zu ms\n",
             kOutputSampleRate, kJAudioTargetQueuedMs);
}

bool init_jaudio_backend()
{
    if (g_jaudioAttempted) return g_jaudioReady;
    g_jaudioAttempted = true;
    if (!env_enabled("PIKMIN_JAUDIO", true)) {
        OSReport("[pikmin::jaudio] disabled by PIKMIN_JAUDIO=0\n");
        return false;
    }
    if (!prepare_jaudio_assets()) return false;
    if (!pikmin_jaudio_synth_init(g_hostAudioDir.c_str())) return false;
    g_jaudioReady = pikmin_jaudio_synth_start_core_se();
    if (!g_jaudioReady) {
        OSReport("[pikmin::jaudio] core SE sequence pikise.jam failed to start\n");
        pikmin_jaudio_synth_shutdown();
        return false;
    }
    pikmin_jaudio_synth_set_bgm_volume(g_bgmVolume);
    pikmin_jaudio_synth_set_se_volume(g_seVolume);
    ensure_jaudio_audio();
    OSReport("[pikmin::jaudio] native JAudio synth armed: BMS=.jam banks=.bx/.aw core-SE=real\n");
    return true;
}

void tick_jaudio()
{
    if (!g_jaudioReady || g_frozen || g_hostPaused || !pikmin_jaudio_synth_has_audio()) return;
    ensure_jaudio_audio();
    if (!g_jaudioStream) return;

    int queuedBytes = std::max(0, SDL_GetAudioStreamQueued(g_jaudioStream));
    int queuedFrames = queuedBytes / static_cast<int>(kChannels * sizeof(float));
    const int targetFrames = kOutputSampleRate * static_cast<int>(kJAudioTargetQueuedMs) / 1000;
    while (queuedFrames < targetFrames) {
        const size_t request = std::min<size_t>(kJAudioChunkFrames, static_cast<size_t>(targetFrames - queuedFrames));
        const size_t frames = pikmin_jaudio_synth_render(g_jaudioFloat, request);
        if (!frames) break;
        const int bytes = static_cast<int>(frames * kChannels * sizeof(float));
        if (!SDL_PutAudioStreamData(g_jaudioStream, g_jaudioFloat, bytes)) {
            OSReport("[pikmin::jaudio] SDL queue failed: %s\n", SDL_GetError());
            break;
        }
        queuedBytes = std::max(0, SDL_GetAudioStreamQueued(g_jaudioStream));
        queuedFrames = queuedBytes / static_cast<int>(kChannels * sizeof(float));
    }
}

bool play_bgm_id(u32 bgmId)
{
    if (!init_jaudio_backend()) return false;
    const char* name = bgm_sequence_name(bgmId);
    if (!name || bgmId == BGM_PikiSE || bgmId == BGM_SysEvent) return false;
    if (!pikmin_jaudio_synth_start_bgm(name)) return false;
    OSReport("[pikmin::jaudio] BGM id=%u sequence=%s\n", bgmId, name);
    tick_jaudio();
    return true;
}

bool set_stream_input_rate(int rate)
{
    ensure_audio();
    if (!g_audioStream) return false;
    if (rate <= 0) rate = kOutputSampleRate;
    if (g_streamInputRate == rate) return true;

    SDL_AudioSpec src{};
    src.format   = SDL_AUDIO_F32;
    src.channels = kChannels;
    src.freq     = rate;
    if (!SDL_SetAudioStreamFormat(g_audioStream, &src, nullptr)) {
        OSReport("[pikmin::audio] failed to set stream input rate=%d: %s\n", rate, SDL_GetError());
        return false;
    }
    g_streamInputRate = rate;
    OSReport("[pikmin::audio] stream input rate=%d Hz (SDL3 resampler active)\n", rate);
    return true;
}

void clear_audio_queue()
{
    if (g_audioStream && !SDL_ClearAudioStream(g_audioStream)) {
        OSReport("[pikmin::audio] clear queue failed: %s\n", SDL_GetError());
    }
}

void queue_pcm(const float* data, size_t frames)
{
    ensure_audio();
    if (!g_audioStream || g_frozen || g_hostPaused || !frames) {
        return;
    }
    const int bytes = static_cast<int>(frames * kChannels * sizeof(float));
    if (!SDL_PutAudioStreamData(g_audioStream, data, bytes)) {
        OSReport("[pikmin::audio] queue failed: %s\n", SDL_GetError());
    }
}

void queue_tone(float f0, float f1, float seconds, float gain)
{
    ensure_audio_policy();
    if (!g_placeholderTones) return;
    if (!set_stream_input_rate(kOutputSampleRate)) return;
    size_t frames = std::max<size_t>(1, static_cast<size_t>(seconds * kOutputSampleRate));
    frames = std::min(frames, kMaxToneFrames);
    float phase = 0.0f;
    for (size_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(frames);
        const float freq = f0 + (f1 - f0) * t;
        phase += 2.0f * kPi * freq / static_cast<float>(kOutputSampleRate);
        const float attack = std::min(1.0f, t * 30.0f);
        const float release = std::min(1.0f, (1.0f - t) * 12.0f);
        const float env = attack * release;
        const float sample = std::sin(phase) * env * gain;
        g_toneBuffer[i * 2 + 0] = sample;
        g_toneBuffer[i * 2 + 1] = sample;
    }
    queue_pcm(g_toneBuffer, frames);
}

void queue_pikmin_boot_cue()
{
    // The boot voice is a banked JAudio SFX, not an STX stream. Keep an audible
    // stand-in until the pikibank.bx/.aw bank decoder is connected.
    queue_tone(620.0f, 760.0f, 0.105f, 0.30f * g_seVolume);
    queue_tone(760.0f, 930.0f, 0.105f, 0.28f * g_seVolume);
    queue_tone(930.0f, 690.0f, 0.165f, 0.30f * g_seVolume);
}

void stop_stx(bool clearQueued = true)
{
    if (g_stx.open) {
        DVDClose(&g_stx.file);
        OSReport("[pikmin::audio] STX stop id=%d\n", g_stx.streamId);
    }
    g_stx = HostStxStream{};
    if (clearQueued) clear_audio_queue();
}

void finish_stx_decode()
{
    if (!g_stx.open) return;
    DVDClose(&g_stx.file);
    g_stx.open = false;
    OSReport("[pikmin::audio] STX decode complete id=%d; draining queued PCM\n", g_stx.streamId);
}

bool start_stx(int streamId)
{
    if (streamId < 0 || streamId >= static_cast<int>(sizeof(kStreamFiles) / sizeof(kStreamFiles[0]))) {
        return false;
    }

    stop_stx();
    char path[192];
    const size_t rootLen = strlen(g_audioRoot);
    const size_t fileLen = strlen(kStreamFiles[streamId]);
    if (rootLen + fileLen + 1 > sizeof(path)) {
        OSReport("[pikmin::audio] STX path too long root=%s file=%s\n", g_audioRoot, kStreamFiles[streamId]);
        return false;
    }
    memcpy(path, g_audioRoot, rootLen);
    memcpy(path + rootLen, kStreamFiles[streamId], fileLen + 1);

    if (!DVDOpen(path, &g_stx.file)) {
        OSReport("[pikmin::audio] STX open failed id=%d path=%s\n", streamId, path);
        g_stx = HostStxStream{};
        return false;
    }

    alignas(32) u8 header[32]{};
    if (DVDReadPrio(&g_stx.file, header, sizeof(header), 0, 2) < 0) {
        OSReport("[pikmin::audio] STX header read failed path=%s\n", path);
        stop_stx();
        return false;
    }

    const u32 declaredBytes = read_be32(header + 0x00);
    g_stx.totalSamples = read_be32(header + 0x04);
    g_stx.sampleRate   = read_be16(header + 0x08);
    g_stx.format       = read_be16(header + 0x0A);
    g_stx.frameRate    = read_be16(header + 0x0E);
    g_stx.streamId     = streamId;
    g_stx.dataOffset   = 0x20;

    const u32 physicalBytes = g_stx.file.length > 0x20 ? g_stx.file.length - 0x20 : 0;
    g_stx.remainingBytes = std::min(declaredBytes, physicalBytes);
    g_stx.open = g_stx.remainingBytes != 0 && g_stx.sampleRate != 0;

    if (!g_stx.open || g_stx.format < 2 || g_stx.format > 5) {
        OSReport("[pikmin::audio] unsupported/empty STX id=%d fmt=%u rate=%u bytes=%u samples=%u\n",
                 streamId, g_stx.format, g_stx.sampleRate, g_stx.remainingBytes, g_stx.totalSamples);
        stop_stx();
        return false;
    }

    // A track switch must not inherit ~hundreds of milliseconds of PCM from
    // the previous scene. The source format is the STX's real rate; SDL3 does
    // device-rate conversion for us.
    clear_audio_queue();
    if (!set_stream_input_rate(g_stx.sampleRate)) {
        stop_stx();
        return false;
    }

    OSReport("[pikmin::audio] STX start id=%d file=%s fmt=%u rate=%u samples=%u data=%u\n",
             streamId, kStreamFiles[streamId], g_stx.format, g_stx.sampleRate, g_stx.totalSamples, g_stx.remainingBytes);
    return true;
}

size_t decode_adpcm_pair(const u8* src, size_t groups, s16* left, s16* right, s16 state[4])
{
    static constexpr s16 filters[16][2] = {
        {0x0000,0x0000},{0x0800,0x0000},{0x0000,0x0800},{0x0400,0x0400},{0x1000,-0x0800},{0x0E00,-0x0600},
        {0x0C00,-0x0400},{0x1200,-0x0A00},{0x1068,-0x08C8},{0x12C0,-0x08FC},{0x1400,-0x0C00},{0x0800,-0x0800},
        {0x0400,-0x0400},{-0x0400,0x0400},{-0x0400,0x0000},{-0x0800,0x0000}
    };
    static constexpr s8 nibble[16] = {0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1};

    s16 la = state[0], lb = state[1], ra = state[2], rb = state[3];
    size_t out = 0;
    for (size_t g = 0; g < groups; ++g) {
        for (int channel = 0; channel < 2; ++channel) {
            const u8 header = *src++;
            const int shift = (header >> 4) & 0xF;
            const int c1 = filters[header & 0xF][0];
            const int c2 = filters[header & 0xF][1];
            s16& a = channel == 0 ? la : ra;
            s16& b = channel == 0 ? lb : rb;
            s16* dst = channel == 0 ? left + out : right + out;
            for (int j = 0; j < 8; ++j) {
                const u8 packed = *src++;
                int value = (static_cast<int>(nibble[packed >> 4]) * (1 << shift)) + ((c1 * a + c2 * b) >> 11);
                dst[j * 2 + 0] = clamp_s16(value);
                b = dst[j * 2 + 0];
                value = (static_cast<int>(nibble[packed & 0xF]) * (1 << shift)) + ((c1 * b + c2 * a) >> 11);
                dst[j * 2 + 1] = clamp_s16(value);
                a = dst[j * 2 + 1];
            }
        }
        out += 16;
    }
    state[0] = la; state[1] = lb; state[2] = ra; state[3] = rb;
    return out;
}

size_t decode_stx_chunk(size_t bytes)
{
    if (!bytes) return 0;
    size_t frames = 0;
    switch (g_stx.format) {
    case 2: // big-endian signed 16-bit stereo PCM
        frames = std::min(bytes / 4, kMaxStreamFrames);
        for (size_t i = 0; i < frames; ++i) {
            g_stxLeft[i]  = static_cast<s16>(read_be16(g_stxRead + i * 4 + 0));
            g_stxRight[i] = static_cast<s16>(read_be16(g_stxRead + i * 4 + 2));
        }
        break;
    case 3: // signed 8-bit stereo PCM
        frames = std::min(bytes / 2, kMaxStreamFrames);
        for (size_t i = 0; i < frames; ++i) {
            g_stxLeft[i]  = static_cast<s16>(static_cast<int>(static_cast<s8>(g_stxRead[i * 2 + 0])) * 256);
            g_stxRight[i] = static_cast<s16>(static_cast<int>(static_cast<s8>(g_stxRead[i * 2 + 1])) * 256);
        }
        break;
    case 4: { // Nintendo stereo ADPCM: 9 bytes left + 9 bytes right => 16 frames
        const size_t groups = std::min(bytes / 18, kMaxStreamFrames / 16);
        frames = decode_adpcm_pair(g_stxRead, groups, g_stxLeft, g_stxRight, g_stx.adpcmStateA);
        break;
    }
    case 5: { // 4x stream: two stereo ADPCM pairs mixed together
        const size_t groups = std::min(bytes / 36, kMaxStreamFrames / 16);
        // Decode each 18-byte pair independently to match JAudio's ADPCM4X mixer.
        for (size_t g = 0; g < groups; ++g) {
            decode_adpcm_pair(g_stxRead + g * 36, 1, g_stxLeft + g * 16, g_stxRight + g * 16, g_stx.adpcmStateA);
            decode_adpcm_pair(g_stxRead + g * 36 + 18, 1, g_stxLeftB + g * 16, g_stxRightB + g * 16, g_stx.adpcmStateB);
        }
        frames = groups * 16;
        for (size_t i = 0; i < frames; ++i) {
            g_stxLeft[i] = clamp_s16((static_cast<int>(g_stxLeft[i]) * 0x5fff + static_cast<int>(g_stxLeftB[i]) * 0x5fff) >> 15);
            g_stxRight[i] = clamp_s16((static_cast<int>(g_stxRight[i]) * 0x5fff + static_cast<int>(g_stxRightB[i]) * 0x5fff) >> 15);
        }
        break;
    }
    }

    const float gain = 0.85f * g_bgmVolume / 32768.0f;
    for (size_t i = 0; i < frames; ++i) {
        g_stxFloat[i * 2 + 0] = static_cast<float>(g_stxLeft[i]) * gain;
        g_stxFloat[i * 2 + 1] = static_cast<float>(g_stxRight[i]) * gain;
    }
    return frames;
}

bool queue_next_stx_chunk()
{
    if (!g_stx.open || !g_stx.remainingBytes) {
        finish_stx_decode();
        return false;
    }
    const u32 readBytes = std::min<u32>(g_stx.remainingBytes, kStreamChunkBytes);
    if (DVDReadPrio(&g_stx.file, g_stxRead, static_cast<s32>(readBytes), static_cast<s32>(g_stx.dataOffset), 2) < 0) {
        OSReport("[pikmin::audio] STX read failed id=%d offset=%u bytes=%u\n", g_stx.streamId, g_stx.dataOffset, readBytes);
        stop_stx();
        return false;
    }
    const size_t frames = decode_stx_chunk(readBytes);
    g_stx.dataOffset += readBytes;
    g_stx.remainingBytes -= readBytes;
    if (!frames) {
        OSReport("[pikmin::audio] STX decode produced no frames id=%d fmt=%u bytes=%u\n", g_stx.streamId, g_stx.format, readBytes);
        stop_stx();
        return false;
    }
    queue_pcm(g_stxFloat, frames);
    if (!g_stx.remainingBytes) finish_stx_decode();
    return true;
}

void tick_stx()
{
    if (!g_stx.open || g_frozen || g_hostPaused) return;
    ensure_audio();
    if (!g_audioStream) return;

    // Query SDL's real input queue instead of guessing that exactly 1/60 sec
    // was consumed each Jac_Gsync. This keeps audio stable when a game frame is
    // late or the window is paused/minimized.
    const int queuedBytes = std::max(0, SDL_GetAudioStreamQueued(g_audioStream));
    int queuedFrames = queuedBytes / static_cast<int>(kChannels * sizeof(float));
    const int targetFrames = std::max(1, static_cast<int>(g_stx.sampleRate) * static_cast<int>(kTargetQueuedMs) / 1000);
    while (g_stx.open && queuedFrames < targetFrames) {
        if (!queue_next_stx_chunk()) break;
        const int nowQueuedBytes = std::max(0, SDL_GetAudioStreamQueued(g_audioStream));
        queuedFrames = nowQueuedBytes / static_cast<int>(kChannels * sizeof(float));
    }
}

struct HostEventState {
    bool active = false;
    u32 type = 0;
    u8 nextSlot = 0;
    std::array<int, 16> actionForSlot{};

    void reset()
    {
        active = false;
        type = 0;
        nextSlot = 0;
        actionForSlot.fill(-1);
    }
};

HostEventState g_hostEvents[16]{};
static constexpr u32 kEventOffsets[] = { 0, 1, 0xad, 0xbe, 0xcd, 0xd7, 0xdb, 0x105 };
static constexpr u16 kEventActionCommands[] = {
    0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x008, 0x009, 0x0E5, 0x00A,
    0x00B, 0x00C, 0x00D, 0x00E, 0x00F, 0x010, 0x011, 0x012, 0x013, 0x0D6, 0x0D7, 0x014,
    0x015, 0x016, 0x017, 0x018, 0x019, 0x029, 0x01A, 0x01B, 0x01C, 0x01D, 0x01E, 0x01F,
    0x020, 0x021, 0x022, 0x023, 0x0E6, 0x027, 0x028, 0x02A, 0x103, 0x104, 0x105, 0x02B,
    0x02C, 0x025, 0x026, 0x02D, 0x02E, 0x02F, 0x024, 0x050, 0x051, 0x052, 0x053, 0x054,
    0x055, 0x056, 0x057, 0x058, 0x059, 0x05A, 0x05B, 0x05C, 0x05D, 0x05E, 0x060, 0x061,
    0x062, 0x063, 0x064, 0x065, 0x066, 0x067, 0x068, 0x069, 0x06A, 0x06B, 0x06C, 0x06D,
    0x06E, 0x06F, 0x070, 0x071, 0x072, 0x073, 0x074, 0x075, 0x076, 0x107, 0x078, 0x079,
    0x07A, 0x07B, 0x07C, 0x07D, 0x07E, 0x0E9, 0x0E8, 0x082, 0x083, 0x084, 0x086, 0x087,
    0x088, 0x089, 0x08A, 0x08C, 0x08D, 0x0C0, 0x0C1, 0x01D, 0x020, 0x119, 0x023, 0x11A,
    0x11B, 0x0C2, 0x0C3, 0x0C4, 0x0C5, 0x0C6, 0x0E7, 0x0C7, 0x0C8, 0x0C9, 0x0D5, 0x0CA,
    0x0CB, 0x0CC, 0x0CD, 0x0CE, 0x0CF, 0x0D0, 0x0D1, 0x106, 0x0D2, 0x0D3, 0x0D4, 0x114,
    0x0E0, 0x0E1, 0x0E2, 0x0E3, 0x0E4, 0x115, 0x116, 0x117, 0x118, 0x0F0, 0x0F1, 0x0F2,
    0x0F3, 0x0F4, 0x0F5, 0x0F6, 0x0F7, 0x0F8, 0x0F9, 0x0FA, 0x0FE, 0x0FB, 0x0FC, 0x0FD,
    0x0FF, 0x100, 0x113, 0x101, 0x102, 0x040, 0x041, 0x042, 0x01A, 0x04E, 0x04F, 0x043,
    0x044, 0x045, 0x046, 0x047, 0x048, 0x049, 0x04A, 0x04B, 0x04C, 0x04D, 0x030, 0x031,
    0x032, 0x033, 0x034, 0x035, 0x036, 0x037, 0x038, 0x039, 0x090, 0x03A, 0x03B, 0x112,
    0x03C, 0x02B, 0x02C, 0x01B, 0x08B, 0x091, 0x02B, 0x0DD, 0x0DE, 0x0DF, 0x0EA, 0x090,
    0x0D9, 0x029, 0x08F, 0x0A0, 0x0A1, 0x0A2, 0x0A3, 0x0A4, 0x0A5, 0x0A6, 0x0A7, 0x0A8,
    0x0A9, 0x0AA, 0x0AB, 0x0AC, 0x0AD, 0x0AE, 0x0AF, 0x0B0, 0x0B1, 0x0B2, 0x0B3, 0x0B4,
    0x0B5, 0x0B6, 0x0B7, 0x0B8, 0x0B9, 0x0BA, 0x0BB, 0x0BC, 0x0BD, 0x035, 0x015, 0x016,
    0x017, 0x018, 0x032, 0x033, 0x034, 0x036, 0x039, 0x090, 0x000, 0x092, 0x093, 0x094,
    0x095, 0x0DA, 0x0DB, 0x0DC, 0x108, 0x109, 0x10A, 0x10B, 0x10C, 0x10D, 0x10E, 0x10F,
    0x110, 0x111,
};

u16 event_action_command(u32 eventType, int actionId)
{
    if (eventType >= sizeof(kEventOffsets) / sizeof(kEventOffsets[0]) || actionId < 0) return 0xffff;
    const size_t index = static_cast<size_t>(kEventOffsets[eventType]) + static_cast<size_t>(actionId);
    if (index >= sizeof(kEventActionCommands) / sizeof(kEventActionCommands[0])) return 0xffff;
    return kEventActionCommands[index];
}

u32 scene_bgm_id(u32 scene, u32 stage)
{
    static constexpr u32 stageBgm[] = { BGM_Tutorial, BGM_Play3, BGM_Cave, BGM_Yakushima, BGM_Flow };
    switch (scene) {
    case SCENE_Title: return BGM_Jungle;
    case SCENE_FileSelect: return BGM_Select;
    case SCENE_WorldMap: return BGM_Map;
    case SCENE_Course:
        return stage < sizeof(stageBgm) / sizeof(stageBgm[0]) ? stageBgm[stage] : BGM_Tutorial;
    case SCENE_Results:
        if (g_challengeMode) return BGM_ChalResult;
        return stage == JACRES_FinalResult ? BGM_FinalResult : BGM_PikiSE;
    case SCENE_ChalSelect: return BGM_Char;
    default: return BGM_PikiSE;
    }
}

void start_demo_audio(u32 demoId)
{
    if (demoId >= sizeof(kDemoAudioConfig)) return;
    const u8 cfg = kDemoAudioConfig[demoId];
    if (cfg & 0x80) {
        // Movie/cutscene streams are the original STX data from the disc.
        pikmin_jaudio_synth_stop_demo_bgm();
        start_stx(cfg & 0x0F);
    } else if (cfg != 0 && cfg != 0x40) {
        // Non-streamed demos drive demobgm.jam through application port 0.
        // Keep the renderer alive and send the same low-nibble cue as JAudio.
        stop_stx(false);
        if (init_jaudio_backend() && pikmin_jaudio_synth_start_demo_bgm()) {
            pikmin_jaudio_synth_write_demo_port(0x30000, 0, cfg & 0x0F);
            tick_jaudio();
        } else {
            const float base = 180.0f + static_cast<float>(cfg & 0x1F) * 13.0f;
            queue_tone(base, base * 1.03f, 0.16f, 0.08f * g_bgmVolume);
        }
    }
}
}

extern "C" {
void Jac_AddDVDBuffer(u8*, u32) {}
void Jac_BackDVDBuffer() {}

int Jac_CheckFreeEvents(void)
{
    int freeCount = 0;
    for (const auto& event : g_hostEvents) {
        if (!event.active) ++freeCount;
    }
    return freeCount;
}

int Jac_CreateEvent(u32 eventType, SVector_*)
{
    if (!eventType) return -1;
    for (int i = 0; i < static_cast<int>(std::size(g_hostEvents)); ++i) {
        if (g_hostEvents[i].active) continue;
        g_hostEvents[i].reset();
        g_hostEvents[i].active = true;
        g_hostEvents[i].type = eventType;
        if (eventType == 7) Jac_PlayEventAction(i, 4);
        return i;
    }
    return -1;
}

BOOL Jac_DemoFrame(int) { return TRUE; }

BOOL Jac_DestroyEvent(s32 idx)
{
    if (idx < 0 || idx >= static_cast<s32>(std::size(g_hostEvents)) || !g_hostEvents[idx].active) return FALSE;
    if (g_jaudioReady) {
        // The original event command queue sends 0xffff to the event child track.
        pikmin_jaudio_synth_write_core_child_port(0x20000, static_cast<u8>(idx), 0, 0xffff);
    }
    g_hostEvents[idx].reset();
    return TRUE;
}

void Jac_EnterBossMode(void) {}
void Jac_ExitBossMode(void) {}

void Jac_FinishDemo(void)
{
    stop_stx();
    pikmin_jaudio_synth_stop_demo_bgm();
    if (g_jaudioReady) pikmin_jaudio_synth_write_core_port(0x1000f, 0, 0);
}

void Jac_FinishPartsFindDemo(void) {}
void Jac_FinishTextDemo(void) {}

void Jac_Freeze(void)
{
    g_frozen = true;
    if (g_audioStream) SDL_PauseAudioStreamDevice(g_audioStream);
    if (g_jaudioStream) SDL_PauseAudioStreamDevice(g_jaudioStream);
}

void Jac_Freeze_Precall(void) { Jac_Freeze(); }

void pikmin_host_audio_set_paused(BOOL paused)
{
    const bool next = paused != FALSE;
    if (g_hostPaused == next) return;
    g_hostPaused = next;

    auto setPaused = [&](SDL_AudioStream* stream) {
        if (!stream) return;
        if (g_hostPaused) {
            SDL_PauseAudioStreamDevice(stream);
        } else if (!g_frozen) {
            SDL_ResumeAudioStreamDevice(stream);
        }
    };
    setPaused(g_audioStream);
    setPaused(g_jaudioStream);
    OSReport("[pikmin::audio] host %s\n", g_hostPaused ? "paused" : "resumed");
}

void Jac_Gsync(void)
{
    tick_stx();
    tick_jaudio();
}

void Jac_InitAllEvent(void)
{
    for (int i = 0; i < static_cast<int>(std::size(g_hostEvents)); ++i) {
        Jac_DestroyEvent(i);
    }
}

void Jac_Orima_Formation(s32 stickX, s32 stickY)
{
    static bool active = false;
    if (!init_jaudio_backend()) return;

    stickX = std::clamp<s32>(stickX, -127, 127);
    stickY = std::clamp<s32>(stickY, -127, 127);
    if (stickY < 0) stickY = -stickY;
    const int magnitude = static_cast<int>(std::sqrt(static_cast<float>(stickX * stickX + stickY * stickY)));

    pikmin_jaudio_synth_write_core_port(0x10007, 2, static_cast<u16>(stickX));
    pikmin_jaudio_synth_write_core_port(0x10007, 3, static_cast<u16>(magnitude));
    const bool nextActive = stickX != 0 || magnitude != 0;
    if (nextActive != active) {
        pikmin_jaudio_synth_write_core_port(0x10007, 0, nextActive ? 1 : 0);
        active = nextActive;
    }
    tick_jaudio();
}

void Jac_Orima_Walk(s32 groundSoundID, u32)
{
    if (!init_jaudio_backend()) return;
    pikmin_jaudio_synth_write_core_port(0x10008, 0, static_cast<u16>(groundSoundID));
    tick_jaudio();
}

void Jac_OutputMode(int) {}

void Jac_Piki_Number(u32 pikiNum)
{
    // This is the same compressed piki-count value used by the original gaya
    // controller track (0x10003).
    u16 value;
    if (pikiNum >= 100) value = 29;
    else if (pikiNum >= 50) value = static_cast<u16>((pikiNum - 50) / 10 + 25);
    else if (pikiNum >= 25) value = static_cast<u16>((pikiNum - 25) / 5 + 20);
    else if (pikiNum >= 15) value = static_cast<u16>((pikiNum - 15) / 2 + 15);
    else value = static_cast<u16>(pikiNum);
    if (init_jaudio_backend()) pikmin_jaudio_synth_write_core_port(0x10003, 0, value);
}

BOOL Jac_PlayEventAction(int eventIdx, int actionId)
{
    if (eventIdx < 0 || eventIdx >= static_cast<int>(std::size(g_hostEvents))) return FALSE;
    auto& event = g_hostEvents[eventIdx];
    if (!event.active) return FALSE;
    const u16 command = event_action_command(event.type, actionId);
    if (command == 0xffff) return FALSE;
    if (!init_jaudio_backend()) return FALSE;

    // Keep a stable action -> voice-slot mapping. The original event manager has
    // priority/group arbitration here; round-robin is sufficient for the host
    // bridge while still preserving explicit stop requests.
    int slot = -1;
    for (int i = 0; i < 16; ++i) {
        if (event.actionForSlot[i] == actionId) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < 16; ++i) {
            const int candidate = (event.nextSlot + i) & 0x0f;
            if (event.actionForSlot[candidate] < 0) { slot = candidate; break; }
        }
    }
    if (slot < 0) slot = event.nextSlot & 0x0f;
    event.nextSlot = static_cast<u8>((slot + 1) & 0x0f);
    event.actionForSlot[slot] = actionId;

    pikmin_jaudio_synth_write_core_child_port(
        0x20000, static_cast<u8>(eventIdx), 0,
        static_cast<u16>((slot << 12) | (command & 0x0fff)));
    tick_jaudio();
    return TRUE;
}

void Jac_PlayOrimaSe(u32 id)
{
    if (init_jaudio_backend()) {
        if (id & JACORIMA_PIKISOUND) {
            pikmin_jaudio_synth_write_core_port(0x1000a, 1, static_cast<u16>(id & ~JACORIMA_PIKISOUND));
        } else {
            pikmin_jaudio_synth_write_core_port(0x1000a, 0, static_cast<u16>(id));
        }
        tick_jaudio();
        return;
    }

    // Diagnostic fallback only; v36 made placeholder tones opt-in.
    if (id == JACORIMA_Unk800C) queue_pikmin_boot_cue();
    else {
        const float base = 300.0f + static_cast<float>(id & 0x3f) * 7.0f;
        queue_tone(base, base * 1.08f, 0.065f, 0.10f * g_seVolume);
    }
}

void Jac_PlaySystemSe(s32 id)
{
    // The original forwards this special UI sound through Olimar's controller.
    if (id == JACSYS_ContainerOK) {
        Jac_PlayOrimaSe(JACORIMA_Unk14);
        return;
    }
    if (init_jaudio_backend()) {
        pikmin_jaudio_synth_write_core_port(0x10009, 0, static_cast<u16>(id));
        tick_jaudio();
        return;
    }
    const float base = 420.0f + static_cast<float>(static_cast<u32>(id) & 0x1f) * 11.0f;
    queue_tone(base, base * 0.92f, 0.045f, 0.08f * g_seVolume);
}

void Jac_SceneExit(u32, u32)
{
    stop_stx();
    pikmin_jaudio_synth_stop_demo_bgm();
}

void Jac_SceneSetup(u32 scene, u32 stage)
{
    if (scene == SCENE_ChalSelect) g_challengeMode = true;
    else if (scene == SCENE_Title || scene == SCENE_WorldMap || scene == SCENE_FileSelect) g_challengeMode = false;

    g_currentScene = scene;
    g_currentStage = stage;
    const u32 nextBgm = scene_bgm_id(scene, stage);

    if (init_jaudio_backend() && nextBgm != g_currentBgm) {
        if (nextBgm == BGM_PikiSE || nextBgm == BGM_SysEvent) {
            pikmin_jaudio_synth_stop_bgm();
        } else {
            play_bgm_id(nextBgm);
        }
        g_currentBgm = nextBgm;
    }
    OSReport("[pikmin::audio] scene setup scene=%u stage=%u bgm=%u (%s)\n",
             scene, stage, nextBgm, bgm_sequence_name(nextBgm) ? bgm_sequence_name(nextBgm) : "none");
}

void Jac_SetBGMVolume(u8 v)
{
    g_bgmVolume = std::clamp(static_cast<float>(v) / 10.0f, 0.0f, 1.0f);
    pikmin_jaudio_synth_set_bgm_volume(g_bgmVolume);
}

void Jac_SetDemoOnyons(int) {}
void Jac_SetDemoPartsCount(int) {}
void Jac_SetDemoPartsID(int) {}

void Jac_SetSEVolume(u8 v)
{
    g_seVolume = std::clamp(static_cast<float>(v) / 10.0f, 0.0f, 1.0f);
    pikmin_jaudio_synth_set_se_volume(g_seVolume);
}

void Jac_Start(void*, u32, u32, const char* root)
{
    if (root && *root) {
        const size_t len = std::min(strlen(root), sizeof(g_audioRoot) - 1);
        memcpy(g_audioRoot, root, len);
        g_audioRoot[len] = '\0';
    }
    ensure_audio_policy();
    const bool jaudio = init_jaudio_backend();
    OSReport("[pikmin::audio] native SDL3 backend armed; root=%s STX=real JAudio=%s placeholder-tones=%s\n",
             g_audioRoot, jaudio ? "BMS/.bx/.aw" : "unavailable", g_placeholderTones ? "on" : "off");
    tick_jaudio();
}

void Jac_StartDemo(u32 id)
{
    OSReport("[pikmin::audio] demo cue=%u\n", id);
    if (g_jaudioReady) pikmin_jaudio_synth_write_core_port(0x1000f, 0, static_cast<u16>(id));
    start_demo_audio(id);
}

void Jac_StartPartsFindDemo(u32 id, BOOL hasAudio)
{
    // These jingles are ordinary system-SE commands in the retail JAudio code.
    if (hasAudio) Jac_PlaySystemSe(id == 0 ? JACSYS_Unk36 : JACSYS_Unk30);
    else Jac_PlaySystemSe(JACSYS_Unk31);
}

void Jac_StartTextDemo(int) {}

BOOL Jac_StopEventAction(int eventIdx, int actionId)
{
    if (eventIdx < 0 || eventIdx >= static_cast<int>(std::size(g_hostEvents))) return FALSE;
    auto& event = g_hostEvents[eventIdx];
    if (!event.active) return FALSE;
    bool stopped = false;
    for (int slot = 0; slot < 16; ++slot) {
        if (event.actionForSlot[slot] != actionId) continue;
        if (g_jaudioReady) {
            pikmin_jaudio_synth_write_core_child_port(
                0x20000, static_cast<u8>(eventIdx), 0, static_cast<u16>(slot << 12));
        }
        event.actionForSlot[slot] = -1;
        stopped = true;
    }
    if (stopped) tick_jaudio();
    return TRUE;
}

void Jac_StopOrimaSe(s32 id)
{
    if ((id & JACORIMA_PIKISOUND) || !init_jaudio_backend()) return;
    pikmin_jaudio_synth_write_core_port(0x1000a, 2, static_cast<u16>(id));
    tick_jaudio();
}

void Jac_StopSe(s32 id)
{
    Jac_StopSystemSe(id);
}

void Jac_StopSystemSe(s32 id)
{
    if (id == JACSYS_ContainerOK) {
        Jac_StopOrimaSe(JACORIMA_Unk14);
        return;
    }
    if (!init_jaudio_backend()) return;
    pikmin_jaudio_synth_write_core_port(0x10009, 1, static_cast<u16>(id));
    tick_jaudio();
}

int Jac_StreamMovieGetPicture(void*, int*, int*) { return 0; }
void Jac_StreamMovieStop(void) { stop_stx(); }
void Jac_StreamMovieUpdate(void) { tick_stx(); tick_jaudio(); }
void Jac_UpdateCamera(SVector_*, SVector_*) {}
BOOL Jac_UpdateEventPosition(int idx, SVector_*)
{
    return idx >= 0 && idx < static_cast<int>(std::size(g_hostEvents)) && g_hostEvents[idx].active ? TRUE : FALSE;
}

// A small host-side subset of piki_bgm.c. The scene manager above is the main
// owner, but these entry points are also called directly by gameplay code.
void Jac_InitBgm(void) { g_currentBgm = BGM_PikiSE; }
void Jac_FadeOutBgm(u32, u32) { pikmin_jaudio_synth_stop_bgm(); }
void Jac_StopBgm(u32) { pikmin_jaudio_synth_stop_bgm(); }
void Jac_ReadyBgm(u32) {}
void Jac_PlayBgm(u32, u32 bgmID)
{
    if (play_bgm_id(bgmID)) g_currentBgm = bgmID;
}
BOOL Jac_ChangeBgmMode(u32, u8) { return TRUE; }
void Jac_SetBgmModeFlag(u32, u8, u8) {}
void Jac_BgmFrameWork(void) { tick_jaudio(); }
void Jac_MoveBgmTrackVol(BgmControl_*) {}
void Jac_ChangeBgmTrackVol(BgmControl_*) {}
void Jac_GameVolume(u8 bgmLevel, u8 seLevel) { Jac_SetBGMVolume(bgmLevel); Jac_SetSEVolume(seLevel); }
void Jac_EasyCrossFade(u8, u32) {}
void Jac_DemoFade(u8, u32, f32) {}
}
