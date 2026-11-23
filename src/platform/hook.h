// The two redirection mechanisms the DLL uses. Both replace a single pointer
// in a table and put it back on removal; no machine code is rewritten and no
// game memory other than its import table is touched.
#pragma once
#include <atomic>
#include <cstddef>

namespace bll {

// Replaces one slot of a COM object's vtable. The vtable is shared by every
// object of the same implementation class, so one installation covers all of
// them.
class VtableHook {
public:
    // Returns true if the slot now points at `replacement`.
    bool install(void* com_object, unsigned slot, void* replacement);
    void remove();

    bool installed() const { return slot_ != nullptr; }
    template <typename Fn>
    Fn original() const {
        return reinterpret_cast<Fn>(original_.load(std::memory_order_acquire));
    }

private:
    void** slot_ = nullptr;
    void* replacement_ = nullptr;
    std::atomic<void*> original_{nullptr};
};

// Redirects imports of `function` from DLLs whose name starts with
// `dll_prefix` (case-insensitive) in the main executable and in every module
// located in the executable's directory. Covers normal and delay-load
// imports. Returns the number of import slots patched; `*original` receives
// the real function.
size_t patch_imports(const char* dll_prefix, const char* function, void* replacement, void** original);

// Restores every slot patched by patch_imports.
void unpatch_imports();

} // namespace bll
