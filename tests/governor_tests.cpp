// Closed-loop tests of the frame governor against the virtual display model.
// They check the control logic (ordering of waits, fail-safes, relative
// effect of each mode under the documented DXGI queue semantics). They are
// not measurements of Minecraft or of any real display.
#include <cstdio>

#include "sim.h"
#include "test.h"

using namespace bll;
using namespace bll::sim;

namespace {

constexpr Ticks k60Hz = 16'666'667;

Config mode_config(int mode, const char* extra = "") {
    return parse_config(std::string("[General]\nMode=") + std::to_string(mode) + "\n" + extra);
}

// A device that offers nothing: no waitable object, no GPU information.
struct NullDevice : GateDevice {
    Wait wait_frame_latency(Ticks) override { return Wait::Unavailable; }
    bool gpu_frame_pending() override { return false; }
};

} // namespace

TEST(governor_mode_0_adds_no_wait) {
    FakeClock clock;
    NullDevice dev;
    FrameGovernor g(mode_config(0), clock);
    g.set_refresh_period(k60Hz);
    for (int i = 0; i < 500; ++i) {
        clock.t += 5 * kNsPerMs;
        PresentSample s;
        s.call = clock.t;
        s.ret = clock.t += 100 * kNsPerUs;
        s.sync_interval = 1;
        const FrameRecord& r = g.on_present(dev, s);
        CHECK_EQ(clock.t, s.ret); // the gate did not advance time
        CHECK_EQ(r.wait_latency + r.wait_gpu + r.wait_pacer + r.wait_jit, Ticks{0});
        if (bll::test::failures()) return;
    }
    CHECK_EQ(clock.sleeps, uint64_t{0});
}

TEST(governor_reports_input_age_and_poll_count) {
    FakeClock clock;
    NullDevice dev;
    FrameGovernor g(mode_config(0), clock);
    PresentSample s;
    s.call = clock.t;
    s.ret = clock.t;
    CHECK_EQ(g.on_present(dev, s).input_age, Ticks{-1}); // the game has not polled yet

    g.note_input_poll(clock.t + 1 * kNsPerMs);
    g.note_input_poll(clock.t + 2 * kNsPerMs);
    s.call = clock.t + 9 * kNsPerMs;
    s.ret = s.call;
    const FrameRecord& r = g.on_present(dev, s);
    CHECK_EQ(r.input_age, 7 * kNsPerMs); // measured from the latest poll
    CHECK_EQ(r.input_polls, 2u);

    s.call += 10 * kNsPerMs;
    s.ret = s.call;
    const FrameRecord& r2 = g.on_present(dev, s); // no new poll: the sample just got older
    CHECK_EQ(r2.input_age, 17 * kNsPerMs);
    CHECK_EQ(r2.input_polls, 0u);
}

TEST(governor_does_not_gate_undisplayed_presents) {
    FakeClock clock;
    NullDevice dev;
    FrameGovernor g(mode_config(3, "[Rendering]\nTargetFPS=30\n"), clock);
    PresentSample s;
    s.displayed = false; // DXGI_PRESENT_TEST or a failed / occluded present
    for (int i = 0; i < 10; ++i) {
        s.call = s.ret = clock.t;
        const FrameRecord& r = g.on_present(dev, s);
        CHECK_EQ(r.index, uint64_t{0});
        CHECK_EQ(clock.t, s.ret);
    }
    CHECK_EQ(clock.sleeps, uint64_t{0});
}

TEST(governor_frame_cap_paces_frame_starts) {
    FakeClock clock;
    clock.late_max = 400 * kNsPerUs;
    NullDevice dev;
    FrameGovernor g(mode_config(3, "[Rendering]\nTargetFPS=100\n"), clock);
    std::vector<double> intervals;
    Ticks prev_start = 0;
    for (int i = 0; i < 600; ++i) {
        const Ticks start = clock.t;
        if (i > 50) intervals.push_back(to_ms(start - prev_start));
        prev_start = start;
        clock.t += (2 + i % 3) * kNsPerMs; // 2..4 ms of work
        PresentSample s;
        s.call = clock.t;
        s.ret = clock.t += 50 * kNsPerUs;
        g.on_present(dev, s);
    }
    CHECK_NEAR(median(intervals), 10.0, 0.01);
    CHECK_NEAR(percentile(intervals, 99), 10.0, 0.01);
    CHECK_NEAR(percentile(intervals, 1), 10.0, 0.01);
    CHECK(g.bottleneck() == Bottleneck::FrameCap);
}

TEST(governor_relative_frame_cap_follows_the_refresh_rate) {
    FakeClock clock;
    FrameGovernor g(mode_config(3), clock); // TargetFPS defaults to -3
    CHECK_EQ(g.pacer_interval(), Ticks{0}); // refresh unknown: no cap yet, never a guessed one
    g.set_refresh_period(6'944'444);        // 144 Hz
    CHECK_NEAR(1e9 / static_cast<double>(g.pacer_interval()), 141.0, 0.01);
    g.set_refresh_period(k60Hz);
    CHECK_NEAR(1e9 / static_cast<double>(g.pacer_interval()), 57.0, 0.01);
}

TEST(governor_bypasses_a_waitable_object_that_stops_signalling) {
    struct DeadDevice : GateDevice {
        FakeClock& clock;
        int waits = 0;
        explicit DeadDevice(FakeClock& c) : clock(c) {}
        Wait wait_frame_latency(Ticks timeout) override {
            ++waits;
            clock.t += timeout;
            return Wait::Timeout;
        }
        bool gpu_frame_pending() override { return false; }
    };
    FakeClock clock;
    DeadDevice dev(clock);
    FrameGovernor g(mode_config(2), clock);
    for (int i = 0; i < 200; ++i) {
        clock.t += 5 * kNsPerMs;
        PresentSample s;
        s.call = s.ret = clock.t;
        g.on_present(dev, s);
    }
    // Three bounded timeouts, then the wait is skipped instead of stalling every frame.
    CHECK_EQ(dev.waits, 3);
}

TEST(governor_gpu_sync_wait_is_bounded) {
    struct StuckGpu : GateDevice {
        Wait wait_frame_latency(Ticks) override { return Wait::Unavailable; }
        bool gpu_frame_pending() override { return true; } // a query that never completes
    };
    FakeClock clock;
    StuckGpu dev;
    FrameGovernor g(mode_config(0, "[Experimental]\nGpuSync=true\n"), clock);
    PresentSample s;
    s.call = s.ret = clock.t;
    const FrameRecord& r = g.on_present(dev, s);
    CHECK(r.gpu_pending);
    CHECK(r.wait_gpu >= 30 * kNsPerMs);
    CHECK(r.wait_gpu < 32 * kNsPerMs);
}

// The main comparison. Vsync on, 60 Hz, a game that renders much faster than
// the refresh rate (3 ms CPU + 2 ms GPU), which is the regime where frames
// pile up in the presentation queue.
TEST(sim_each_mode_orders_latency_as_the_queue_model_predicts) {
    Workload w;
    constexpr int kFrames = 4000;
    // Vanilla: DXGI default maximum frame latency of 3, blocking Present.
    const LoopResult vanilla = run_game_loop(mode_config(0), k60Hz, 3, false, w, kFrames);
    // Device-level SetMaximumFrameLatency(1) only.
    const LoopResult queue1 =
        run_game_loop(mode_config(2, "[Rendering]\nWaitableSwapChain=false\n"), k60Hz, 1, false, w, kFrames);
    // Waitable swap chain with latency 1, waited on before the frame starts.
    const LoopResult waitable = run_game_loop(mode_config(2), k60Hz, 1, true, w, kFrames);
    // Plus the adaptive just-in-time start.
    const LoopResult jit = run_game_loop(mode_config(4), k60Hz, 1, true, w, kFrames);

    const double p = to_ms(k60Hz);
    const double v = median(vanilla.input_to_display_ms), q = median(queue1.input_to_display_ms);
    const double wt = median(waitable.input_to_display_ms), j = median(jit.input_to_display_ms);
    std::printf("    model, 60 Hz vsync, input poll -> vblank, median / p99 ms:\n");
    std::printf("      vanilla (queue 3)        %6.2f / %6.2f\n", v, percentile(vanilla.input_to_display_ms, 99));
    std::printf("      device latency 1         %6.2f / %6.2f\n", q, percentile(queue1.input_to_display_ms, 99));
    std::printf("      waitable, latency 1      %6.2f / %6.2f\n", wt, percentile(waitable.input_to_display_ms, 99));
    std::printf("      waitable + just-in-time  %6.2f / %6.2f  (repeated refreshes: %zu of %zu)\n", j,
                percentile(jit.input_to_display_ms, 99), jit.repeated_refreshes, jit.input_to_display_ms.size());

    CHECK_NEAR(v, 4 * p, 1.0);  // three queued frames plus the one being shown
    CHECK_NEAR(q, 2 * p, 1.0);  // one frame of queueing left, spent blocked inside Present
    CHECK_NEAR(wt, 1 * p, 1.0); // the blocking moved in front of the input poll
    CHECK(j < wt - 6.0);        // most of the remaining refresh period given back
    CHECK(j > 5.0);             // but never less than the work plus margins

    // None of this may cost frames: every mode still delivers one frame per refresh.
    for (const LoopResult* r : {&vanilla, &queue1, &waitable, &jit}) {
        CHECK_NEAR(median(r->frame_interval_ms), p, 0.05);
    }
    CHECK_EQ(waitable.repeated_refreshes, size_t{0});
    CHECK(jit.repeated_refreshes * 200 <= jit.input_to_display_ms.size()); // <= 0.5 %
}

TEST(sim_just_in_time_backs_off_under_frame_time_spikes) {
    Workload w;
    w.cpu_jitter = 1500 * kNsPerUs;
    w.spike_every = 97;
    w.spike_extra = 9 * kNsPerMs; // a hitch every ~1.6 s that nothing could predict
    constexpr int kFrames = 6000;
    const LoopResult waitable = run_game_loop(mode_config(2), k60Hz, 1, true, w, kFrames);
    const LoopResult jit = run_game_loop(mode_config(4), k60Hz, 1, true, w, kFrames);
    std::printf("    model with hitches: waitable %.2f ms (%zu repeats), just-in-time %.2f ms (%zu repeats) of %zu\n",
                median(waitable.input_to_display_ms), waitable.repeated_refreshes,
                median(jit.input_to_display_ms), jit.repeated_refreshes, jit.input_to_display_ms.size());
    // With hitches it cannot predict, the scheduler gives up most of its delay
    // rather than keep missing: never worse than plain waitable mode...
    CHECK(median(jit.input_to_display_ms) <= median(waitable.input_to_display_ms) + 0.1);
    // ...and the extra repeated refreshes stay under 0.5 % of frames.
    CHECK(jit.repeated_refreshes <= waitable.repeated_refreshes + jit.input_to_display_ms.size() / 200);
}

TEST(sim_just_in_time_is_inert_when_the_game_cannot_hold_refresh) {
    Workload w;
    w.cpu_mean = 14 * kNsPerMs; // CPU-bound right at the refresh period
    w.gpu_mean = 3 * kNsPerMs;
    const LoopResult waitable = run_game_loop(mode_config(2), k60Hz, 1, true, w, 3000);
    const LoopResult jit = run_game_loop(mode_config(4), k60Hz, 1, true, w, 3000);
    // No headroom means no delay: the scheduler must not make a struggling game worse.
    CHECK_NEAR(median(jit.input_to_display_ms), median(waitable.input_to_display_ms), 0.5);
    CHECK(jit.repeated_refreshes <= waitable.repeated_refreshes + 3);
}

TEST(sim_spin_cost_is_bounded_and_reported) {
    const LoopResult jit = run_game_loop(mode_config(4), k60Hz, 1, true, Workload{}, 4000);
    const double share = static_cast<double>(jit.spin_total) / static_cast<double>(jit.elapsed);
    std::printf("    model: busy-wait share of one core in just-in-time mode at 60 Hz: %.2f %%\n", share * 100.0);
    CHECK(share < 0.05);
}
