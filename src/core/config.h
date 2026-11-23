// Configuration model and INI parser. Every key maps to implemented
// behaviour; unknown keys are reported as warnings and otherwise ignored.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace bll {

struct Config {
    // [General]
    bool enabled = true;
    bool logging = true;
    // Any unexpected API result permanently reverts the affected feature to pass-through.
    bool safe_mode = true;
    // Behaviour-changing features stay off on builds other than the 1.16.100 release unless set.
    bool allow_unrecognized_build = false;
    int mode = 2;

    // [Rendering]
    bool low_latency_queue = false;   // IDXGIDevice1::SetMaximumFrameLatency
    int target_queue_depth = 1;       // 1..16
    bool waitable_swap_chain = false; // FRAME_LATENCY_WAITABLE_OBJECT + wait at frame start
    bool allow_tearing = false;       // ALLOW_TEARING when presenting with interval 0
    bool disable_vsync = false;       // force sync interval 0
    bool frame_pacing = false;
    double target_fps = 0.0;          // 0 = off, >0 = cap, <0 = refresh rate minus |value|
    int spin_max_us = 1500;           // upper bound of the busy-wait tail, 0 = never spin

    // [Input]
    bool optimize_gamepad = true;           // install gamepad hooks (needed for input metrics)
    bool buffer_button_transitions = false; // sampler thread + transition latch
    int sampler_hz = 1000;                  // 125..2000
    double trigger_press_threshold = 0.5;
    double trigger_release_threshold = 0.3;

    // [Experimental]
    bool adaptive_cpu_gpu_scheduling = false; // just-in-time frame start
    bool gpu_sync = false;                    // do not start a frame while the previous one is still on the GPU
    int jit_margin_us = 2000;

    // [Diagnostics]
    bool record_frame_timings = false; // per-frame CSV
    bool record_input_timing = true;   // input-age statistics in the periodic summary
    int summary_interval_s = 10;
};

// Applies one of the documented presets (0..5) on top of `cfg`.
void apply_mode(Config& cfg, int mode);

// Parses INI text. The preset selected by [General] Mode is applied first and
// explicit keys override it regardless of their position in the file.
Config parse_config(std::string_view ini, std::vector<std::string>* warnings = nullptr);

// Human-readable dump of the effective configuration for the log.
std::string describe(const Config& cfg);

// Default file content written by the launcher on first run.
extern const char* const kDefaultConfigText;

} // namespace bll
