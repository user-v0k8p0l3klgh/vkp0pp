#include "core/time.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#endif

namespace bll {

#ifdef _WIN32
namespace {
int64_t qpc_frequency() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}
} // namespace

Ticks qpc_to_ns(int64_t qpc) {
    static const int64_t freq = qpc_frequency();
    // Split to avoid overflowing 64 bits on long uptimes.
    return (qpc / freq) * kNsPerSec + (qpc % freq) * kNsPerSec / freq;
}

Ticks now_ns() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return qpc_to_ns(c.QuadPart);
}
#else
Ticks now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
#endif

} // namespace bll
