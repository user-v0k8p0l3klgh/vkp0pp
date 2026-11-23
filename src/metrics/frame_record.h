// Per-frame record shared by the frame governor, the metrics writer and the
// analysis tooling. All durations are nanoseconds; -1 means "not available".
#pragma once
#include <cstdint>

#include "core/time.h"

namespace bll {

enum class Bottleneck : uint8_t { Unknown, Cpu, Gpu, Present, DisplayRefresh, FrameCap, Mixed };

const char* to_string(Bottleneck b);

struct FrameRecord {
    uint64_t index = 0;
    Ticks frame_start = 0;    // end of the previous frame-start gate
    Ticks present_call = 0;
    Ticks present_return = 0;
    Ticks interval = 0;       // present_call - previous present_call
    // Waits performed after this Present, before the next frame starts.
    Ticks wait_latency = 0;   // waitable swap chain object
    Ticks wait_gpu = 0;       // GPU still busy with the previous frame
    Ticks wait_pacer = 0;     // frame limiter
    Ticks wait_jit = 0;       // adaptive just-in-time delay
    // present_call minus the most recent gamepad poll made by the game.
    Ticks input_age = -1;
    // Present call to the vblank that displayed it, for an earlier frame whose
    // statistics became available during this one.
    Ticks display_latency = -1;
    uint32_t input_polls = 0; // gamepad polls by the game since the previous Present
    uint32_t sync_interval = 0;
    uint32_t present_flags = 0;
    int32_t result = 0;       // HRESULT of Present
    Bottleneck frame_class = Bottleneck::Unknown;
    bool gpu_pending = false; // previous frame still on the GPU at the frame boundary
    bool missed_refresh = false;
};

} // namespace bll
