#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// In-process JAudio/BMS software synthesizer bridge. The implementation wraps
// resource_dasm's smssynth renderer, but audio device ownership stays in
// Pikmin's SDL3 backend so STX and synthesized JAudio can coexist cleanly.
bool pikmin_jaudio_synth_init(const char* audiores_directory);
void pikmin_jaudio_synth_shutdown();

bool pikmin_jaudio_synth_start_core_se();
bool pikmin_jaudio_synth_start_bgm(const char* sequence_name);
bool pikmin_jaudio_synth_start_demo_bgm();
void pikmin_jaudio_synth_stop_bgm();
void pikmin_jaudio_synth_stop_demo_bgm();

void pikmin_jaudio_synth_set_bgm_volume(float volume);
void pikmin_jaudio_synth_set_se_volume(float volume);
bool pikmin_jaudio_synth_has_audio();

// Writes to JAudio application ports. connection_id values are the IDs used by
// Pikmin's original Jam_GetTrackHandle() calls (for example 0x10009 and
// 0x1000A). Writes are queued until the BMS track has consumed the previous
// value, matching the original CmdQueue behaviour closely enough for gameplay
// sound triggers.
void pikmin_jaudio_synth_write_core_port(std::uint32_t connection_id, std::uint8_t port, std::uint16_t value);
void pikmin_jaudio_synth_write_demo_port(std::uint32_t connection_id, std::uint8_t port, std::uint16_t value);
void pikmin_jaudio_synth_write_core_child_port(std::uint32_t connection_id, std::uint8_t child,
                                                std::uint8_t port, std::uint16_t value);

// Produces interleaved F32 stereo at 48 kHz. Returns frames written; the output
// buffer is always initialized (silence if no renderer is active).
std::size_t pikmin_jaudio_synth_render(float* out, std::size_t frames);

#ifdef __cplusplus
}
#endif
