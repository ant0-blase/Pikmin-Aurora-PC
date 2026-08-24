#include "jaudio_smssynth_bridge.h"

#include <cstring>

extern "C" bool pikmin_jaudio_synth_init(const char*) { return false; }
extern "C" void pikmin_jaudio_synth_shutdown() {}
extern "C" bool pikmin_jaudio_synth_start_core_se() { return false; }
extern "C" bool pikmin_jaudio_synth_start_bgm(const char*) { return false; }
extern "C" bool pikmin_jaudio_synth_start_demo_bgm() { return false; }
extern "C" void pikmin_jaudio_synth_stop_bgm() {}
extern "C" void pikmin_jaudio_synth_stop_demo_bgm() {}
extern "C" void pikmin_jaudio_synth_set_bgm_volume(float) {}
extern "C" void pikmin_jaudio_synth_set_se_volume(float) {}
extern "C" bool pikmin_jaudio_synth_has_audio() { return false; }
extern "C" void pikmin_jaudio_synth_write_core_port(std::uint32_t, std::uint8_t, std::uint16_t) {}
extern "C" void pikmin_jaudio_synth_write_demo_port(std::uint32_t, std::uint8_t, std::uint16_t) {}
extern "C" void pikmin_jaudio_synth_write_core_child_port(std::uint32_t, std::uint8_t, std::uint8_t, std::uint16_t) {}
extern "C" std::size_t pikmin_jaudio_synth_render(float* out, std::size_t frames)
{
    if (out && frames) std::memset(out, 0, frames * 2 * sizeof(float));
    return 0;
}
