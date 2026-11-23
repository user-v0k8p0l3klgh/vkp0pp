#include "timing/adaptive_scheduler.h"

#include <algorithm>
#include <cstdlib>

namespace bll {

AdaptiveScheduler::AdaptiveScheduler(Ticks base_margin) : base_margin_(base_margin) {}

void AdaptiveScheduler::reset() {
    learned_ = mean_ = dev_ = delay_ = 0;
    scale_ = 0.0;
    cooldown_ = 0;
    primed_ = false;
}

void AdaptiveScheduler::on_frame(Ticks work, Ticks period, bool missed) {
    work = std::max<Ticks>(work, 0);
    if (!primed_) {
        mean_ = work;
        dev_ = work / 2;
        primed_ = true;
    } else {
        const Ticks err = work - mean_;
        mean_ += err / 8;
        dev_ += (std::llabs(err) - dev_) / 4;
    }

    if (period > 0) {
        // Widen by a fixed step per miss and narrow by step * target rate per
        // clean frame: the two balance when misses occur at the target rate.
        const Ticks step = period / 16;
        if (missed) learned_ = std::min(learned_ + step, period);
        else learned_ = std::max<Ticks>(learned_ - static_cast<Ticks>(static_cast<double>(step) * kTargetMissRate), 0);
    }

    if (missed) {
        ++misses_;
        scale_ *= 0.5;
        cooldown_ = kCooldownFrames;
    } else if (cooldown_ > 0) {
        --cooldown_;
    } else {
        scale_ = std::min(1.0, scale_ + kRecoveryStep);
    }

    const Ticks budget = period - predicted_work() - base_margin_ - learned_;
    if (period <= 0 || budget <= 0) {
        delay_ = 0;
        return;
    }
    const auto scaled = static_cast<Ticks>(static_cast<double>(budget) * scale_);
    delay_ = std::min(scaled, static_cast<Ticks>(static_cast<double>(period) * kMaxFraction));
}

} // namespace bll
