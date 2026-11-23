// Records fatal exceptions in the game process (access violations, stack
// overflows, fail-fast and the like) to crash.log: exception code, faulting
// module and offset, and an unwound call stack as module+offset pairs.
// bll.dll is shipped with debug information, so its offsets can be resolved
// to source lines.
//
// The handler only observes: it always lets the exception continue to the
// game's own handlers. It avoids the logger (whose lock may be held by the
// crashing thread) and writes with WriteFile into a pre-opened handle.
#pragma once
#include <filesystem>

namespace bll {

void crash_log_install(const std::filesystem::path& file);
void crash_log_remove();

} // namespace bll
