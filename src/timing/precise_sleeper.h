// Hybrid wait: sleep on an OS timer until shortly before the deadline, then
// busy-wait the remainder. The length of the busy-wait tail follows the
// observed lateness of the OS timer instead of being a fixed constant, so the
// CPU cost is only what the timer's real precision requires.
#pragma once
#include "core/stats.h"
#include "core/time.h"

namespace bll {

// OS services used by the sleeper; faked in tests.
struct WaitBackend {
    virtual ~WaitBackend() = default;
    virtual Ticks now() = 0;
    // Blocks until roughly `wake`; may return late (and occasionally early).
    virtual void sleep_until(Ticks wake) = 0;
    // One iteration of a busy-wait loop.
    virtual void pause() = 0;
};

// Tracks how late timer wake-ups are and derives the spin margin from it.
class SpinMarginEstimator {
public:
    SpinMarginEstimator(Ticks min_margin, Ticks max_margin);
    void record_overshoot(Ticks late_by);
    Ticks margin() const { return margin_; }

private:
    static constexpr size_t kWindow = 64;
    RollingWindow<Ticks, kWindow> samples_;
    Ticks min_, max_, margin_;
    size_t since_update_ = 0;
};

class PreciseSleeper {
public:
    // max_spin == 0 disables busy-waiting: the wait then ends whenever the OS timer fires.
    PreciseSleeper(WaitBackend& backend, Ticks max_spin);

    // Returns the time at which the wait ended.
    Ticks wait_until(Ticks deadline);

    Ticks margin() const { return estimator_.margin(); }
    // Totals since construction, for reporting the CPU cost of spinning.
    Ticks total_spin() const { return total_spin_; }
    Ticks total_sleep() const { return total_sleep_; }

private:
    WaitBackend& backend_;
    Ticks max_spin_;
    SpinMarginEstimator estimator_;
    Ticks total_spin_ = 0;
    Ticks total_sleep_ = 0;
};

// Real backend: high-resolution waitable timer on Windows, sleep_for elsewhere.
WaitBackend& system_wait_backend();

} // namespace bll
