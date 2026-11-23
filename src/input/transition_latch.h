// Preserves digital transitions that happen between two polls by the game.
//
// A polled gamepad API only exposes the current state, so a press and release
// that both fall between two game polls are invisible to the game. A sampler
// thread polling faster than the game feeds every sample into observe(); the
// game's own poll then calls consume(), which replays at most one pending
// transition per button per poll:
//
//   - a normal press/release is reported on the very next game poll, exactly
//     as without the latch (no added latency);
//   - a tap that fell between two polls is reported as pressed for one poll
//     and released on the following one (not lost);
//   - a held button produces no transitions, so nothing is ever duplicated;
//   - once the pending transitions are drained the reported state equals the
//     physical state, so buttons cannot stick.
//
// Triggers are analog but act as buttons in game (attack / use), so they get
// the same treatment through a hysteresis threshold. Sticks are never
// buffered: the game always receives the newest analog values.
//
// The latch cannot recover transitions the sampler itself did not see; its
// resolution is the sampler rate and ultimately the controller's report rate.
//
// Not thread-safe: the caller serialises observe() and consume() and must
// call them in the order the underlying device reads happened.
#pragma once
#include <array>
#include <cstdint>

#include "core/time.h"
#include "input/pad_state.h"

namespace bll {

class TransitionLatch {
public:
    struct Stats {
        uint64_t replayed = 0;  // transitions delivered one poll late to preserve a tap
        uint64_t collapsed = 0; // transitions dropped because more than kMaxPending queued up
        uint64_t resets = 0;    // pending state discarded after a consumer stall
    };

    TransitionLatch(double trigger_press, double trigger_release);

    // A device sample, in time order.
    void observe(const PadState& state);

    // The game's poll. Must directly follow observe() of the sample the game
    // just read. Returns the state to hand to the game.
    PadState consume(Ticks now);

    // Device disconnected or focus lost: forget everything.
    void reset();

    const Stats& stats() const { return stats_; }

private:
    static constexpr int kButtons = 32;
    static constexpr int kChannels = kButtons + 2; // + left/right trigger
    static constexpr uint8_t kMaxPending = 3;
    // If the game has not polled for this long, pending transitions are stale
    // (pause, focus loss) and replaying them would produce phantom input.
    static constexpr Ticks kStaleAfter = 250 * kNsPerMs;

    void toggle(int channel);

    double press_, release_;
    PadState last_{};
    uint64_t actual_ = 0;   // physical state per channel (bit = down)
    uint64_t reported_ = 0; // state last handed to the game per channel
    std::array<uint8_t, kChannels> pending_{};
    std::array<double, 2> peak_{};
    Ticks last_consume_ = 0;
    Stats stats_;
};

} // namespace bll
