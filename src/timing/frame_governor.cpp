#include "timing/frame_governor.h"

#include <algorithm>

namespace bll {

namespace {
// A waitable object that stops signalling (occluded or suspended window) must
// not turn into a per-frame stall, so waits are bounded and then bypassed.
constexpr Ticks kLatencyTimeoutMin = 50 * kNsPerMs;
constexpr int kLatencyTimeoutsBeforeBypass = 3;
constexpr int kLatencyBypassFrames = 240;
constexpr Ticks kGpuPollStep = 250 * kNsPerUs;
constexpr Ticks kGpuWaitMax = 30 * kNsPerMs;
} // namespace

FrameGovernor::FrameGovernor(const Config& cfg, WaitBackend& backend)
    : backend_(backend),
      sleeper_(backend, static_cast<Ticks>(cfg.spin_max_us) * kNsPerUs),
      scheduler_(static_cast<Ticks>(cfg.jit_margin_us) * kNsPerUs),
      waitable_(cfg.waitable_swap_chain),
      gpu_sync_(cfg.gpu_sync),
      pacing_(cfg.frame_pacing),
      adaptive_(cfg.adaptive_cpu_gpu_scheduling),
      target_fps_(cfg.target_fps) {
    update_pacer_interval();
}

void FrameGovernor::set_refresh_period(Ticks period) {
    if (period == refresh_period_) return;
    refresh_period_ = period;
    update_pacer_interval();
}

void FrameGovernor::update_pacer_interval() {
    double fps = 0.0;
    if (pacing_) {
        if (target_fps_ > 0.0) {
            fps = target_fps_;
        } else if (target_fps_ < 0.0 && refresh_period_ > 0) {
            // Relative target: refresh rate minus |target|, never below half the refresh rate.
            const double refresh = 1e9 / static_cast<double>(refresh_period_);
            fps = std::max(refresh + target_fps_, refresh / 2.0);
        }
    }
    pacer_.set_interval(fps > 0.0 ? static_cast<Ticks>(1e9 / fps) : 0);
}

const FrameRecord& FrameGovernor::on_present(GateDevice& device, const PresentSample& s) {
    FrameRecord& rec = record_;
    rec = FrameRecord{};
    rec.frame_start = frame_start_;
    rec.present_call = s.call;
    rec.present_return = s.ret;
    rec.sync_interval = s.sync_interval;
    rec.present_flags = s.flags;
    rec.result = s.result;

    const uint32_t polls = poll_count_.load(std::memory_order_acquire);
    rec.input_polls = polls - prev_poll_count_;
    prev_poll_count_ = polls;
    if (polls != 0) {
        // A poll racing with Present on another thread counts as age zero.
        rec.input_age = std::max<Ticks>(s.call - last_poll_.load(std::memory_order_relaxed), 0);
    }

    if (!s.displayed) return rec;

    rec.index = ++frames_;
    rec.interval = prev_call_ ? s.call - prev_call_ : 0;
    prev_call_ = s.call;

    const bool vsync = s.sync_interval >= 1;
    const bool refresh_locked = refresh_period_ > 0 && (vsync || waitable_);

    if (adaptive_ && frame_start_ != 0) {
        // A frame that took more than 1.5 refreshes while we were delaying its
        // start is treated as a miss caused by the delay.
        rec.missed_refresh =
            refresh_locked && prev_jit_ > 0 && rec.interval > refresh_period_ + refresh_period_ / 2;
        scheduler_.on_frame(s.call - frame_start_, refresh_locked ? refresh_period_ : 0, rec.missed_refresh);
    }

    frame_start_ = gate(device, rec, refresh_locked);

    FrameTiming timing;
    timing.interval = rec.interval;
    timing.present_block = s.ret - s.call;
    timing.wait_latency = rec.wait_latency;
    timing.wait_gpu = rec.wait_gpu;
    timing.wait_pacer = rec.wait_pacer;
    timing.gpu_pending = rec.gpu_pending;
    timing.vsync = vsync;
    rec.frame_class = classify_frame(timing);
    detector_.add(timing);
    return rec;
}

Ticks FrameGovernor::gate(GateDevice& device, FrameRecord& rec, bool refresh_locked) {
    Ticks now = rec.present_return;

    if (waitable_) {
        if (latency_bypass_frames_ > 0) {
            --latency_bypass_frames_;
        } else {
            const Ticks timeout = std::max(kLatencyTimeoutMin, 4 * refresh_period_);
            const GateDevice::Wait r = device.wait_frame_latency(timeout);
            const Ticks after = backend_.now();
            if (r == GateDevice::Wait::Signaled) {
                latency_timeouts_ = 0;
                rec.wait_latency = after - now;
            } else if (r == GateDevice::Wait::Timeout && ++latency_timeouts_ >= kLatencyTimeoutsBeforeBypass) {
                latency_timeouts_ = 0;
                latency_bypass_frames_ = kLatencyBypassFrames;
            }
            now = after;
        }
    }
    // When the latency wait blocked it ended at a vblank; otherwise the end of
    // Present is the best available frame opportunity.
    const Ticks opportunity = now;

    if (gpu_sync_) {
        rec.gpu_pending = device.gpu_frame_pending();
        if (rec.gpu_pending) {
            const Ticks begin = now;
            const Ticks limit = begin + kGpuWaitMax;
            do {
                backend_.sleep_until(now + kGpuPollStep);
                now = backend_.now();
            } while (now < limit && device.gpu_frame_pending());
            rec.wait_gpu = now - begin;
        }
    }

    const Ticks pacer_deadline = pacer_.next_start(now);
    Ticks jit_deadline = 0;
    if (adaptive_ && refresh_locked) jit_deadline = opportunity + scheduler_.delay();
    prev_jit_ = 0;

    const Ticks deadline = std::max(pacer_deadline, jit_deadline);
    if (deadline > now) {
        const Ticks end = sleeper_.wait_until(deadline);
        // Attribute the wait: the pacer first, the remainder to the JIT delay.
        const Ticks pacer_part = std::clamp<Ticks>(pacer_deadline - now, 0, end - now);
        rec.wait_pacer = pacer_part;
        rec.wait_jit = jit_deadline > pacer_deadline ? end - now - pacer_part : 0;
        prev_jit_ = rec.wait_jit;
        now = end;
    }
    return now;
}

} // namespace bll
