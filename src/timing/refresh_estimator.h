// Estimates the display refresh period from DXGI frame statistics
// (SyncRefreshCount, SyncQPCTime) pairs, so no refresh rate is hard-coded.
#pragma once
#include <cstdint>

#include "core/stats.h"
#include "core/time.h"

namespace bll {

class RefreshEstimator {
public:
    // One observation of the vblank counter and the time of that vblank.
    void observe(uint32_t sync_refresh_count, Ticks sync_time);
    // Fallback until measurements exist, e.g. from the output's display mode.
    void seed(Ticks period);

    // 0 while unknown.
    Ticks period() const { return period_; }
    bool measured() const { return measured_; }

private:
    RollingWindow<Ticks, 32> periods_;
    uint32_t last_count_ = 0;
    Ticks last_time_ = 0;
    Ticks period_ = 0;
    bool have_last_ = false;
    bool measured_ = false;
};

} // namespace bll
