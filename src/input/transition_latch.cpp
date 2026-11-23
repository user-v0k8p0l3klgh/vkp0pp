#include "input/transition_latch.h"

#include <algorithm>
#include <bit>

namespace bll {

namespace {
constexpr int kLeftTrigger = 32;
constexpr int kRightTrigger = 33;
constexpr uint64_t bit(int channel) { return uint64_t{1} << channel; }
} // namespace

TransitionLatch::TransitionLatch(double trigger_press, double trigger_release)
    : press_(trigger_press), release_(trigger_release) {}

void TransitionLatch::reset() {
    last_ = PadState{};
    actual_ = reported_ = 0;
    pending_.fill(0);
    peak_.fill(0.0);
    last_consume_ = 0;
}

void TransitionLatch::toggle(int channel) {
    actual_ ^= bit(channel);
    uint8_t& p = pending_[static_cast<size_t>(channel)];
    if (p >= kMaxPending) {
        // Dropping two transitions keeps the parity, so the replay still ends
        // on the physical state.
        p -= 1;
        stats_.collapsed += 2;
    } else {
        p += 1;
    }
}

void TransitionLatch::observe(const PadState& s) {
    uint32_t changed = s.buttons ^ last_.buttons;
    while (changed) {
        const int b = std::countr_zero(changed);
        changed &= changed - 1;
        toggle(b);
    }

    const double triggers[2] = {s.left_trigger, s.right_trigger};
    for (int i = 0; i < 2; ++i) {
        const int channel = kLeftTrigger + i;
        const bool down = (actual_ & bit(channel)) != 0;
        // Hysteresis: press above the press threshold, release below the release threshold.
        if (!down && triggers[i] >= press_) toggle(channel);
        else if (down && triggers[i] <= release_) toggle(channel);
        peak_[static_cast<size_t>(i)] = std::max(peak_[static_cast<size_t>(i)], triggers[i]);
    }
    last_ = s;
}

PadState TransitionLatch::consume(Ticks now) {
    if (last_consume_ != 0 && now - last_consume_ > kStaleAfter) {
        // Jump straight to the physical state instead of replaying old taps.
        if (std::any_of(pending_.begin(), pending_.end(), [](uint8_t p) { return p > 1; })) ++stats_.resets;
        pending_.fill(0);
        reported_ = actual_;
    }
    last_consume_ = now;

    for (int c = 0; c < kChannels; ++c) {
        uint8_t& p = pending_[static_cast<size_t>(c)];
        if (p == 0) continue;
        reported_ ^= bit(c);
        --p;
        if (p > 0) ++stats_.replayed;
    }

    PadState out = last_; // newest analog values, never buffered
    out.buttons = static_cast<uint32_t>(reported_ & 0xFFFFFFFFu);

    double* const values[2] = {&out.left_trigger, &out.right_trigger};
    for (int i = 0; i < 2; ++i) {
        const bool report_down = (reported_ & bit(kLeftTrigger + i)) != 0;
        const bool actual_down = (actual_ & bit(kLeftTrigger + i)) != 0;
        if (report_down && !actual_down) {
            // Replaying a pull that has already been released: report its peak.
            *values[i] = std::max(peak_[static_cast<size_t>(i)], press_);
        } else if (!report_down && actual_down) {
            // Replaying a release between two pulls.
            *values[i] = 0.0;
        }
        // The next window's peak starts from the current level.
        peak_[static_cast<size_t>(i)] = i == 0 ? last_.left_trigger : last_.right_trigger;
    }
    return out;
}

} // namespace bll
