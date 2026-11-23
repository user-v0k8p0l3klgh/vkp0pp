// One gamepad as seen through a hook: owns the transition latch and
// serialises the sampler thread and the game's own polls.
//
// `Reader` performs the real device read (the original, un-hooked API). The
// read happens inside the lock so that the order in which samples reach the
// latch is the order in which they were taken from the device; the critical
// section is one device read (microseconds) and is only ever contended by the
// sampler thread.
#pragma once
#include <atomic>
#include <mutex>

#include "input/transition_latch.h"

namespace bll {

class LatchedPad {
public:
    LatchedPad(double trigger_press, double trigger_release) : latch_(trigger_press, trigger_release) {}

    // Sampler thread. `read` returns false when the device is unavailable.
    template <typename Reader>
    void sample(Reader&& read) {
        std::lock_guard lock(mutex_);
        PadState s;
        if (read(s)) {
            latch_.observe(s);
        } else {
            latch_.reset();
        }
    }

    // Game thread. Returns false (and leaves `out` untouched) if the read failed.
    template <typename Reader>
    bool poll(Reader&& read, Ticks now, PadState& out) {
        std::lock_guard lock(mutex_);
        PadState s;
        if (!read(s)) {
            latch_.reset();
            return false;
        }
        latch_.observe(s);
        out = latch_.consume(now);
        last_game_poll_.store(now, std::memory_order_relaxed);
        return true;
    }

    // Lets the sampler idle while the game is not polling (paused, suspended).
    Ticks last_game_poll() const { return last_game_poll_.load(std::memory_order_relaxed); }

    TransitionLatch::Stats stats() {
        std::lock_guard lock(mutex_);
        return latch_.stats();
    }

private:
    std::mutex mutex_;
    TransitionLatch latch_;
    std::atomic<Ticks> last_game_poll_{0};
};

} // namespace bll
