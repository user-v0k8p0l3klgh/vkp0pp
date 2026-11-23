// The per-frame control loop, independent of DXGI so it can be simulated.
//
// The DXGI layer calls on_present() right after the game's Present returned.
// That point is the boundary between two frames of the game loop, so every
// wait performed here delays the *start* of the next frame (and therefore the
// game's next input poll) rather than the delivery of a finished frame.
//
// Gate order:
//   1. frame-latency waitable object  - a presentation queue slot is free
//   2. GPU sync                       - the previous frame left the GPU
//   3. frame pacer deadline           - frame cap
//   4. just-in-time delay             - start as late as the refresh allows
#pragma once
#include <atomic>

#include "core/config.h"
#include "metrics/frame_record.h"
#include "timing/adaptive_scheduler.h"
#include "timing/bottleneck.h"
#include "timing/frame_pacer.h"
#include "timing/precise_sleeper.h"

namespace bll {

// Presentation-device services needed by the gate; faked in tests.
struct GateDevice {
    enum class Wait { Signaled, Timeout, Unavailable };
    virtual ~GateDevice() = default;
    // Waits for the swap chain's frame-latency waitable object.
    virtual Wait wait_frame_latency(Ticks timeout) = 0;
    // True while the previously submitted frame has not finished on the GPU.
    // Must return false when this cannot be determined.
    virtual bool gpu_frame_pending() = 0;
};

struct PresentSample {
    Ticks call = 0;
    Ticks ret = 0;
    uint32_t sync_interval = 0;
    uint32_t flags = 0;
    int32_t result = 0;
    // False for DXGI_PRESENT_TEST, failed or occluded presents: they are
    // recorded but never gated.
    bool displayed = true;
};

class FrameGovernor {
public:
    FrameGovernor(const Config& cfg, WaitBackend& backend);

    // Render thread only.
    const FrameRecord& on_present(GateDevice& device, const PresentSample& sample);

    // Any thread: the game polled a gamepad at time `t`.
    void note_input_poll(Ticks t) {
        last_poll_.store(t, std::memory_order_relaxed);
        poll_count_.fetch_add(1, std::memory_order_release);
    }

    // Render thread: refresh period estimate, 0 if unknown.
    void set_refresh_period(Ticks period);
    // Render thread: feature switches can be withdrawn at run time (safe mode).
    void set_waitable_enabled(bool on) { waitable_ = on; }
    void set_gpu_sync_enabled(bool on) { gpu_sync_ = on; }

    Bottleneck bottleneck() const { return detector_.current(); }
    const AdaptiveScheduler& scheduler() const { return scheduler_; }
    const PreciseSleeper& sleeper() const { return sleeper_; }
    Ticks pacer_interval() const { return pacer_.interval(); }

private:
    void update_pacer_interval();
    Ticks gate(GateDevice& device, FrameRecord& rec, bool refresh_locked);

    WaitBackend& backend_;
    PreciseSleeper sleeper_;
    FramePacer pacer_;
    AdaptiveScheduler scheduler_;
    BottleneckDetector detector_;
    FrameRecord record_;

    bool waitable_;
    bool gpu_sync_;
    bool pacing_;
    bool adaptive_;
    double target_fps_;

    Ticks refresh_period_ = 0;
    Ticks frame_start_ = 0;
    Ticks prev_call_ = 0;
    Ticks prev_jit_ = 0;
    uint64_t frames_ = 0;
    int latency_timeouts_ = 0;
    int latency_bypass_frames_ = 0;

    std::atomic<Ticks> last_poll_{0};
    std::atomic<uint32_t> poll_count_{0};
    uint32_t prev_poll_count_ = 0;
};

} // namespace bll
