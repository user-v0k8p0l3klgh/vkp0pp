#include "timing/bottleneck.h"

namespace bll {

const char* to_string(Bottleneck b) {
    switch (b) {
    case Bottleneck::Cpu: return "cpu";
    case Bottleneck::Gpu: return "gpu";
    case Bottleneck::Present: return "present";
    case Bottleneck::DisplayRefresh: return "display-refresh";
    case Bottleneck::FrameCap: return "frame-cap";
    case Bottleneck::Mixed: return "mixed";
    default: return "unknown";
    }
}

Bottleneck classify_frame(const FrameTiming& t) {
    if (t.interval <= 0) return Bottleneck::Unknown;
    const Ticks slack = t.interval / 20; // 5 % of the frame
    // Order matters: a wait we inserted ourselves is the most specific signal.
    if (t.wait_pacer > slack) return Bottleneck::FrameCap;
    if (t.wait_gpu > slack || t.gpu_pending) return Bottleneck::Gpu;
    if (t.wait_latency > slack) return Bottleneck::DisplayRefresh;
    if (t.present_block > t.interval / 5) return t.vsync ? Bottleneck::DisplayRefresh : Bottleneck::Present;
    return Bottleneck::Cpu;
}

Bottleneck BottleneckDetector::add(const FrameTiming& t) {
    window_[head_] = classify_frame(t);
    head_ = (head_ + 1) % kWindow;
    if (count_ < kWindow) ++count_;
    if (count_ < kWindow) return current_;

    std::array<size_t, 7> votes{};
    for (Bottleneck b : window_) ++votes[static_cast<size_t>(b)];
    size_t best = 0;
    for (size_t i = 1; i < votes.size(); ++i)
        if (votes[i] > votes[best]) best = i;
    current_ = votes[best] * 10 >= kWindow * 6 ? static_cast<Bottleneck>(best) : Bottleneck::Mixed;
    return current_;
}

} // namespace bll
