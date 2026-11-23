#include "core/config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace bll {

const char* const kDefaultConfigText = R"ini(; BedrockLowLatency configuration. Delete this file to restore the defaults.
; Full reference: docs/USAGE.md

[General]
Enabled=true
EnableLogging=true
; Revert a feature to pass-through for the rest of the session on any unexpected API result.
SafeMode=true
; Behaviour-changing features are only enabled on package version 1.16.100.x unless this is true.
AllowUnrecognizedBuild=false
; 0 vanilla (measure only)   1 v-sync optimisation      2 low-latency queue
; 3 frame pacing             4 just-in-time (experimental) 5 combined
Mode=2

[Rendering]
; Keys left commented out take their value from Mode.
;LowLatencyQueue=true
;TargetQueueDepth=1
;WaitableSwapChain=true
;AllowTearing=false
;DisableVSync=false
;AdaptiveFramePacing=false
; 0 = no cap, N = cap at N FPS, -N = display refresh rate minus N
;TargetFPS=0
; Upper bound of the busy-wait at the end of each timed wait, in microseconds. 0 disables spinning.
;SpinMaxUs=1500

[Input]
OptimizeGamepad=true
;BufferButtonTransitions=false
;SamplerHz=1000
;TriggerPressThreshold=0.5
;TriggerReleaseThreshold=0.3

[Experimental]
;AdaptiveCpuGpuScheduling=false
;GpuSync=false
;JitMarginUs=2000

[Diagnostics]
RecordFrameTimings=false
RecordInputTiming=true
SummaryIntervalSeconds=10
)ini";

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::optional<bool> parse_bool(std::string_view v) {
    const std::string s = lower(v);
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return std::nullopt;
}

std::optional<double> parse_number(std::string_view v) {
    const std::string s(v);
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return std::nullopt;
    return d;
}

struct Entry {
    std::string section, key, value;
    int line;
};

void warn(std::vector<std::string>* w, int line, const std::string& msg) {
    if (w) w->push_back("line " + std::to_string(line) + ": " + msg);
}

} // namespace

void apply_mode(Config& c, int mode) {
    c.mode = mode;
    c.low_latency_queue = c.waitable_swap_chain = false;
    c.allow_tearing = c.disable_vsync = false;
    c.frame_pacing = false;
    c.buffer_button_transitions = false;
    c.adaptive_cpu_gpu_scheduling = c.gpu_sync = false;
    switch (mode) {
    case 1:
        c.allow_tearing = true;
        c.disable_vsync = true;
        break;
    case 2:
        c.low_latency_queue = true;
        c.waitable_swap_chain = true;
        break;
    case 3:
        c.frame_pacing = true;
        break;
    case 4:
        c.low_latency_queue = true;
        c.waitable_swap_chain = true;
        c.adaptive_cpu_gpu_scheduling = true;
        c.gpu_sync = true;
        break;
    case 5:
        c.low_latency_queue = true;
        c.waitable_swap_chain = true;
        c.allow_tearing = true;
        c.frame_pacing = true;
        c.buffer_button_transitions = true;
        break;
    default:
        c.mode = 0;
        break;
    }
}

Config parse_config(std::string_view ini, std::vector<std::string>* warnings) {
    std::vector<Entry> entries;
    std::string section;
    int line_no = 0;
    while (!ini.empty()) {
        const size_t nl = ini.find('\n');
        std::string_view line = trim(ini.substr(0, nl));
        ini.remove_prefix(nl == std::string_view::npos ? ini.size() : nl + 1);
        ++line_no;
        if (line_no == 1 && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            const size_t close = line.find(']');
            if (close == std::string_view::npos) {
                warn(warnings, line_no, "unterminated section header");
                continue;
            }
            section = lower(trim(line.substr(1, close - 1)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            warn(warnings, line_no, "expected key=value");
            continue;
        }
        std::string_view value = line.substr(eq + 1);
        if (const size_t comment = value.find(';'); comment != std::string_view::npos) value = value.substr(0, comment);
        entries.push_back({section, lower(trim(line.substr(0, eq))), std::string(trim(value)), line_no});
    }

    Config cfg;
    int mode = cfg.mode;
    bool fps_explicit = false;
    for (const Entry& e : entries) {
        if (e.section == "general" && e.key == "mode") {
            const auto n = parse_number(e.value);
            if (n && *n >= 0 && *n <= 5 && *n == static_cast<int>(*n)) mode = static_cast<int>(*n);
            else warn(warnings, e.line, "Mode must be an integer from 0 to 5");
        }
    }
    apply_mode(cfg, mode);

    for (const Entry& e : entries) {
        const std::string id = e.section + "." + e.key;
        if (id == "general.mode") continue;

        auto set_bool = [&](bool& target) {
            if (const auto b = parse_bool(e.value)) target = *b;
            else warn(warnings, e.line, "'" + e.value + "' is not a boolean");
        };
        auto set_int = [&](int& target, int lo, int hi) {
            const auto n = parse_number(e.value);
            if (!n) return warn(warnings, e.line, "'" + e.value + "' is not a number");
            const int clamped = static_cast<int>(std::clamp(*n, static_cast<double>(lo), static_cast<double>(hi)));
            if (clamped != *n) warn(warnings, e.line, e.key + " clamped to " + std::to_string(clamped));
            target = clamped;
        };
        auto set_double = [&](double& target, double lo, double hi) {
            const auto n = parse_number(e.value);
            if (!n) return warn(warnings, e.line, "'" + e.value + "' is not a number");
            target = std::clamp(*n, lo, hi);
            if (target != *n) warn(warnings, e.line, e.key + " clamped");
        };

        if (id == "general.enabled") set_bool(cfg.enabled);
        else if (id == "general.enablelogging") set_bool(cfg.logging);
        else if (id == "general.safemode") set_bool(cfg.safe_mode);
        else if (id == "general.allowunrecognizedbuild") set_bool(cfg.allow_unrecognized_build);
        else if (id == "rendering.lowlatencyqueue") set_bool(cfg.low_latency_queue);
        else if (id == "rendering.targetqueuedepth") set_int(cfg.target_queue_depth, 1, 16);
        else if (id == "rendering.waitableswapchain") set_bool(cfg.waitable_swap_chain);
        else if (id == "rendering.allowtearing") set_bool(cfg.allow_tearing);
        else if (id == "rendering.disablevsync") set_bool(cfg.disable_vsync);
        else if (id == "rendering.adaptiveframepacing") set_bool(cfg.frame_pacing);
        else if (id == "rendering.targetfps") set_double(cfg.target_fps, -60.0, 1000.0), fps_explicit = true;
        else if (id == "rendering.spinmaxus") set_int(cfg.spin_max_us, 0, 4000);
        else if (id == "input.optimizegamepad") set_bool(cfg.optimize_gamepad);
        else if (id == "input.bufferbuttontransitions") set_bool(cfg.buffer_button_transitions);
        else if (id == "input.samplerhz") set_int(cfg.sampler_hz, 125, 2000);
        else if (id == "input.triggerpressthreshold") set_double(cfg.trigger_press_threshold, 0.05, 0.95);
        else if (id == "input.triggerreleasethreshold") set_double(cfg.trigger_release_threshold, 0.01, 0.9);
        else if (id == "experimental.adaptivecpugpuscheduling") set_bool(cfg.adaptive_cpu_gpu_scheduling);
        else if (id == "experimental.gpusync") set_bool(cfg.gpu_sync);
        else if (id == "experimental.jitmarginus") set_int(cfg.jit_margin_us, 250, 8000);
        else if (id == "diagnostics.recordframetimings") set_bool(cfg.record_frame_timings);
        else if (id == "diagnostics.recordinputtiming") set_bool(cfg.record_input_timing);
        else if (id == "diagnostics.summaryintervalseconds") set_int(cfg.summary_interval_s, 1, 600);
        else warn(warnings, e.line, "unknown key [" + e.section + "] " + e.key + " ignored");
    }

    if (cfg.trigger_release_threshold >= cfg.trigger_press_threshold) {
        cfg.trigger_release_threshold = cfg.trigger_press_threshold * 0.6;
        if (warnings) warnings->push_back("TriggerReleaseThreshold must be below TriggerPressThreshold; adjusted");
    }
    if (!cfg.optimize_gamepad && cfg.buffer_button_transitions) {
        cfg.buffer_button_transitions = false;
        if (warnings) warnings->push_back("BufferButtonTransitions needs OptimizeGamepad; disabled");
    }
    // Mode 3 without an explicit target paces slightly below the refresh rate.
    if (cfg.mode == 3 && cfg.frame_pacing && !fps_explicit) cfg.target_fps = -3.0;
    return cfg;
}

std::string describe(const Config& c) {
    char buf[1024];
    std::snprintf(
        buf, sizeof buf,
        "enabled=%d safe_mode=%d allow_unrecognized_build=%d mode=%d | queue=%d depth=%d waitable=%d tearing=%d "
        "disable_vsync=%d pacing=%d target_fps=%.3f spin_max_us=%d | gamepad=%d latch=%d sampler_hz=%d "
        "trigger=%.2f/%.2f | jit=%d gpu_sync=%d jit_margin_us=%d | frame_csv=%d input_stats=%d summary_s=%d",
        c.enabled, c.safe_mode, c.allow_unrecognized_build, c.mode, c.low_latency_queue, c.target_queue_depth,
        c.waitable_swap_chain, c.allow_tearing, c.disable_vsync, c.frame_pacing, c.target_fps, c.spin_max_us,
        c.optimize_gamepad, c.buffer_button_transitions, c.sampler_hz, c.trigger_press_threshold,
        c.trigger_release_threshold, c.adaptive_cpu_gpu_scheduling, c.gpu_sync, c.jit_margin_us,
        c.record_frame_timings, c.record_input_timing, c.summary_interval_s);
    return buf;
}

} // namespace bll
