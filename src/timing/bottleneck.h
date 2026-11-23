// Classifies what limits the frame rate: the CPU, the GPU, presentation
// back-pressure, the display refresh, or our own frame cap. Individual frames
// are classified from where the time went; the reported state is the majority
// over a window so that single outliers do not flip it.
#pragma once
#include <array>
#include <cstddef>

#include "metrics/frame_record.h"

namespace bll {

struct FrameTiming {
    Ticks interval = 0;      // frame-to-frame time
    Ticks present_block = 0; // time spent inside Present
    Ticks wait_latency = 0;
    Ticks wait_gpu = 0;
    Ticks wait_pacer = 0;
    bool gpu_pending = false;
    bool vsync = false;      // presented with a sync interval >= 1
};

Bottleneck classify_frame(const FrameTiming& t);

class BottleneckDetector {
public:
    Bottleneck add(const FrameTiming& t);
    // Majority class over the window, Mixed without a 60 % majority,
    // Unknown until the window has filled once.
    Bottleneck current() const { return current_; }

private:
    static constexpr size_t kWindow = 120;
    std::array<Bottleneck, kWindow> window_{};
    size_t head_ = 0;
    size_t count_ = 0;
    Bottleneck current_ = Bottleneck::Unknown;
};

} // namespace bll
