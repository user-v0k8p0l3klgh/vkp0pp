// bll-harness: a small windowed Direct3D 11 application with a game-shaped
// loop (poll gamepad, work, clear, Present). It exists to exercise bll.dll's
// real hook paths without the game:
//
//   bll-harness [--dll <bll.dll>] [--late] [--frames N] [--vsync 0|1]
//               [--work-ms N] [--resize]
//
//   --dll     load bll.dll before the swap chain is created (as the launcher does)
//   --late    load it after the swap chain exists (late-attach path)
//   --resize  call ResizeBuffers half-way, with the flags the app itself used
//
// Gamepad input comes from XInputGetState. Next to the scripted xinput1_4.dll
// (fake_xinput.c) the harness knows exactly how many taps were emitted and
// reports how many of them its once-per-frame poll saw.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <xinput.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/time.h"

namespace {

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

template <typename T>
void safe_release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

struct Options {
    std::wstring dll;
    bool late = false;
    bool resize = false;
    int frames = 600;
    UINT vsync = 1;
    int work_ms = 2;
};

HMODULE load_bll(const std::wstring& path) {
    const HMODULE module = LoadLibraryW(path.c_str());
    if (!module) {
        std::printf("harness: LoadLibrary(bll.dll) failed (%lu)\n", GetLastError());
        return nullptr;
    }
    using WaitReady = int (*)(unsigned);
    const auto wait_ready = reinterpret_cast<WaitReady>(reinterpret_cast<void*>(GetProcAddress(module, "bll_wait_ready")));
    const int ready = wait_ready ? wait_ready(10000) : 0;
    std::printf("harness: bll.dll loaded, ready=%d\n", ready);
    return module;
}

} // namespace

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--dll") opt.dll = value();
        else if (a == L"--late") opt.late = true;
        else if (a == L"--resize") opt.resize = true;
        else if (a == L"--frames") opt.frames = _wtoi(value().c_str());
        else if (a == L"--vsync") opt.vsync = static_cast<UINT>(_wtoi(value().c_str()));
        else if (a == L"--work-ms") opt.work_ms = _wtoi(value().c_str());
    }

    HMODULE bll = nullptr;
    if (!opt.dll.empty() && !opt.late) bll = load_bll(opt.dll);

    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BllHarness";
    RegisterClassW(&wc);
    const HWND hwnd = CreateWindowW(wc.lpszClassName, L"bll-harness", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 640, 360,
                                    nullptr, nullptr, wc.hInstance, nullptr);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                   nullptr, &context);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                               &context);
    if (FAILED(hr)) {
        std::printf("harness: D3D11CreateDevice failed (0x%08lX)\n", static_cast<unsigned long>(hr));
        return 3;
    }

    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    dxgi_device->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 640;
    desc.Height = 360;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* chain = nullptr;
    hr = factory->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr, &chain);
    if (FAILED(hr) || !chain) {
        std::printf("harness: CreateSwapChainForHwnd failed (0x%08lX)\n", static_cast<unsigned long>(hr));
        return 3;
    }
    DXGI_SWAP_CHAIN_DESC1 actual{};
    chain->GetDesc1(&actual);
    std::printf("harness: swap chain created, flags asked 0x%X, actual 0x%X\n", desc.Flags, actual.Flags);

    if (!opt.dll.empty() && opt.late) {
        bll = load_bll(opt.dll);
        if (bll) {
            using Attach = void (*)();
            if (const auto attach = reinterpret_cast<Attach>(reinterpret_cast<void*>(GetProcAddress(bll, "bll_attach_existing"))))
                attach();
        }
    }

    // Present only when the scripted controller is in use.
    using Completed = void(WINAPI*)(DWORD*, DWORD*, LONGLONG*);
    const auto completed = reinterpret_cast<Completed>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"xinput1_4.dll"), "FakeXInputCompleted")));
    DWORD taps_before = 0, pulls_before = 0, taps_after = 0, pulls_after = 0;
    LONGLONG device_now = 0;
    if (completed) completed(&taps_before, &pulls_before, &device_now);

    int failures = 0, taps_seen = 0, pulls_seen = 0, resize_result = 0;
    bool a_down = false, trigger_down = false;
    long long max_analog_age_ms = 0;
    std::vector<double> frame_ms;
    bll::Ticks previous = bll::now_ns();

    for (int frame = 0; frame < opt.frames; ++frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

        XINPUT_STATE pad{};
        if (XInputGetState(0, &pad) == ERROR_SUCCESS) {
            const bool a = (pad.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
            const bool trigger = pad.Gamepad.bRightTrigger > 128;
            taps_seen += a && !a_down;
            pulls_seen += trigger && !trigger_down;
            a_down = a;
            trigger_down = trigger;
            if (completed) {
                DWORD t, p;
                completed(&t, &p, &device_now);
                // The stick encodes the device time of the read that produced it.
                const long long age = (device_now % 30000 - pad.Gamepad.sThumbRX + 30000) % 30000;
                max_analog_age_ms = std::max(max_analog_age_ms, age);
            }
        }

        if (opt.work_ms > 0) Sleep(static_cast<DWORD>(opt.work_ms));

        ID3D11Texture2D* back_buffer = nullptr;
        ID3D11RenderTargetView* view = nullptr;
        if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) &&
            SUCCEEDED(device->CreateRenderTargetView(back_buffer, nullptr, &view))) {
            const float shade = static_cast<float>(frame % 120) / 120.0f;
            const float color[4] = {shade, 0.2f, 1.0f - shade, 1.0f};
            context->ClearRenderTargetView(view, color);
        }
        safe_release(view);
        safe_release(back_buffer);

        hr = chain->Present(opt.vsync, 0);
        if (FAILED(hr)) {
            if (failures++ == 0) std::printf("harness: Present failed (0x%08lX) at frame %d\n", static_cast<unsigned long>(hr), frame);
        }
        const bll::Ticks now = bll::now_ns();
        frame_ms.push_back(bll::to_ms(now - previous));
        previous = now;

        if (opt.resize && frame == opt.frames / 2) {
            context->ClearState();
            // Exactly what an unaware application does: its own (zero) flags.
            hr = chain->ResizeBuffers(0, 800, 450, DXGI_FORMAT_UNKNOWN, 0);
            resize_result = static_cast<int>(hr);
            std::printf("harness: ResizeBuffers -> 0x%08lX\n", static_cast<unsigned long>(hr));
            if (FAILED(hr)) ++failures;
        }
    }
    if (completed) completed(&taps_after, &pulls_after, &device_now);

    std::sort(frame_ms.begin(), frame_ms.end());
    const double median = frame_ms.empty() ? 0 : frame_ms[frame_ms.size() / 2];
    const double p99 = frame_ms.empty() ? 0 : frame_ms[frame_ms.size() * 99 / 100];
    std::printf("RESULT frames=%d present_failures=%d frame_ms_median=%.2f frame_ms_p99=%.2f resize=0x%08X\n", opt.frames,
                failures, median, p99, static_cast<unsigned>(resize_result));
    if (completed)
        std::printf("RESULT taps_emitted=%lu taps_seen=%d pulls_emitted=%lu pulls_seen=%d max_analog_age_ms=%lld\n",
                    taps_after - taps_before, taps_seen, pulls_after - pulls_before, pulls_seen, max_analog_age_ms);

    safe_release(chain);
    safe_release(factory);
    safe_release(adapter);
    safe_release(dxgi_device);
    safe_release(context);
    safe_release(device);
    DestroyWindow(hwnd);
    // Give the DLL's diagnostics thread a moment to flush its final records.
    Sleep(300);
    return failures == 0 ? 0 : 1;
}
