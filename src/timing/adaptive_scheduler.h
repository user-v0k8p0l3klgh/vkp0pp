// Just-in-time frame start for refresh-locked presentation.
//
// When frames are released once per display refresh, starting the frame the
// moment the queue slot frees means the input sampled for it is a full refresh
// period old when it is shown. Delaying the start by
//     period - predicted CPU work - margin
// shows the same frame at the same vblank with fresher input.
//
// The CPU work is predicted as a smoothed mean plus a multiple of the smoothed
// deviation. What cannot be observed from the Present call (GPU time, the
// compositor's deadline, timer error) is covered by a margin that is *learned*:
// every missed refresh widens it, and it narrows again so slowly that the
// steady-state miss rate settles near kTargetMissRate. If the game cannot
// keep that rate with any delay, the margin consumes the whole budget and the
// scheduler becomes inert.
#pragma once
#include <cstdint>

#include "core/time.h"

namespace bll {

class AdaptiveScheduler {
public:
    // base_margin: minimum safety margin, the learned part is added on top.
    explicit AdaptiveScheduler(Ticks base_margin);

    // work: time the frame needed from its start to the Present call.
    // period: refresh period (0 if unknown, which disables the delay).
    // missed: the frame did not make the refresh it was aimed at.
    void on_frame(Ticks work, Ticks period, bool missed);

    // Delay to insert between the frame opportunity and the frame start.
    Ticks delay() const { return delay_; }

    double scale() const { return scale_; }
    uint64_t misses() const { return misses_; }
    Ticks learned_margin() const { return learned_; }
    Ticks predicted_work() const { return mean_ + kDeviations * dev_; }

    void reset();

    // Roughly one repeated refresh per thousand frames is accepted in exchange
    // for running close to the deadline.
    static constexpr double kTargetMissRate = 0.001;

private:
    static constexpr int kDeviations = 4;
    static constexpr int kCooldownFrames = 60;
    static constexpr double kRecoveryStep = 0.01;
    static constexpr double kMaxFraction = 0.8;

    Ticks base_margin_;
    Ticks learned_ = 0;
    Ticks mean_ = 0;
    Ticks dev_ = 0;
    Ticks delay_ = 0;
    // Starts at zero: the delay is earned by clean frames, never assumed.
    double scale_ = 0.0;
    int cooldown_ = 0;
    uint64_t misses_ = 0;
    bool primed_ = false;
};

} // namespace bll
