// Small thread-safe file logger. Not for per-frame use: hot paths publish to
// the metrics ring and only log on state changes.
#pragma once
#include <filesystem>

namespace bll {

enum class LogLevel { Info, Warn, Error };

void log_open(const std::filesystem::path& file);
void log_close();

#if defined(__MINGW32__)
__attribute__((format(gnu_printf, 2, 3)))
#elif defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void log(LogLevel level, const char* fmt, ...);

#define BLL_INFO(...) ::bll::log(::bll::LogLevel::Info, __VA_ARGS__)
#define BLL_WARN(...) ::bll::log(::bll::LogLevel::Warn, __VA_ARGS__)
#define BLL_ERROR(...) ::bll::log(::bll::LogLevel::Error, __VA_ARGS__)

} // namespace bll
