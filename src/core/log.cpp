#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "core/time.h"

namespace bll {

namespace {
std::mutex g_mutex;
std::FILE* g_file = nullptr;
Ticks g_start = 0;
} // namespace

void log_open(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    if (g_file) std::fclose(g_file);
#ifdef _WIN32
    g_file = _wfopen(file.c_str(), L"w");
#else
    g_file = std::fopen(file.c_str(), "w");
#endif
    g_start = now_ns();
}

void log_close() {
    std::lock_guard lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

void log(LogLevel level, const char* fmt, ...) {
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    static const char* const names[] = {"INFO ", "WARN ", "ERROR"};
    std::fprintf(g_file, "[%10.3f] %s ", to_ms(now_ns() - g_start) / 1000.0, names[static_cast<int>(level)]);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}

} // namespace bll
