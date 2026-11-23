#include "metrics/metrics.h"
#include "sim.h"
#include "test.h"
#include "timing/adaptive_scheduler.h"
#include "timing/bottleneck.h"
#include "timing/frame_pacer.h"
#include "timing/precise_sleeper.h"
#include "timing/refresh_estimator.h"

using namespace bll;
using namespace bll::sim;

namespace {
constexpr Ticks k60Hz = 16'666'667;
constexpr Ticks k144Hz = 6'944'444;
} // namespace

TEST(pacer_disabled_never_waits) {
    FramePacer p;
    CHECK_EQ(p.next_start(123456), Ticks{0});
}

TEST(pacer_keeps_a_fixed_grid) {
    FramePacer p;
    p.set_interval(10 * kNsPerMs);
    Ticks now = 1000 * kNsPerMs;
    CHECK_EQ(p.next_start(now), now); // first frame starts immediately
    for (int i = 1; i <= 50; ++i) {
        const Ticks frame_done = now + 4 * kNsPerMs; // frame finished early
        const Ticks start = p.next_start(frame_done);
        CHECK_EQ(start, 1000 * kNsPerMs + i * 10 * kNsPerMs);
        now = start;
    }
}

TEST(pacer_absorbs_a_slightly_late_frame_without_drift) {
    FramePacer p;
    p.set_interval(10 * kNsPerMs);
    const Ticks t0 = 1000 * kNsPerMs;
    CHECK_EQ(p.next_start(t0), t0);
    // Frame 1 overruns by 2 ms: it starts immediately...
    CHECK(p.next_start(t0 + 12 * kNsPerMs) <= t0 + 12 * kNsPerMs);
    // ...and the following frame is back on the original grid.
    CHECK_EQ(p.next_start(t0 + 15 * kNsPerMs), t0 + 20 * kNsPerMs);
}

TEST(pacer_resyncs_after_a_stall_instead_of_bursting) {
    FramePacer p;
    p.set_interval(10 * kNsPerMs);
    const Ticks t0 = 1000 * kNsPerMs;
    p.next_start(t0);
    const Ticks after_stall = t0 + 500 * kNsPerMs;
    CHECK(p.next_start(after_stall) <= after_stall);
    // No backlog of "owed" frames: the grid restarts one interval after the stall ended.
    CHECK_EQ(p.next_start(after_stall + 1 * kNsPerMs), after_stall + 10 * kNsPerMs);
    CHECK_EQ(p.next_start(after_stall + 11 * kNsPerMs), after_stall + 20 * kNsPerMs);
}

TEST(sleeper_never_returns_before_the_deadline) {
    FakeClock clock;
    clock.late_min = 50 * kNsPerUs;
    clock.late_max = 900 * kNsPerUs;
    PreciseSleeper sleeper(clock, 1500 * kNsPerUs);
    Ticks worst_overshoot = 0;
    for (int i = 0; i < 2000; ++i) {
        const Ticks deadline = clock.t + 7 * kNsPerMs;
        const Ticks end = sleeper.wait_until(deadline);
        CHECK(end >= deadline);
        worst_overshoot = std::max(worst_overshoot, end - deadline);
        if (bll::test::failures()) return;
    }
    // The spin tail absorbs the timer lateness: precision is one spin iteration.
    CHECK(worst_overshoot <= clock.pause_cost);
}

TEST(sleeper_margin_tracks_timer_quality) {
    FakeClock good, bad;
    good.late_max = 60 * kNsPerUs;
    bad.late_min = 600 * kNsPerUs;
    bad.late_max = 1100 * kNsPerUs;
    PreciseSleeper a(good, 2500 * kNsPerUs), b(bad, 2500 * kNsPerUs);
    for (int i = 0; i < 500; ++i) {
        a.wait_until(good.t + 5 * kNsPerMs);
        b.wait_until(bad.t + 5 * kNsPerMs);
    }
    // A precise timer needs only a short busy-wait; a sloppy one a longer one.
    CHECK(a.margin() < 300 * kNsPerUs);
    CHECK(b.margin() > 1000 * kNsPerUs);
    CHECK(b.margin() <= 2500 * kNsPerUs);
    CHECK(a.total_spin() * 2 < b.total_spin());
}

TEST(sleeper_with_spinning_disabled_never_spins) {
    FakeClock clock;
    clock.late_max = 400 * kNsPerUs;
    PreciseSleeper sleeper(clock, 0);
    for (int i = 0; i < 200; ++i) {
        const Ticks deadline = clock.t + 5 * kNsPerMs;
        CHECK(sleeper.wait_until(deadline) >= deadline);
    }
    CHECK_EQ(clock.pauses, uint64_t{0});
    CHECK_EQ(sleeper.total_spin(), Ticks{0});
}

TEST(sleeper_past_deadline_returns_immediately) {
    FakeClock clock;
    PreciseSleeper sleeper(clock, 1500 * kNsPerUs);
    const Ticks before = clock.t;
    CHECK_EQ(sleeper.wait_until(before - 5 * kNsPerMs), before);
    CHECK_EQ(clock.sleeps, uint64_t{0});
}

TEST(refresh_estimator_measures_period_from_frame_statistics) {
    RefreshEstimator e;
    CHECK_EQ(e.period(), Ticks{0});
    e.seed(k60Hz);
    CHECK_EQ(e.period(), k60Hz);
    CHECK(!e.measured());
    Ticks t = 5 * kNsPerSec;
    uint32_t count = 0xFFFFFFF0u; // crosses the 32-bit wrap
    for (int i = 0; i < 40; ++i) {
        e.observe(count, t);
        const uint32_t step = 1 + static_cast<uint32_t>(i % 3); // statistics are not sampled every vblank
        count += step;
        t += static_cast<Ticks>(step) * k144Hz;
    }
    CHECK(e.measured());
    CHECK_NEAR(e.period(), k144Hz, 10);
}

TEST(refresh_estimator_ignores_glitches) {
    RefreshEstimator e;
    Ticks t = kNsPerSec;
    uint32_t c = 100;
    for (int i = 0; i < 10; ++i) e.observe(c += 1, t += k60Hz);
    e.observe(c += 1, t += 3 * kNsPerSec); // suspend / mode switch
    e.observe(c, t);                       // repeated sample
    for (int i = 0; i < 10; ++i) e.observe(c += 1, t += k60Hz);
    CHECK_NEAR(e.period(), k60Hz, 10);
}

TEST(scheduler_starts_at_zero_and_earns_its_delay) {
    AdaptiveScheduler s(2 * kNsPerMs);
    CHECK_EQ(s.delay(), Ticks{0});
    s.on_frame(4 * kNsPerMs, k60Hz, false);
    CHECK(s.delay() < 200 * kNsPerUs); // essentially nothing after one frame
    for (int i = 0; i < 300; ++i) s.on_frame(4 * kNsPerMs, k60Hz, false);
    // Steady 4 ms of work, 2 ms margin: about 10.6 ms can be given back.
    CHECK_NEAR(to_ms(s.delay()), 10.6, 0.5);
    CHECK(s.delay() + s.predicted_work() + 2 * kNsPerMs <= k60Hz);
}

TEST(scheduler_backs_off_multiplicatively_on_a_miss) {
    AdaptiveScheduler s(2 * kNsPerMs);
    for (int i = 0; i < 300; ++i) s.on_frame(4 * kNsPerMs, k60Hz, false);
    const Ticks before = s.delay();
    s.on_frame(4 * kNsPerMs, k60Hz, true);
    CHECK(s.delay() <= before / 2 + kNsPerUs);
    const Ticks after_miss = s.delay();
    for (int i = 0; i < 30; ++i) s.on_frame(4 * kNsPerMs, k60Hz, false);
    CHECK_NEAR(s.delay(), after_miss, 50 * kNsPerUs); // held during the cool-down
    for (int i = 0; i < 300; ++i) s.on_frame(4 * kNsPerMs, k60Hz, false);
    // The scale recovers, but the margin learned from the miss is kept.
    CHECK(s.delay() > after_miss + after_miss / 2);
    CHECK(s.delay() < before - 500 * kNsPerUs);
    CHECK(s.learned_margin() > 500 * kNsPerUs);
    CHECK_EQ(s.misses(), uint64_t{1});
}

TEST(scheduler_learned_margin_settles_at_the_target_miss_rate) {
    // Hidden cost the scheduler cannot observe (GPU time, compositor deadline):
    // a frame misses whenever delay + work + hidden exceeds the period.
    constexpr Ticks kHidden = 5 * kNsPerMs;
    AdaptiveScheduler s(1 * kNsPerMs);
    uint64_t late_misses = 0;
    constexpr int kFrames = 60000;
    for (int i = 0; i < kFrames; ++i) {
        const bool missed = s.delay() + 4 * kNsPerMs + kHidden > k60Hz;
        if (missed && i >= kFrames / 2) ++late_misses;
        s.on_frame(4 * kNsPerMs, k60Hz, missed);
    }
    // The margin has grown to cover the hidden cost...
    CHECK(s.learned_margin() >= kHidden - 2 * kNsPerMs);
    // ...and probing below it costs about one refresh per thousand frames.
    CHECK(late_misses >= 10);
    CHECK(late_misses <= 60);
}

TEST(scheduler_goes_inert_when_misses_persist) {
    AdaptiveScheduler s(2 * kNsPerMs);
    for (int i = 0; i < 300; ++i) s.on_frame(4 * kNsPerMs, k60Hz, false);
    CHECK(s.delay() > 5 * kNsPerMs);
    for (int i = 0; i < 40; ++i) s.on_frame(4 * kNsPerMs, k60Hz, true); // misses no matter what
    CHECK_EQ(s.delay(), Ticks{0});
}

TEST(scheduler_gives_no_delay_without_headroom_or_refresh) {
    AdaptiveScheduler s(2 * kNsPerMs);
    for (int i = 0; i < 300; ++i) s.on_frame(15 * kNsPerMs, k60Hz, false); // no budget left
    CHECK_EQ(s.delay(), Ticks{0});
    AdaptiveScheduler u(2 * kNsPerMs);
    for (int i = 0; i < 300; ++i) u.on_frame(2 * kNsPerMs, 0, false); // refresh unknown
    CHECK_EQ(u.delay(), Ticks{0});
}

TEST(scheduler_widens_prediction_for_noisy_work) {
    AdaptiveScheduler steady(2 * kNsPerMs), noisy(2 * kNsPerMs);
    for (int i = 0; i < 400; ++i) {
        steady.on_frame(5 * kNsPerMs, k60Hz, false);
        noisy.on_frame((i % 2 ? 3 : 7) * kNsPerMs, k60Hz, false); // same mean, high variance
    }
    CHECK(noisy.delay() < steady.delay());
    CHECK(noisy.predicted_work() > 7 * kNsPerMs);
}

TEST(bottleneck_classifies_single_frames) {
    FrameTiming t;
    t.interval = 10 * kNsPerMs;
    CHECK(classify_frame(t) == Bottleneck::Cpu);
    t.wait_pacer = 3 * kNsPerMs;
    CHECK(classify_frame(t) == Bottleneck::FrameCap);
    t = {};
    t.interval = 10 * kNsPerMs;
    t.gpu_pending = true;
    CHECK(classify_frame(t) == Bottleneck::Gpu);
    t.gpu_pending = false;
    t.wait_latency = 6 * kNsPerMs;
    CHECK(classify_frame(t) == Bottleneck::DisplayRefresh);
    t.wait_latency = 0;
    t.present_block = 5 * kNsPerMs;
    CHECK(classify_frame(t) == Bottleneck::Present);
    t.vsync = true;
    CHECK(classify_frame(t) == Bottleneck::DisplayRefresh);
    CHECK(classify_frame(FrameTiming{}) == Bottleneck::Unknown);
}

TEST(bottleneck_detector_needs_a_majority) {
    BottleneckDetector d;
    FrameTiming cpu, gpu;
    cpu.interval = gpu.interval = 10 * kNsPerMs;
    gpu.gpu_pending = true;
    CHECK(d.current() == Bottleneck::Unknown);
    for (int i = 0; i < 120; ++i) d.add(cpu);
    CHECK(d.current() == Bottleneck::Cpu);
    for (int i = 0; i < 60; ++i) d.add(gpu); // 50/50
    CHECK(d.current() == Bottleneck::Mixed);
    for (int i = 0; i < 60; ++i) d.add(gpu);
    CHECK(d.current() == Bottleneck::Gpu);
}

TEST(metrics_summary_statistics) {
    std::vector<FrameRecord> frames;
    for (int i = 1; i <= 1000; ++i) {
        FrameRecord r;
        r.index = static_cast<uint64_t>(i);
        r.interval = (i % 100 == 0 ? 30 : 10) * kNsPerMs; // 1 % slow frames
        r.present_call = i * 10 * kNsPerMs;
        r.present_return = r.present_call + kNsPerMs;
        r.input_age = (5 + i % 3) * kNsPerMs;
        r.display_latency = 20 * kNsPerMs;
        r.input_polls = 2;
        r.frame_class = Bottleneck::Cpu;
        frames.push_back(r);
    }
    FrameRecord test_present; // index 0: not a displayed frame, must be ignored
    test_present.interval = 999 * kNsPerMs;
    frames.push_back(test_present);

    const Summary s = summarize(frames);
    CHECK_EQ(s.frames, size_t{1000});
    CHECK_NEAR(s.frame_ms_p50, 10.0, 1e-9);
    CHECK_NEAR(s.frame_ms_p99, 10.0, 1e-9);
    CHECK_NEAR(s.low_01pct_fps, 1000.0 / 30.0, 1e-6);
    CHECK_NEAR(s.avg_fps, 1000.0 / 10.2, 1e-6);
    CHECK_NEAR(s.input_age_ms_p50, 6.0, 1e-9);
    CHECK_NEAR(s.input_age_ms_p99, 7.0, 1e-9);
    CHECK_NEAR(s.input_to_display_ms_p50, 26.0, 1e-9);
    CHECK_NEAR(s.polls_per_frame, 2.0, 1e-9);
    CHECK(s.bottleneck == Bottleneck::Cpu);
    CHECK(!format_summary(s).empty());
}

TEST(metrics_csv_row_matches_header_columns) {
    FrameRecord r;
    r.index = 7;
    r.result = static_cast<int32_t>(0x887A0001u);
    const std::string header = csv_header(), row = csv_row(r);
    CHECK_EQ(std::count(header.begin(), header.end(), ','), std::count(row.begin(), row.end(), ','));
    CHECK(row.find("0x887A0001") != std::string::npos);
}

TEST(metrics_sink_delivers_records_to_csv) {
    const auto path = std::filesystem::temp_directory_path() / "bll_metrics_test.csv";
    {
        MetricsSink sink;
        MetricsSink::Options o;
        o.csv_path = path;
        sink.start(o);
        for (uint64_t i = 1; i <= 500; ++i) {
            FrameRecord r;
            r.index = i;
            sink.submit(r);
        }
        sink.stop(); // drains before returning
    }
    std::FILE* f = std::fopen(path.string().c_str(), "r");
    CHECK(f != nullptr);
    if (!f) return;
    int lines = 0;
    for (int c; (c = std::fgetc(f)) != EOF;) lines += c == '\n';
    std::fclose(f);
    std::filesystem::remove(path);
    CHECK_EQ(lines, 501); // header + one row per record
}
