#include "diagnostics/crash_log.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace bll {

namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
PVOID g_handler = nullptr;
std::atomic<int> g_reports{0};
constexpr int kMaxReports = 4;

void write_line(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buf, sizeof buf - 2, fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof buf) - 3) n = static_cast<int>(sizeof buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD written = 0;
    WriteFile(g_file, buf, static_cast<DWORD>(n), &written, nullptr);
}

// "module.dll+0x1234" for an address, or the raw address if it is in no module.
void describe(DWORD64 address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = path;
        for (const wchar_t* p = path; *p; ++p)
            if (*p == L'\\' || *p == L'/') name = p + 1;
        std::snprintf(out, size, "%ls+0x%llx", name,
                      static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    } else {
        std::snprintf(out, size, "0x%016llx", static_cast<unsigned long long>(address));
    }
}

bool is_fatal(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN / fail-fast
    case 0xC0000374: // STATUS_HEAP_CORRUPTION
    case 0xC000041D: // STATUS_FATAL_USER_CALLBACK_EXCEPTION
        return true;
    default:
        return false;
    }
}

LONG CALLBACK handler(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!is_fatal(code) || g_file == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    if (g_reports.fetch_add(1) >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;

    char where[300];
    describe(reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress), where, sizeof where);
    write_line("=== exception 0x%08lX at %s, thread %lu", static_cast<unsigned long>(code), where,
               GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
        write_line("    %s of address 0x%016llx", kind == 0 ? "read" : kind == 1 ? "write" : "execute",
                   static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
    }

    // Walk the stack from the faulting context using the x64 unwind tables.
    CONTEXT ctx = *info->ContextRecord;
    for (int frame = 0; frame < 32 && ctx.Rip != 0; ++frame) {
        describe(ctx.Rip, where, sizeof where);
        write_line("    #%02d %s", frame, where);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (!function) {
            // Leaf function: the return address is on top of the stack.
            if (ctx.Rsp == 0 || IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), sizeof(DWORD64))) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, function, &ctx, &handler_data, &establisher,
                         nullptr);
    }

    // Module bases help to interpret offsets in modules without symbols.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&handler), &self);
    write_line("    bll.dll base 0x%016llx, game base 0x%016llx",
               static_cast<unsigned long long>(reinterpret_cast<DWORD64>(self)),
               static_cast<unsigned long long>(reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr))));
    FlushFileBuffers(g_file);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void crash_log_install(const std::filesystem::path& file) {
    if (g_handler) return;
    g_file = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return;
    write_line("crash log armed; this file stays almost empty unless the game crashes");
    // First in the chain so the record is written even if the game's own handler terminates the process.
    g_handler = AddVectoredExceptionHandler(1, &handler);
}

void crash_log_remove() {
    if (g_handler) RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;
}

} // namespace bll
