#include <thread>

#include "core/config.h"
#include "core/spsc_ring.h"
#include "core/stats.h"
#include "core/version.h"
#include "test.h"

using namespace bll;

TEST(config_defaults_are_mode_2) {
    std::vector<std::string> w;
    const Config c = parse_config("", &w);
    CHECK(w.empty());
    CHECK_EQ(c.mode, 2);
    CHECK(c.enabled && c.safe_mode && !c.allow_unrecognized_build);
    CHECK(c.low_latency_queue && c.waitable_swap_chain);
    CHECK_EQ(c.target_queue_depth, 1);
    // Nothing experimental or behaviour-changing beyond the queue is on by default.
    CHECK(!c.allow_tearing && !c.disable_vsync && !c.frame_pacing);
    CHECK(!c.buffer_button_transitions && !c.adaptive_cpu_gpu_scheduling && !c.gpu_sync);
    CHECK(c.optimize_gamepad);
}

TEST(config_shipped_default_file_parses_cleanly) {
    std::vector<std::string> w;
    const Config c = parse_config(kDefaultConfigText, &w);
    CHECK(w.empty());
    CHECK_EQ(c.mode, 2);
    CHECK(c.low_latency_queue && c.waitable_swap_chain && !c.record_frame_timings);
}

TEST(config_mode_0_changes_nothing) {
    const Config c = parse_config("[General]\nMode=0\n");
    CHECK(!c.low_latency_queue && !c.waitable_swap_chain && !c.allow_tearing && !c.disable_vsync);
    CHECK(!c.frame_pacing && !c.buffer_button_transitions && !c.adaptive_cpu_gpu_scheduling && !c.gpu_sync);
}

TEST(config_modes_map_to_features) {
    Config c = parse_config("[General]\nMode=1\n");
    CHECK(c.allow_tearing && c.disable_vsync && !c.low_latency_queue);
    c = parse_config("[General]\nMode=3\n");
    CHECK(c.frame_pacing);
    CHECK_NEAR(c.target_fps, -3.0, 1e-9); // refresh-relative default
    c = parse_config("[General]\nMode=3\n[Rendering]\nTargetFPS=0\n");
    CHECK_NEAR(c.target_fps, 0.0, 1e-9); // an explicit value wins
    c = parse_config("[General]\nMode=4\n");
    CHECK(c.adaptive_cpu_gpu_scheduling && c.gpu_sync && c.waitable_swap_chain);
    c = parse_config("[General]\nMode=5\n");
    CHECK(c.low_latency_queue && c.waitable_swap_chain && c.allow_tearing && c.frame_pacing);
    CHECK(c.buffer_button_transitions && !c.adaptive_cpu_gpu_scheduling && !c.disable_vsync);
}

TEST(config_explicit_keys_override_mode_regardless_of_order) {
    const Config c = parse_config("[Rendering]\nWaitableSwapChain = off ; comment\nTargetFPS=141.5\n"
                                  "[Input]\nBufferButtonTransitions=YES\n[General]\nMode=2\n");
    CHECK(c.low_latency_queue);
    CHECK(!c.waitable_swap_chain);
    CHECK(c.buffer_button_transitions);
    CHECK_NEAR(c.target_fps, 141.5, 1e-9);
}

TEST(config_reports_unknown_and_invalid_values) {
    std::vector<std::string> w;
    const Config c = parse_config("\xEF\xBB\xBF[General]\nMode=9\nbogus\n[Rendering]\nTargetQueueDepth=99\n"
                                  "DisableVSync=maybe\n[Input]\nctrl_improvedInputResponse=1\nSamplerHz=5\n",
                                  &w);
    CHECK_EQ(c.mode, 2);
    CHECK_EQ(c.target_queue_depth, 16);
    CHECK(!c.disable_vsync);
    CHECK_EQ(c.sampler_hz, 125);
    CHECK_EQ(w.size(), size_t{6});
}

TEST(config_latch_requires_gamepad_hooks) {
    std::vector<std::string> w;
    const Config c = parse_config("[Input]\nOptimizeGamepad=false\nBufferButtonTransitions=true\n", &w);
    CHECK(!c.buffer_button_transitions);
    CHECK_EQ(w.size(), size_t{1});
}

TEST(config_trigger_thresholds_keep_hysteresis) {
    const Config c = parse_config("[Input]\nTriggerPressThreshold=0.3\nTriggerReleaseThreshold=0.6\n");
    CHECK(c.trigger_release_threshold < c.trigger_press_threshold);
}

TEST(version_parses_target_package) {
    PackageIdentity id;
    CHECK(parse_package_full_name("Microsoft.MinecraftUWP_1.16.10004.0_x64__8wekyb3d8bbwe", id));
    CHECK(id.name == "Microsoft.MinecraftUWP");
    CHECK(id.version == (PackageVersion{1, 16, 10004, 0}));
    CHECK(id.architecture == "x64");
    CHECK(id.publisher_id == "8wekyb3d8bbwe");
    CHECK(classify_build(id) == BuildClass::Target);
    CHECK(game_version(id.version) == "1.16.100.4");
}

TEST(version_rejects_other_builds) {
    PackageIdentity id;
    // Betas of the same cycle used the same package name; 1.16.100.54 ran RenderDragon.
    CHECK(parse_package_full_name("Microsoft.MinecraftUWP_1.16.10054.0_x64__8wekyb3d8bbwe", id));
    CHECK(classify_build(id) == BuildClass::OtherBedrock);
    CHECK(parse_package_full_name("Microsoft.MinecraftUWP_1.16.10060.0_x64__8wekyb3d8bbwe", id));
    CHECK(classify_build(id) == BuildClass::OtherBedrock);
    // The package revision is not part of the game version.
    CHECK(parse_package_full_name("Microsoft.MinecraftUWP_1.16.10004.70_x64__8wekyb3d8bbwe", id));
    CHECK(classify_build(id) == BuildClass::Target);
    // 1.16.200 is the first stable RenderDragon build on Windows.
    CHECK(parse_package_full_name("Microsoft.MinecraftUWP_1.16.20002.0_x64__8wekyb3d8bbwe", id));
    CHECK(classify_build(id) == BuildClass::OtherBedrock);
    CHECK(parse_package_full_name("Microsoft.MinecraftUWP_1.16.10004.0_x86__8wekyb3d8bbwe", id));
    CHECK(classify_build(id) == BuildClass::OtherBedrock);
    CHECK(parse_package_full_name("Microsoft.MinecraftUWP_1.21.12001.0_x64__8wekyb3d8bbwe", id));
    CHECK(classify_build(id) == BuildClass::OtherBedrock);
    CHECK(parse_package_full_name("Microsoft.MinecraftWindowsBeta_1.16.10004.0_x64__8wekyb3d8bbwe", id));
    CHECK(classify_build(id) == BuildClass::Foreign);
    CHECK(parse_package_full_name("Microsoft.MinecraftUWP_1.16.10004.0_x64__someoneelse", id));
    CHECK(classify_build(id) == BuildClass::Foreign);
    CHECK(classify_build(PackageIdentity{}) == BuildClass::Unpackaged);
}

TEST(version_rejects_malformed_names) {
    PackageIdentity id;
    CHECK(!parse_package_full_name("", id));
    CHECK(!parse_package_full_name("Microsoft.MinecraftUWP", id));
    CHECK(!parse_package_full_name("Name_1.2.3_x64__pub", id));
    CHECK(!parse_package_full_name("Name_1.2.3.4.5_x64__pub", id));
    CHECK(!parse_package_full_name("Name_1.2.x.4_x64__pub", id));
    CHECK(!parse_package_full_name("Name_1.2.3.99999_x64__pub", id));
    CHECK(!parse_package_full_name("Name_1.2.3.4_x64_pub", id));
}

TEST(stats_percentiles_use_nearest_rank) {
    RollingWindow<int, 128> w;
    CHECK_EQ(w.percentile(50), 0);
    for (int i = 100; i >= 1; --i) w.push(i);
    CHECK_EQ(w.percentile(50), 50);
    CHECK_EQ(w.percentile(95), 95);
    CHECK_EQ(w.percentile(99), 99);
    CHECK_EQ(w.percentile(100), 100);
    CHECK_EQ(w.percentile(0), 1);
    CHECK_NEAR(w.mean(), 50.5, 1e-9);
    CHECK_NEAR(w.stddev(), 29.011, 0.01);
}

TEST(stats_window_discards_oldest) {
    RollingWindow<int, 4> w;
    for (int i = 1; i <= 10; ++i) w.push(i);
    CHECK_EQ(w.size(), size_t{4});
    CHECK_EQ(w.percentile(0), 7);
    CHECK_EQ(w.max(), 10);
}

TEST(spsc_ring_is_bounded_and_ordered) {
    SpscRing<int, 4> r;
    int v = 0;
    CHECK(!r.pop(v));
    for (int i = 0; i < 4; ++i) CHECK(r.push(i));
    CHECK(!r.push(99)); // full: rejected, not overwritten
    for (int i = 0; i < 4; ++i) {
        CHECK(r.pop(v));
        CHECK_EQ(v, i);
    }
    CHECK(!r.pop(v));
}

TEST(spsc_ring_transfers_everything_across_threads) {
    SpscRing<uint32_t, 256> r;
    constexpr uint32_t kCount = 200000;
    std::thread producer([&] {
        for (uint32_t i = 0; i < kCount;) {
            if (r.push(i)) ++i;
            else std::this_thread::yield();
        }
    });
    uint32_t expected = 0;
    bool ordered = true;
    while (expected < kCount) {
        uint32_t v;
        if (r.pop(v)) ordered &= v == expected++;
        else std::this_thread::yield();
    }
    producer.join();
    CHECK(ordered);
    CHECK_EQ(r.size(), size_t{0});
}
