#include "metrics/metrics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "core/log.h"

namespace bll {

namespace {

// Nearest-rank percentile of an unsorted vector (sorted in place).
double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    const double rank = std::ceil(p / 100.0 * static_cast<double>(v.size()));
    const size_t k = rank < 1.0 ? 0 : std::min(v.size() - 1, static_cast<size_t>(rank) - 1);
    return v[k];
}

} // namespace

Summary summarize(const std::vector<FrameRecord>& frames) {
    Summary s;
    std::vector<double> frame_ms, input_ms, display_ms, block_ms;
    std::array<size_t, 7> classes{};
    double gate_total = 0;
    uint64_t polls = 0;
    size_t gpu_pending = 0;

    for (const FrameRecord& r : frames) {
        if (r.index == 0) continue; // not a displayed frame
        ++s.frames;
        if (r.interval > 0) frame_ms.push_back(to_ms(r.interval));
        if (r.input_age >= 0) input_ms.push_back(to_ms(r.input_age));
        if (r.display_latency >= 0) display_ms.push_back(to_ms(r.display_latency));
        block_ms.push_back(to_ms(r.present_return - r.present_call));
        gate_total += to_ms(r.wait_latency + r.wait_gpu + r.wait_pacer + r.wait_jit);
        polls += r.input_polls;
        gpu_pending += r.gpu_pending;
        s.missed_refreshes += r.missed_refresh;
        ++classes[static_cast<size_t>(r.frame_class)];
    }
    if (s.frames == 0) return s;

    if (!frame_ms.empty()) {
        double sum = 0;
        for (double v : frame_ms) sum += v;
        const double mean = sum / static_cast<double>(frame_ms.size());
        double var = 0;
        for (double v : frame_ms) var += (v - mean) * (v - mean);
        s.frame_ms_stddev = frame_ms.size() > 1 ? std::sqrt(var / static_cast<double>(frame_ms.size() - 1)) : 0;
        s.avg_fps = mean > 0 ? 1000.0 / mean : 0;
        s.frame_ms_p50 = percentile(frame_ms, 50);
        s.frame_ms_p95 = percentile(frame_ms, 95);
        s.frame_ms_p99 = percentile(frame_ms, 99);
        s.low_1pct_fps = 1000.0 / s.frame_ms_p99;
        s.low_01pct_fps = 1000.0 / percentile(frame_ms, 99.9);
    }
    if (!input_ms.empty()) {
        s.input_age_ms_p50 = percentile(input_ms, 50);
        s.input_age_ms_p95 = percentile(input_ms, 95);
        s.input_age_ms_p99 = percentile(input_ms, 99);
    }
    if (!display_ms.empty()) {
        s.display_latency_ms_p50 = percentile(display_ms, 50);
        s.display_latency_ms_p95 = percentile(display_ms, 95);
    }
    if (s.input_age_ms_p50 >= 0 && s.display_latency_ms_p50 >= 0)
        s.input_to_display_ms_p50 = s.input_age_ms_p50 + s.display_latency_ms_p50;
    s.present_block_ms_p50 = percentile(block_ms, 50);
    s.gate_wait_ms_mean = gate_total / static_cast<double>(s.frames);
    s.polls_per_frame = static_cast<double>(polls) / static_cast<double>(s.frames);
    s.gpu_pending_share = static_cast<double>(gpu_pending) / static_cast<double>(s.frames);

    size_t best = 0;
    for (size_t i = 1; i < classes.size(); ++i)
        if (classes[i] > classes[best]) best = i;
    s.bottleneck = classes[best] * 10 >= s.frames * 6 ? static_cast<Bottleneck>(best) : Bottleneck::Mixed;
    return s;
}

std::string format_summary(const Summary& s) {
    char buf[640];
    std::snprintf(buf, sizeof buf,
                  "frames=%zu fps=%.1f frame_ms p50=%.2f p95=%.2f p99=%.2f sd=%.2f low1%%=%.1f low0.1%%=%.1f | "
                  "input_age_ms p50=%.2f p95=%.2f p99=%.2f polls/frame=%.2f | display_ms p50=%.2f p95=%.2f "
                  "input->display p50=%.2f | present_block_ms p50=%.2f gate_ms=%.2f gpu_pending=%.0f%% missed=%zu "
                  "limit=%s",
                  s.frames, s.avg_fps, s.frame_ms_p50, s.frame_ms_p95, s.frame_ms_p99, s.frame_ms_stddev,
                  s.low_1pct_fps, s.low_01pct_fps, s.input_age_ms_p50, s.input_age_ms_p95, s.input_age_ms_p99,
                  s.polls_per_frame, s.display_latency_ms_p50, s.display_latency_ms_p95, s.input_to_display_ms_p50,
                  s.present_block_ms_p50, s.gate_wait_ms_mean, s.gpu_pending_share * 100.0, s.missed_refreshes,
                  to_string(s.bottleneck));
    return buf;
}

std::string csv_header() {
    return "frame,frame_start_ns,present_call_ns,present_return_ns,interval_ns,wait_latency_ns,wait_gpu_ns,"
           "wait_pacer_ns,wait_jit_ns,input_age_ns,display_latency_ns,input_polls,sync_interval,present_flags,"
           "hresult,gpu_pending,missed_refresh,class";
}

std::string csv_row(const FrameRecord& r) {
    char buf[384];
    std::snprintf(buf, sizeof buf, "%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%u,%u,%u,0x%08X,%d,%d,%s",
                  static_cast<unsigned long long>(r.index), static_cast<long long>(r.frame_start),
                  static_cast<long long>(r.present_call), static_cast<long long>(r.present_return),
                  static_cast<long long>(r.interval), static_cast<long long>(r.wait_latency),
                  static_cast<long long>(r.wait_gpu), static_cast<long long>(r.wait_pacer),
                  static_cast<long long>(r.wait_jit), static_cast<long long>(r.input_age),
                  static_cast<long long>(r.display_latency), r.input_polls, r.sync_interval, r.present_flags,
                  static_cast<unsigned>(r.result), r.gpu_pending ? 1 : 0, r.missed_refresh ? 1 : 0,
                  to_string(r.frame_class));
    return buf;
}

MetricsSink::~MetricsSink() { stop(); }

void MetricsSink::start(const Options& options) {
    if (thread_.joinable()) return;
    options_ = options;
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
}

void MetricsSink::stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    thread_.join();
}

void MetricsSink::run() {
    std::ofstream csv;
    if (!options_.csv_path.empty()) {
        csv.open(options_.csv_path, std::ios::out | std::ios::trunc);
        if (csv) csv << csv_header() << '\n';
        else BLL_WARN("could not open frame CSV for writing");
    }

    std::vector<FrameRecord> batch;
    batch.reserve(4096);
    const Ticks period = static_cast<Ticks>(options_.summary_interval_s) * kNsPerSec;
    Ticks next_summary = now_ns() + period;

    for (;;) {
        const bool stopping = stop_.load();
        FrameRecord r;
        while (ring_.pop(r)) {
            if (csv) csv << csv_row(r) << '\n';
            batch.push_back(r);
        }
        const Ticks now = now_ns();
        if (now >= next_summary || stopping) {
            next_summary = now + period;
            if (!batch.empty()) {
                Summary s = summarize(batch);
                if (!options_.input_stats) s.input_age_ms_p50 = s.input_age_ms_p95 = s.input_age_ms_p99 = -1;
                const uint64_t dropped = dropped_.exchange(0);
                const std::string extra = extra_ ? extra_() : std::string();
                BLL_INFO("summary: %s%s%s%s", format_summary(s).c_str(), extra.empty() ? "" : " | ", extra.c_str(),
                         dropped ? " (records dropped)" : "");
                batch.clear();
            }
            if (csv) csv.flush();
        }
        if (stopping) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

} // namespace bll
