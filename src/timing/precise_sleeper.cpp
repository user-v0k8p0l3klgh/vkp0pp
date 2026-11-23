#include "timing/precise_sleeper.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <immintrin.h>
#else
#include <chrono>
#include <thread>
#endif

namespace bll {

namespace {
constexpr Ticks kMinMargin = 100 * kNsPerUs;
constexpr Ticks kSafety = 50 * kNsPerUs;
} // namespace

SpinMarginEstimator::SpinMarginEstimator(Ticks min_margin, Ticks max_margin)
    : min_(std::min(min_margin, max_margin)), max_(max_margin), margin_(max_margin) {}

void SpinMarginEstimator::record_overshoot(Ticks late_by) {
    samples_.push(std::max<Ticks>(late_by, 0));
    // Re-evaluating every few samples keeps this off the per-frame cost.
    if (++since_update_ < 8) return;
    since_update_ = 0;
    const Ticks p99 = samples_.percentile(99.0);
    margin_ = std::clamp(p99 + p99 / 10 + kSafety, min_, max_);
}

PreciseSleeper::PreciseSleeper(WaitBackend& backend, Ticks max_spin)
    : backend_(backend), max_spin_(std::max<Ticks>(max_spin, 0)), estimator_(kMinMargin, max_spin_) {}

Ticks PreciseSleeper::wait_until(Ticks deadline) {
    Ticks now = backend_.now();
    if (now >= deadline) return now;

    const Ticks margin = max_spin_ > 0 ? estimator_.margin() : 0;
    if (deadline - now > margin) {
        const Ticks wake = deadline - margin;
        const Ticks before = now;
        backend_.sleep_until(wake);
        now = backend_.now();
        total_sleep_ += now - before;
        estimator_.record_overshoot(now - wake);
    }
    if (max_spin_ > 0 && now < deadline) {
        const Ticks before = now;
        while ((now = backend_.now()) < deadline) backend_.pause();
        total_spin_ += now - before;
    }
    return now;
}

namespace {

#ifdef _WIN32
class WinWaitBackend final : public WaitBackend {
public:
    WinWaitBackend() {
        // High-resolution timers need Windows 10 1803; fall back to a normal one.
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~WinWaitBackend() override {
        if (timer_) CloseHandle(timer_);
    }
    Ticks now() override { return now_ns(); }
    void sleep_until(Ticks wake) override {
        const Ticks left = wake - now_ns();
        if (left <= 0) return;
        if (!timer_) {
            Sleep(static_cast<DWORD>(left / kNsPerMs));
            return;
        }
        LARGE_INTEGER due;
        due.QuadPart = -(left / 100); // relative, 100 ns units
        if (due.QuadPart == 0) return;
        // The wait is bounded so a misbehaving timer can never stall the caller.
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer_, static_cast<DWORD>(left / kNsPerMs) + 2);
    }
    void pause() override { _mm_pause(); }

private:
    HANDLE timer_ = nullptr;
};
#else
class StdWaitBackend final : public WaitBackend {
public:
    Ticks now() override { return now_ns(); }
    void sleep_until(Ticks wake) override {
        const Ticks left = wake - now_ns();
        if (left > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(left));
    }
    void pause() override { std::this_thread::yield(); }
};
#endif

} // namespace

WaitBackend& system_wait_backend() {
#ifdef _WIN32
    static thread_local WinWaitBackend backend; // one timer handle per waiting thread
#else
    static StdWaitBackend backend;
#endif
    return backend;
}

} // namespace bll
