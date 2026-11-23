// Diagnostics thread: drains frame records from the render thread, optionally
// writes them to a CSV file and logs a periodic statistical summary.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/spsc_ring.h"
#include "metrics/frame_record.h"

namespace bll {

struct Summary {
    size_t frames = 0;
    double avg_fps = 0;
    double frame_ms_p50 = 0, frame_ms_p95 = 0, frame_ms_p99 = 0, frame_ms_stddev = 0;
    double low_1pct_fps = 0, low_01pct_fps = 0; // FPS at the 99th / 99.9th percentile frame time
    double input_age_ms_p50 = -1, input_age_ms_p95 = -1, input_age_ms_p99 = -1;
    double display_latency_ms_p50 = -1, display_latency_ms_p95 = -1;
    // input_age + display_latency medians: game's input poll to scan-out start.
    double input_to_display_ms_p50 = -1;
    double polls_per_frame = 0;
    double present_block_ms_p50 = 0;
    double gate_wait_ms_mean = 0;
    double gpu_pending_share = 0;
    size_t missed_refreshes = 0;
    Bottleneck bottleneck = Bottleneck::Unknown;
};

// Pure: statistics over a batch of records.
Summary summarize(const std::vector<FrameRecord>& frames);
std::string format_summary(const Summary& s);
std::string csv_header();
std::string csv_row(const FrameRecord& r);

class MetricsSink {
public:
    struct Options {
        std::filesystem::path csv_path; // empty = no CSV
        int summary_interval_s = 10;
        bool input_stats = true;
    };

    ~MetricsSink();
    void start(const Options& options);
    void stop();

    // Render thread; wait-free. Records are dropped (and counted) when the
    // diagnostics thread falls behind.
    void submit(const FrameRecord& r) {
        if (!ring_.push(r)) dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    // Text appended to every summary line by other subsystems (set rarely).
    void set_extra_provider(std::string (*provider)()) { extra_ = provider; }

private:
    void run();

    SpscRing<FrameRecord, 4096> ring_;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    Options options_;
    std::string (*extra_)() = nullptr;
};

} // namespace bll
