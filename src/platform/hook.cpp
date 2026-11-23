#include "platform/hook.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "platform/pe_image.h"

namespace bll {

namespace {

bool write_pointer(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

struct PatchedSlot {
    void** slot;
    void* previous;
    void* replacement;
};

std::mutex g_patch_mutex;
std::vector<PatchedSlot> g_patched;

bool starts_with_icase(const std::string& s, const char* prefix) {
    for (size_t i = 0; prefix[i]; ++i)
        if (i >= s.size() || std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

std::wstring directory_of(HMODULE module) {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
}

} // namespace

bool VtableHook::install(void* com_object, unsigned slot, void* replacement) {
    if (!com_object || !replacement) return false;
    void** const vtable = *static_cast<void***>(com_object);
    void** const entry = &vtable[slot];
    if (*entry == replacement) return true; // already installed on this vtable
    if (slot_ != nullptr && slot_ != entry) {
        BLL_WARN("vtable hook: a different implementation class was encountered; keeping the first");
        return false;
    }
    // Publish the original before the slot changes: another thread may enter
    // the replacement immediately.
    original_.store(*entry, std::memory_order_release);
    if (!write_pointer(entry, replacement)) {
        BLL_ERROR("vtable hook: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    slot_ = entry;
    replacement_ = replacement;
    return true;
}

void VtableHook::remove() {
    if (!slot_) return;
    // Only restore if nobody hooked on top of us in the meantime.
    if (*slot_ == replacement_) write_pointer(slot_, original_.load());
    slot_ = nullptr;
}

size_t patch_imports(const char* dll_prefix, const char* function, void* replacement, void** original) {
    HMODULE modules[1024];
    DWORD needed = 0;
    const HANDLE process = GetCurrentProcess();
    if (!EnumProcessModules(process, modules, sizeof modules, &needed)) return 0;
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), std::size(modules));
    const std::wstring exe_dir = directory_of(GetModuleHandleW(nullptr));

    size_t patched = 0;
    std::lock_guard lock(g_patch_mutex);
    for (size_t m = 0; m < count; ++m) {
        if (modules[m] != GetModuleHandleW(nullptr) && _wcsicmp(directory_of(modules[m]).c_str(), exe_dir.c_str()) != 0)
            continue; // system and third-party modules are left alone
        MODULEINFO mi{};
        if (!GetModuleInformation(process, modules[m], &mi, sizeof mi)) continue;
        const auto* base = static_cast<const uint8_t*>(mi.lpBaseOfDll);
        const PeInfo info = parse_pe_image(base, mi.SizeOfImage);
        for (const PeImport& imp : info.imports) {
            if (imp.function != function || !starts_with_icase(imp.dll, dll_prefix)) continue;
            // Resolve the real function ourselves: a delay-load slot still
            // holds its resolver stub, which would overwrite our patch.
            const std::wstring dll(imp.dll.begin(), imp.dll.end());
            HMODULE target = GetModuleHandleW(dll.c_str());
            if (!target) target = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            void* const real = target ? reinterpret_cast<void*>(GetProcAddress(target, function)) : nullptr;
            if (!real) {
                BLL_WARN("import hook: cannot resolve %s!%s", imp.dll.c_str(), function);
                continue;
            }
            void** const slot = reinterpret_cast<void**>(const_cast<uint8_t*>(base) + imp.iat_rva);
            void* const previous = *slot;
            if (previous == replacement) continue;
            if (original && !*original) *original = real;
            if (write_pointer(slot, replacement)) {
                g_patched.push_back({slot, imp.delay_load ? previous : real, replacement});
                ++patched;
            }
        }
    }
    return patched;
}

void unpatch_imports() {
    std::lock_guard lock(g_patch_mutex);
    for (const PatchedSlot& p : g_patched)
        if (*p.slot == p.replacement) write_pointer(p.slot, p.previous);
    g_patched.clear();
}

} // namespace bll
