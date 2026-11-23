// Monotonic time in nanoseconds. All algorithm classes take timestamps as
// arguments so they can be driven by a virtual clock in tests.
#pragma once
#include <cstdint>

namespace bll {

using Ticks = int64_t; // nanoseconds on a monotonic clock

constexpr Ticks kNsPerUs = 1'000;
constexpr Ticks kNsPerMs = 1'000'000;
constexpr Ticks kNsPerSec = 1'000'000'000;

// QueryPerformanceCounter on Windows, steady_clock elsewhere.
Ticks now_ns();

#ifdef _WIN32
// Converts a raw QueryPerformanceCounter value (as found in DXGI frame
// statistics) to the time base of now_ns().
Ticks qpc_to_ns(int64_t qpc);
#endif

constexpr double to_ms(Ticks t) { return static_cast<double>(t) / 1e6; }

} // namespace bll
