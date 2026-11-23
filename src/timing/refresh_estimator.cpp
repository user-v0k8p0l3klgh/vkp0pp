#include "timing/refresh_estimator.h"

namespace bll {

void RefreshEstimator::seed(Ticks period) {
    if (!measured_ && period > 0) period_ = period;
}

void RefreshEstimator::observe(uint32_t count, Ticks time) {
    if (have_last_ && count != last_count_ && time > last_time_) {
        const uint32_t dc = count - last_count_; // unsigned, so counter wrap is harmless
        const Ticks per = (time - last_time_) / static_cast<Ticks>(dc);
        // Accept 20 Hz..1000 Hz; anything else is a statistics glitch (mode switch, suspend).
        if (dc < 1000 && per >= kNsPerMs && per <= 50 * kNsPerMs) {
            periods_.push(per);
            if (periods_.size() >= 4) {
                period_ = periods_.percentile(50.0);
                measured_ = true;
            }
        } else {
            periods_.clear();
        }
    }
    if (!have_last_ || count != last_count_) {
        last_count_ = count;
        last_time_ = time;
        have_last_ = true;
    }
}

} // namespace bll
