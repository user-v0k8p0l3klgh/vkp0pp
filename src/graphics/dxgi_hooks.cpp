// DXGI integration: swap-chain creation, Present, ResizeBuffers.
//
// Everything here goes through documented DXGI interfaces reached by vtable
// redirection; no address inside the game is used. The same code therefore
// serves the game's CoreWindow swap chain and the HWND swap chain of the test
// harness.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_5.h>

#include <atomic>
#include <mutex>

#include "core/log.h"
#include "graphics/gpu_sync.h"
#include "platform/abi.h"
#include "platform/hook.h"
#include "runtime.h"
#include "timing/refresh_estimator.h"

namespace bll {

namespace {

using CreateForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*,
                                                          const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                          IDXGISwapChain1**);
using CreateForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                    IDXGISwapChain1**);
using CreateForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                           IDXGIOutput*, IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);

VtableHook g_hook_core_window, g_hook_hwnd, g_hook_composition, g_hook_present, g_hook_present1, g_hook_resize,
    g_hook_release;

// State of the one swap chain being governed (the game has exactly one).
struct ChainState {
    UINT added_flags = 0;   // creation flags added by us; must be repeated on ResizeBuffers
    bool tearing = false;   // created with ALLOW_TEARING (by the game or by us)
    bool modify_ok = true;  // cleared in safe mode when DXGI rejects a modified call
    bool is_d3d11 = false;
    HANDLE latency_event = nullptr;
    IDXGIDevice1* dxgi_device = nullptr;
    UINT original_device_latency = 0;
    bool device_latency_applied = false;
    GpuSync gpu;
    RefreshEstimator refresh;
    Ticks pushed_period = 0;

    struct Submitted {
        UINT count;
        Ticks call;
    };
    Submitted submitted[32] = {};
    unsigned submitted_head = 0;
    UINT last_displayed = 0;

    uint64_t frames = 0;
    UINT last_logged_sync = ~0u;
    UINT last_logged_flags = ~0u;
};

std::mutex g_attach_mutex;
ChainState g_state;
std::atomic<IDXGISwapChain*> g_primary{nullptr};
std::atomic<bool> g_seen{false};
std::atomic<bool> g_adopt_existing{false};
thread_local bool t_in_present = false;

struct ChainGate final : GateDevice {
    Wait wait_frame_latency(Ticks timeout) override {
        if (!g_state.latency_event) return Wait::Unavailable;
        const DWORD ms = static_cast<DWORD>(timeout / kNsPerMs);
        return WaitForSingleObjectEx(g_state.latency_event, ms, FALSE) == WAIT_OBJECT_0 ? Wait::Signaled
                                                                                         : Wait::Timeout;
    }
    bool gpu_frame_pending() override { return g_state.gpu.pending(); }
};
ChainGate g_gate;

bool is_flip_model(DXGI_SWAP_EFFECT effect) {
    return effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
}

bool tearing_supported(IDXGIFactory2* factory) {
    IDXGIFactory5* f5 = nullptr;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&f5)))) return false;
    BOOL allowed = FALSE;
    const HRESULT hr = f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed, sizeof allowed);
    f5->Release();
    return SUCCEEDED(hr) && allowed;
}

void release_state_locked() {
    ChainState& s = g_state;
    if (s.dxgi_device) {
        if (s.device_latency_applied) s.dxgi_device->SetMaximumFrameLatency(s.original_device_latency);
        s.dxgi_device->Release();
    }
    s.gpu.shutdown();
    // The application owns the handle returned by GetFrameLatencyWaitableObject.
    if (s.latency_event) CloseHandle(s.latency_event);
    s.added_flags = 0;
    s.tearing = s.is_d3d11 = s.device_latency_applied = false;
    s.modify_ok = true;
    s.latency_event = nullptr;
    s.dxgi_device = nullptr;
    s.original_device_latency = 0;
    s.refresh = RefreshEstimator{};
    s.pushed_period = 0;
    s.submitted_head = 0;
    s.last_displayed = 0;
    s.frames = 0;
    s.last_logged_sync = s.last_logged_flags = ~0u;
}

void detach(IUnknown* chain) {
    std::lock_guard lock(g_attach_mutex);
    if (static_cast<IUnknown*>(g_primary.load()) != chain) return;
    g_primary.store(nullptr);
    release_state_locked();
    BLL_INFO("swap chain %p destroyed", static_cast<void*>(chain));
}

ULONG STDMETHODCALLTYPE hk_release(IUnknown* self);
HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* self, UINT sync, UINT flags);
HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p);
HRESULT STDMETHODCALLTYPE hk_resize_buffers(IDXGISwapChain* self, UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                            UINT flags);

void install_swap_chain_hooks(IDXGISwapChain* chain) {
    g_hook_present.install(chain, bll_slot_swapchain_present, reinterpret_cast<void*>(&hk_present));
    g_hook_resize.install(chain, bll_slot_swapchain_resize_buffers, reinterpret_cast<void*>(&hk_resize_buffers));
    g_hook_release.install(chain, bll_slot_release, reinterpret_cast<void*>(&hk_release));
    IDXGISwapChain1* chain1 = nullptr;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1)))) {
        g_hook_present1.install(chain1, bll_slot_swapchain_present1, reinterpret_cast<void*>(&hk_present1));
        // Through the original Release: this reference must not look like the chain's last one.
        if (const auto release = g_hook_release.original<ReleaseFn>()) release(chain1);
        else chain1->Release();
    }
}

// Makes `chain` the governed swap chain. `added_flags` are creation flags
// that were added by the creation hook (0 when adopting an existing chain).
void attach(IDXGISwapChain* chain, UINT creation_flags, UINT added_flags, const char* origin) {
    Runtime* rt = runtime();
    std::lock_guard lock(g_attach_mutex);
    release_state_locked();
    ChainState& s = g_state;
    s.added_flags = added_flags;
    s.tearing = (creation_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

    install_swap_chain_hooks(chain);
    const auto release = g_hook_release.original<ReleaseFn>();
    // References taken on the chain here are dropped through the original
    // Release so that our own bookkeeping is not mistaken for destruction.
    auto drop = [&](IUnknown* p) { release ? release(p) : p->Release(); };

    ID3D11Device* d3d11 = nullptr;
    ID3D12Device* d3d12 = nullptr;
    if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&d3d11)))) {
        s.is_d3d11 = true;
        BLL_INFO("graphics backend: Direct3D 11, feature level 0x%04X", static_cast<unsigned>(d3d11->GetFeatureLevel()));
    } else if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&d3d12)))) {
        BLL_WARN("graphics backend: Direct3D 12. This is not the 1.16.100 renderer; Direct3D 11 features stay off");
        d3d12->Release();
    } else {
        BLL_WARN("graphics backend: unknown device type");
    }

    const Config& cfg = rt->cfg;
    if ((creation_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) && cfg.waitable_swap_chain) {
        IDXGISwapChain2* chain2 = nullptr;
        if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain2)))) {
            // Swap-chain level latency: only valid on waitable swap chains.
            const HRESULT hr = chain2->SetMaximumFrameLatency(static_cast<UINT>(cfg.target_queue_depth));
            s.latency_event = chain2->GetFrameLatencyWaitableObject();
            BLL_INFO("waitable swap chain: SetMaximumFrameLatency(%d) -> 0x%08lX, waitable object %s",
                     cfg.target_queue_depth, static_cast<unsigned long>(hr), s.latency_event ? "acquired" : "missing");
            drop(chain2);
        } else {
            BLL_WARN("the swap chain does not expose IDXGISwapChain2; no waitable object on this system");
        }
    }
    rt->governor.set_waitable_enabled(s.latency_event != nullptr);

    if (s.is_d3d11 && rt->active && cfg.low_latency_queue) {
        // Device level latency: the mechanism for ordinary (non-waitable) D3D11 swap chains.
        if (SUCCEEDED(d3d11->QueryInterface(IID_PPV_ARGS(&s.dxgi_device)))) {
            s.dxgi_device->GetMaximumFrameLatency(&s.original_device_latency);
            const HRESULT hr = s.dxgi_device->SetMaximumFrameLatency(static_cast<UINT>(cfg.target_queue_depth));
            s.device_latency_applied = SUCCEEDED(hr);
            BLL_INFO("IDXGIDevice1::SetMaximumFrameLatency(%d) -> 0x%08lX (was %u)", cfg.target_queue_depth,
                     static_cast<unsigned long>(hr), s.original_device_latency);
        }
    }

    bool gpu_sync = false;
    if (s.is_d3d11 && rt->active && cfg.gpu_sync) gpu_sync = s.gpu.init(d3d11);
    rt->governor.set_gpu_sync_enabled(gpu_sync);
    if (d3d11) d3d11->Release();

    g_primary.store(chain);
    g_seen.store(true);
    BLL_INFO("governing swap chain %p (%s): creation flags 0x%X, added 0x%X, tearing=%d, waitable=%d, gpu_sync=%d",
             static_cast<void*>(chain), origin, creation_flags, added_flags, s.tearing, s.latency_event != nullptr,
             gpu_sync);
}

void log_desc(const char* api, const DXGI_SWAP_CHAIN_DESC1& d) {
    BLL_INFO("%s: %ux%u format=%d buffers=%u swap_effect=%d scaling=%d flags=0x%X sample_count=%u", api, d.Width,
             d.Height, static_cast<int>(d.Format), d.BufferCount, static_cast<int>(d.SwapEffect),
             static_cast<int>(d.Scaling), d.Flags, d.SampleDesc.Count);
}

// Shared by both creation hooks. `create` calls the original with a description.
template <typename Create>
HRESULT create_swap_chain(const char* api, IDXGIFactory2* factory, const DXGI_SWAP_CHAIN_DESC1* desc, bool windowed,
                          IDXGISwapChain1** out, Create&& create) {
    Runtime* rt = runtime();
    if (!rt || !desc || !out) return create(desc);

    log_desc(api, *desc);
    DXGI_SWAP_CHAIN_DESC1 modified = *desc;
    UINT added = 0;
    if (rt->active) {
        const bool flip = is_flip_model(desc->SwapEffect);
        if (!flip && (rt->cfg.waitable_swap_chain || rt->cfg.allow_tearing))
            BLL_WARN("%s: not a flip-model swap chain; waitable object and tearing are unavailable", api);
        // Both flags can only be chosen at creation; neither can be added by ResizeBuffers later.
        if (rt->cfg.waitable_swap_chain && flip && windowed &&
            !(desc->Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
            added |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (rt->cfg.allow_tearing && flip && !(desc->Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
            if (tearing_supported(factory)) added |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            else BLL_WARN("%s: the system reports no tearing support; AllowTearing ignored", api);
        }
        modified.Flags |= added;
    }

    HRESULT hr = create(&modified);
    if (FAILED(hr) && added != 0) {
        BLL_WARN("%s failed with added flags 0x%X (0x%08lX); retrying exactly as the game asked", api, added,
                 static_cast<unsigned long>(hr));
        added = 0;
        modified = *desc;
        hr = create(desc);
    }
    if (SUCCEEDED(hr) && *out) attach(*out, modified.Flags, added, api);
    else BLL_WARN("%s -> 0x%08lX", api, static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_for_core_window(IDXGIFactory2* self, IUnknown* device, IUnknown* window,
                                                    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output,
                                                    IDXGISwapChain1** out) {
    const auto original = g_hook_core_window.original<CreateForCoreWindowFn>();
    return create_swap_chain("CreateSwapChainForCoreWindow", self, desc, true, out,
                             [&](const DXGI_SWAP_CHAIN_DESC1* d) { return original(self, device, window, d, output, out); });
}

HRESULT STDMETHODCALLTYPE hk_create_for_hwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output,
                                             IDXGISwapChain1** out) {
    const auto original = g_hook_hwnd.original<CreateForHwndFn>();
    const bool windowed = !fullscreen || fullscreen->Windowed;
    return create_swap_chain("CreateSwapChainForHwnd", self, desc, windowed, out, [&](const DXGI_SWAP_CHAIN_DESC1* d) {
        return original(self, device, hwnd, d, fullscreen, output, out);
    });
}

// XAML-hosted UWP applications create their swap chain this way (SwapChainPanel).
HRESULT STDMETHODCALLTYPE hk_create_for_composition(IDXGIFactory2* self, IUnknown* device,
                                                    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output,
                                                    IDXGISwapChain1** out) {
    const auto original = g_hook_composition.original<CreateForCompositionFn>();
    return create_swap_chain("CreateSwapChainForComposition", self, desc, true, out,
                             [&](const DXGI_SWAP_CHAIN_DESC1* d) { return original(self, device, d, output, out); });
}

ULONG STDMETHODCALLTYPE hk_release(IUnknown* self) {
    const ULONG remaining = g_hook_release.original<ReleaseFn>()(self);
    if (remaining == 0 && static_cast<IUnknown*>(g_primary.load(std::memory_order_relaxed)) == self) detach(self);
    return remaining;
}

// Reads DXGI frame statistics: refresh period and, for the frame that most
// recently reached the screen, the time from its Present call to its vblank.
Ticks update_frame_statistics(IDXGISwapChain* chain, ChainState& s, Runtime& rt, Ticks present_call) {
    UINT count = 0;
    if (SUCCEEDED(chain->GetLastPresentCount(&count))) {
        s.submitted[s.submitted_head % std::size(s.submitted)] = {count, present_call};
        ++s.submitted_head;
    }
    DXGI_FRAME_STATISTICS st{};
    if (FAILED(chain->GetFrameStatistics(&st)) || st.SyncQPCTime.QuadPart == 0) return -1;

    const Ticks sync_time = qpc_to_ns(st.SyncQPCTime.QuadPart);
    s.refresh.observe(st.SyncRefreshCount, sync_time);
    const Ticks period = s.refresh.period();
    if (!s.refresh.measured()) return -1;

    // Only forward meaningful changes: the pacer re-anchors when its interval changes.
    const Ticks diff = period > s.pushed_period ? period - s.pushed_period : s.pushed_period - period;
    if (diff * 500 > period) {
        s.pushed_period = period;
        rt.governor.set_refresh_period(period);
        BLL_INFO("display refresh measured: %.3f Hz (period %.3f ms)", 1e9 / static_cast<double>(period), to_ms(period));
    }

    if (st.PresentCount == s.last_displayed) return -1;
    s.last_displayed = st.PresentCount;
    for (const ChainState::Submitted& e : s.submitted) {
        if (e.count != st.PresentCount || e.call == 0) continue;
        const auto vblanks_ago = static_cast<int32_t>(st.SyncRefreshCount - st.PresentRefreshCount);
        const Ticks shown = sync_time - static_cast<Ticks>(vblanks_ago) * period;
        const Ticks latency = shown - e.call;
        return latency >= 0 && latency < kNsPerSec ? latency : -1;
    }
    return -1;
}

template <typename Call>
HRESULT present_common(IDXGISwapChain* self, UINT sync, UINT flags, Call&& call) {
    Runtime* rt = runtime();
    if (!rt || t_in_present) return call(sync, flags);

    if (self != g_primary.load(std::memory_order_relaxed)) {
        // A chain created before we were loaded: adopt it on its first Present.
        if (g_primary.load() != nullptr || !g_adopt_existing.exchange(false)) return call(sync, flags);
        DXGI_SWAP_CHAIN_DESC desc{};
        self->GetDesc(&desc);
        attach(self, desc.Flags, 0, "existing swap chain, adopted at Present");
    }

    ChainState& s = g_state;
    const bool test_only = (flags & DXGI_PRESENT_TEST) != 0;
    UINT new_sync = sync, new_flags = flags;
    if (rt->active && s.modify_ok && !test_only) {
        if (rt->cfg.disable_vsync) new_sync = 0;
        // ALLOW_TEARING is only valid with sync interval 0 on a chain created for tearing.
        if (new_sync == 0 && s.tearing && rt->cfg.allow_tearing) new_flags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    if (!test_only) s.gpu.mark_frame_end();

    t_in_present = true;
    PresentSample sample;
    sample.call = now_ns();
    HRESULT hr = call(new_sync, new_flags);
    if (hr == DXGI_ERROR_INVALID_CALL && (new_sync != sync || new_flags != flags)) {
        // Our modification was rejected and nothing was presented: fall back to
        // the game's own arguments and stop modifying.
        BLL_WARN("Present(%u, 0x%X) rejected; reverting to the game's arguments (%u, 0x%X)", new_sync, new_flags, sync,
                 flags);
        if (rt->cfg.safe_mode) s.modify_ok = false;
        new_sync = sync;
        new_flags = flags;
        hr = call(sync, flags);
    }
    sample.ret = now_ns();
    t_in_present = false;

    sample.sync_interval = new_sync;
    sample.flags = new_flags;
    sample.result = static_cast<int32_t>(hr);
    sample.displayed = !test_only && SUCCEEDED(hr) && hr != DXGI_STATUS_OCCLUDED;

    if (new_sync != s.last_logged_sync || new_flags != s.last_logged_flags) {
        // Logged on change only: shows how the game presents (e.g. whether gfx_vsync has any effect).
        BLL_INFO("Present: game asked (sync=%u flags=0x%X), issued (sync=%u flags=0x%X) -> 0x%08lX", sync, flags,
                 new_sync, new_flags, static_cast<unsigned long>(hr));
        s.last_logged_sync = new_sync;
        s.last_logged_flags = new_flags;
    }

    Ticks display_latency = -1;
    if (sample.displayed) {
        ++s.frames;
        display_latency = update_frame_statistics(self, s, *rt, sample.call);
        // The game or a device reset may put the latency back; check now and then.
        if (s.device_latency_applied && (s.frames & 0xFF) == 0) {
            UINT current = 0;
            if (SUCCEEDED(s.dxgi_device->GetMaximumFrameLatency(&current)) &&
                current != static_cast<UINT>(rt->cfg.target_queue_depth))
                s.dxgi_device->SetMaximumFrameLatency(static_cast<UINT>(rt->cfg.target_queue_depth));
        }
    }

    FrameRecord record = rt->governor.on_present(g_gate, sample);
    record.display_latency = display_latency;
    rt->metrics.submit(record);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* self, UINT sync, UINT flags) {
    const auto original = g_hook_present.original<PresentFn>();
    return present_common(self, sync, flags, [&](UINT s, UINT f) { return original(self, s, f); });
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    const auto original = g_hook_present1.original<Present1Fn>();
    return present_common(self, sync, flags, [&](UINT s, UINT f) { return original(self, s, f, p); });
}

HRESULT STDMETHODCALLTYPE hk_resize_buffers(IDXGISwapChain* self, UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                            UINT flags) {
    const auto original = g_hook_resize.original<ResizeBuffersFn>();
    if (!runtime() || self != g_primary.load()) return original(self, count, w, h, format, flags);

    // The waitable-object and tearing flags are fixed at creation and must be
    // passed again on every resize, otherwise the call fails.
    const UINT with_added = flags | g_state.added_flags;
    HRESULT hr = original(self, count, w, h, format, with_added);
    if (FAILED(hr) && with_added != flags) {
        BLL_WARN("ResizeBuffers with flags 0x%X failed (0x%08lX); retrying with the game's flags 0x%X", with_added,
                 static_cast<unsigned long>(hr), flags);
        hr = original(self, count, w, h, format, flags);
    }
    BLL_INFO("ResizeBuffers(%u, %ux%u, format %d, flags 0x%X) -> 0x%08lX", count, w, h, static_cast<int>(format),
             with_added, static_cast<unsigned long>(hr));
    return hr;
}

} // namespace

bool swap_chain_seen() { return g_seen.load(); }

bool install_dxgi_hooks() {
    IDXGIFactory2* factory = nullptr;
    const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        BLL_ERROR("CreateDXGIFactory1 failed (0x%08lX); graphics hooks not installed", static_cast<unsigned long>(hr));
        return false;
    }
    const bool a = g_hook_core_window.install(factory, bll_slot_factory_create_for_core_window,
                                              reinterpret_cast<void*>(&hk_create_for_core_window));
    const bool b = g_hook_hwnd.install(factory, bll_slot_factory_create_for_hwnd,
                                       reinterpret_cast<void*>(&hk_create_for_hwnd));
    const bool c = g_hook_composition.install(factory, bll_slot_factory_create_for_composition,
                                              reinterpret_cast<void*>(&hk_create_for_composition));
    factory->Release();
    BLL_INFO("DXGI factory hooks: CreateSwapChainForCoreWindow=%d CreateSwapChainForHwnd=%d "
             "CreateSwapChainForComposition=%d",
             a, b, c);
    return a && b && c;
}

void attach_to_existing_swap_chain() {
    static std::atomic<bool> tried{false};
    if (g_seen.load() || tried.exchange(true)) return;
    // Any swap chain exposes the vtable shared by all of them. A composition
    // swap chain needs no window, which is what a UWP process requires; a
    // hidden window is the fall-back where composition swap chains do not exist.
    ID3D11Device* device = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* dummy = nullptr;
    HWND window = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                   nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                               nullptr);
    if (SUCCEEDED(hr)) hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = desc.Height = 8;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        // The originals are called directly: this throw-away chain must not be governed.
        const auto create_composition = g_hook_composition.original<CreateForCompositionFn>();
        hr = create_composition ? create_composition(factory, device, &desc, nullptr, &dummy)
                                : factory->CreateSwapChainForComposition(device, &desc, nullptr, &dummy);
        if (FAILED(hr)) {
            window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, nullptr, nullptr);
            if (window) {
                desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
                const auto create = g_hook_hwnd.original<CreateForHwndFn>();
                hr = create ? create(factory, device, window, &desc, nullptr, nullptr, &dummy)
                            : factory->CreateSwapChainForHwnd(device, window, &desc, nullptr, nullptr, &dummy);
            }
        }
    }
    if (SUCCEEDED(hr) && dummy) {
        g_adopt_existing.store(true);
        install_swap_chain_hooks(dummy);
        BLL_INFO("late attach: Present hooked through a temporary swap chain; creation-time features "
                 "(waitable object, tearing flag) are unavailable until the game recreates its swap chain");
    } else {
        BLL_WARN("late attach failed (0x%08lX); graphics features inactive this session", static_cast<unsigned long>(hr));
    }
    if (dummy) {
        if (const auto release = g_hook_release.original<ReleaseFn>()) release(dummy);
        else dummy->Release();
    }
    if (factory) factory->Release();
    if (device) device->Release();
    if (window) DestroyWindow(window);
}

void remove_dxgi_hooks() {
    g_hook_core_window.remove();
    g_hook_hwnd.remove();
    g_hook_composition.remove();
    g_hook_present.remove();
    g_hook_present1.remove();
    g_hook_resize.remove();
    g_hook_release.remove();
    std::lock_guard lock(g_attach_mutex);
    g_primary.store(nullptr);
    release_state_locked();
}

std::string graphics_status() {
    char buf[200];
    const ChainState& s = g_state;
    std::snprintf(buf, sizeof buf, "d3d11=%d waitable=%d tearing=%d device_latency=%d modify_ok=%d", s.is_d3d11,
                  s.latency_event != nullptr, s.tearing, s.device_latency_applied, s.modify_ok);
    return buf;
}

} // namespace bll
