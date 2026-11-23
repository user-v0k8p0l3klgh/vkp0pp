#include <atomic>
#include <random>
#include <thread>

#include "input/latched_pad.h"
#include "input/transition_latch.h"
#include "test.h"

using namespace bll;

namespace {

constexpr uint32_t kA = 0x4;
constexpr uint32_t kB = 0x8;
constexpr Ticks kPoll = 16 * kNsPerMs;

PadState buttons(uint32_t mask) {
    PadState s;
    s.buttons = mask;
    return s;
}

// One game poll: the game reads `fresh` from the device, the latch answers.
PadState game_poll(TransitionLatch& latch, const PadState& fresh, Ticks& clock) {
    clock += kPoll;
    latch.observe(fresh);
    return latch.consume(clock);
}

int rising_edges(const std::vector<uint32_t>& seen, uint32_t mask) {
    int n = 0;
    uint32_t prev = 0;
    for (uint32_t s : seen) {
        if ((s & mask) && !(prev & mask)) ++n;
        prev = s;
    }
    return n;
}

} // namespace

TEST(latch_is_transparent_without_missed_transitions) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    // Press, hold, release: reported on the same poll as without the latch.
    CHECK_EQ(game_poll(latch, buttons(0), t).buttons, 0u);
    CHECK_EQ(game_poll(latch, buttons(kA), t).buttons, kA);
    CHECK_EQ(game_poll(latch, buttons(kA), t).buttons, kA);
    CHECK_EQ(game_poll(latch, buttons(kA | kB), t).buttons, kA | kB);
    CHECK_EQ(game_poll(latch, buttons(kB), t).buttons, kB);
    CHECK_EQ(game_poll(latch, buttons(0), t).buttons, 0u);
    CHECK_EQ(latch.stats().replayed, uint64_t{0});
}

TEST(latch_preserves_tap_between_game_polls) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    game_poll(latch, buttons(0), t);
    // The sampler sees a full press and release before the game polls again.
    latch.observe(buttons(kA));
    latch.observe(buttons(0));
    CHECK_EQ(game_poll(latch, buttons(0), t).buttons, kA); // tap delivered
    CHECK_EQ(game_poll(latch, buttons(0), t).buttons, 0u); // and released
    CHECK_EQ(game_poll(latch, buttons(0), t).buttons, 0u); // exactly once
    CHECK_EQ(latch.stats().replayed, uint64_t{1});
}

TEST(latch_preserves_release_between_two_presses) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    CHECK_EQ(game_poll(latch, buttons(kA), t).buttons, kA);
    latch.observe(buttons(0));
    latch.observe(buttons(kA));
    // Still held physically, but the game must see the release to count two presses.
    CHECK_EQ(game_poll(latch, buttons(kA), t).buttons, 0u);
    CHECK_EQ(game_poll(latch, buttons(kA), t).buttons, kA);
    CHECK_EQ(game_poll(latch, buttons(kA), t).buttons, kA);
}

TEST(latch_never_duplicates_a_held_button) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    std::vector<uint32_t> seen;
    seen.push_back(game_poll(latch, buttons(0), t).buttons);
    for (int frame = 0; frame < 200; ++frame) {
        for (int s = 0; s < 16; ++s) latch.observe(buttons(kA)); // sampler keeps seeing "held"
        seen.push_back(game_poll(latch, buttons(kA), t).buttons);
    }
    CHECK_EQ(rising_edges(seen, kA), 1);
}

TEST(latch_buttons_are_independent) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    game_poll(latch, buttons(kB), t);
    latch.observe(buttons(kA | kB));
    latch.observe(buttons(kB));
    // B stays held and untouched while the A tap is replayed.
    CHECK_EQ(game_poll(latch, buttons(kB), t).buttons, kA | kB);
    CHECK_EQ(game_poll(latch, buttons(kB), t).buttons, kB);
}

TEST(latch_caps_pending_transitions_and_converges) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    game_poll(latch, buttons(0), t);
    for (int i = 0; i < 25; ++i) { // 25 taps between two polls: far beyond anything physical
        latch.observe(buttons(kA));
        latch.observe(buttons(0));
    }
    CHECK(latch.stats().collapsed > 0);
    std::vector<uint32_t> seen{0};
    for (int i = 0; i < 8; ++i) seen.push_back(game_poll(latch, buttons(0), t).buttons);
    CHECK_EQ(seen.back(), 0u); // not stuck
    CHECK(rising_edges(seen, kA) >= 1);
    CHECK(rising_edges(seen, kA) <= 2); // bounded replay
}

TEST(latch_discards_stale_transitions_after_a_stall) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    game_poll(latch, buttons(0), t);
    latch.observe(buttons(kA));
    latch.observe(buttons(0));
    t += kNsPerSec; // game paused / unfocused for a second
    CHECK_EQ(game_poll(latch, buttons(0), t).buttons, 0u); // no phantom tap on resume
    CHECK_EQ(latch.stats().resets, uint64_t{1});
    // A button that is physically held on resume is reported as held.
    latch.observe(buttons(kB));
    t += kNsPerSec;
    CHECK_EQ(game_poll(latch, buttons(kB), t).buttons, kB);
}

TEST(latch_reset_clears_everything) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    game_poll(latch, buttons(kA), t);
    latch.observe(buttons(0));
    latch.observe(buttons(kA));
    latch.reset(); // disconnect
    CHECK_EQ(game_poll(latch, buttons(0), t).buttons, 0u);
    CHECK_EQ(game_poll(latch, buttons(0), t).buttons, 0u);
}

TEST(latch_always_returns_newest_analog_values) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    PadState s;
    s.right_x = 0.25;
    s.left_y = -1.0;
    game_poll(latch, s, t);
    for (int i = 1; i <= 10; ++i) { // stale samples must never be replayed
        s.right_x = 0.25 + 0.05 * i;
        s.device_timestamp = static_cast<uint64_t>(i);
        latch.observe(s);
    }
    s.right_x = 0.123456789012345; // no quantisation either
    s.device_timestamp = 99;
    const PadState out = game_poll(latch, s, t);
    CHECK(out.right_x == 0.123456789012345);
    CHECK(out.left_y == -1.0);
    CHECK_EQ(out.device_timestamp, uint64_t{99});
}

TEST(latch_passes_trigger_values_through_untouched) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    PadState s;
    for (double v : {0.0, 0.1, 0.35, 0.49, 0.5, 0.8, 1.0, 0.6, 0.31, 0.3, 0.05, 0.0}) {
        s.right_trigger = v;
        s.left_trigger = 1.0 - v;
        const PadState out = game_poll(latch, s, t);
        CHECK(out.right_trigger == v);
        CHECK(out.left_trigger == 1.0 - v);
    }
}

TEST(latch_preserves_trigger_pull_between_game_polls) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    PadState s;
    game_poll(latch, s, t);
    for (double v : {0.4, 0.9, 0.7, 0.1}) { // quick pull seen only by the sampler
        s.right_trigger = v;
        latch.observe(s);
    }
    s.right_trigger = 0.0;
    CHECK_NEAR(game_poll(latch, s, t).right_trigger, 0.9, 1e-12); // reported at its peak
    CHECK(game_poll(latch, s, t).right_trigger == 0.0);
    CHECK(game_poll(latch, s, t).left_trigger == 0.0);
}

TEST(latch_trigger_hysteresis_ignores_jitter_around_threshold) {
    TransitionLatch latch(0.5, 0.3);
    Ticks t = 0;
    PadState s;
    s.right_trigger = 0.6;
    game_poll(latch, s, t);
    for (double v : {0.45, 0.55, 0.4, 0.6, 0.35, 0.52}) { // never crosses the release threshold
        s.right_trigger = v;
        latch.observe(s);
    }
    CHECK_NEAR(game_poll(latch, s, t).right_trigger, 0.52, 1e-12);
    CHECK_EQ(latch.stats().replayed, uint64_t{0});
}

// Property test: random taps of random length, a sampler at 8x the game's poll
// rate. Many taps start and end between two game polls. For every button the
// game must see exactly as many presses as physically happened, and end in
// the physical state.
TEST(latch_random_streams_lose_and_invent_nothing) {
    std::mt19937 rng(12345);
    for (int round = 0; round < 200; ++round) {
        TransitionLatch latch(0.5, 0.3);
        Ticks t = 0;
        std::vector<uint32_t> truth, seen;
        uint32_t phys = 0;
        int remaining = 0;
        truth.push_back(0);
        seen.push_back(game_poll(latch, buttons(0), t).buttons);
        for (int frame = 0; frame < 300; ++frame) {
            for (int s = 0; s < 8; ++s) {
                if (remaining-- <= 0) {
                    if (phys) {
                        // Gaps of at least three game polls let the replay drain.
                        phys = 0;
                        remaining = static_cast<int>(rng() % 40) + 24;
                    } else {
                        phys = (rng() & 1) ? kA : kB;
                        remaining = static_cast<int>(rng() % 20) + 1; // 1..20 samples, often < one poll
                    }
                }
                truth.push_back(phys);
                if (s < 7) latch.observe(buttons(phys));
            }
            seen.push_back(game_poll(latch, buttons(phys), t).buttons);
        }
        for (int i = 0; i < 4; ++i) seen.push_back(game_poll(latch, buttons(phys), t).buttons);
        CHECK_EQ(seen.back(), phys);
        CHECK_EQ(rising_edges(seen, kA), rising_edges(truth, kA));
        CHECK_EQ(rising_edges(seen, kB), rising_edges(truth, kB));
        CHECK_EQ(latch.stats().collapsed, uint64_t{0});
        if (bll::test::failures()) return;
    }
}

// The same physical stream polled without the latch loses taps; this pins the
// problem the latch exists to solve.
TEST(latch_baseline_polling_does_lose_taps) {
    std::mt19937 rng(777);
    std::vector<uint32_t> truth{0}, seen{0};
    uint32_t phys = 0;
    int hold = 0;
    for (int frame = 0; frame < 2000; ++frame) {
        for (int s = 0; s < 8; ++s) {
            if (hold-- <= 0) {
                phys ^= kA;
                hold = static_cast<int>(rng() % 12) + 1;
            }
            truth.push_back(phys);
        }
        seen.push_back(phys);
    }
    CHECK(rising_edges(seen, kA) < rising_edges(truth, kA));
}

// Sampler thread and game thread on a shared simulated device. The device
// emits a known number of short taps; with the latch the game counts them all.
TEST(latched_pad_counts_every_tap_across_threads) {
    constexpr int kTaps = 300;
    std::atomic<uint32_t> device{0};
    std::atomic<bool> done{false};
    LatchedPad pad(0.5, 0.3);
    const auto read = [&](PadState& s) {
        s.buttons = device.load();
        return true;
    };

    std::atomic<uint64_t> samples{0};
    std::thread sampler([&] {
        while (!done.load()) {
            pad.sample(read);
            samples.fetch_add(1);
        }
    });

    int presses = 0;
    uint32_t prev = 0;
    Ticks clock = 0;
    const auto poll_game = [&] {
        PadState out;
        clock += kNsPerMs;
        CHECK(pad.poll(read, clock, out));
        if ((out.buttons & kA) && !(prev & kA)) ++presses;
        prev = out.buttons;
    };

    // Each state is held until the free-running sampler has observed it, so
    // every tap is physically visible to the sampler, yet it starts and ends
    // entirely between two game polls: plain polling would see none of them.
    const auto hold_until_sampled = [&] {
        const uint64_t at = samples.load();
        while (samples.load() < at + 2) std::this_thread::yield();
    };
    for (int i = 0; i < kTaps; ++i) {
        device.store(kA); // seq_cst: visible to the sampler before `samples` is read below
        hold_until_sampled();
        device.store(0);
        hold_until_sampled();
        poll_game();
        poll_game();
    }
    done.store(true);
    sampler.join();

    CHECK_EQ(presses, kTaps);
    CHECK_EQ(prev, 0u);
    CHECK_EQ(pad.stats().replayed, static_cast<uint64_t>(kTaps));
    CHECK_EQ(pad.stats().collapsed, uint64_t{0});
}

TEST(latched_pad_resets_on_failed_read) {
    LatchedPad pad(0.5, 0.3);
    bool connected = true;
    uint32_t state = kA;
    const auto read = [&](PadState& s) {
        s.buttons = state;
        return connected;
    };
    PadState out;
    CHECK(pad.poll(read, kNsPerMs, out));
    CHECK_EQ(out.buttons, kA);
    connected = false;
    out.buttons = 0xDEAD;
    CHECK(!pad.poll(read, 2 * kNsPerMs, out));
    CHECK_EQ(out.buttons, 0xDEADu); // untouched on failure: the hook returns the real error
    connected = true;
    state = 0;
    CHECK(pad.poll(read, 3 * kNsPerMs, out));
    CHECK_EQ(out.buttons, 0u); // no stuck button after reconnect
}
