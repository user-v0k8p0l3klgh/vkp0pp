// Wine visual check: a D3D11 window that loads winwf_test.dll, presses Alt+G
// and clicks the on/off button, writing marker files so a script can screenshot
// each state.
//   x86_64-w64-mingw32-gcc -O1 -o d3dhost.exe tests/d3dhost.c -ld3d11 -ldxgi -luser32 -lgdi32

#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <stdio.h>
#include "fakemc.h"

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *sc;
static ID3D11RenderTargetView *rtv;

static void key(WORD vk, int up) {
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}

static void frames(int ms);

static void click(int x, int y) {
    INPUT in[3] = {0};
    for (int i = 0; i < 3; i++) in[i].type = INPUT_MOUSE;
    in[0].mi.dx = x * 65535 / (GetSystemMetrics(SM_CXSCREEN) - 1);
    in[0].mi.dy = y * 65535 / (GetSystemMetrics(SM_CYSCREEN) - 1);
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[0], sizeof in[0]); frames(150);
    SendInput(1, &in[1], sizeof in[1]); frames(80);
    SendInput(1, &in[2], sizeof in[2]);
}

static void alt_g(void) {
    key(VK_MENU, 0); Sleep(60); key('G', 0); Sleep(120); key('G', 1); Sleep(30); key(VK_MENU, 1);
}

static void frames(int ms) {
    DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        float c[4] = {0.25f, 0.45f, 0.75f, 1};
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
        IDXGISwapChain_Present(sc, 1, 0);
        Sleep(8);
    }
}

static void mark(const char *name) {
    char p[64];
    snprintf(p, sizeof p, "Z:\\tmp\\run\\%s", name);
    FILE *f = fopen(p, "w");
    if (f) fclose(f);
    printf("marker %s\n", name);
    frames(1500); // screenshot window
}

static void (*set_enabled)(int);


int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"host";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"host", L"Minecraft (test host)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 900, 600, NULL, NULL, wc.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC d = {0};
    d.BufferCount = 2;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = hwnd;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d, &sc, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("d3d11 failed %lx\n", hr); return 2; }
    ID3D11Texture2D *bb;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    ID3D11Texture2D_Release(bb);
    SetForegroundWindow(hwnd);
    frames(500);

    if (!fake_mc_setup()) { printf("layout\n"); return 2; }
    HMODULE w = LoadLibraryW(L"winwf_test.dll");
    if (!w) { printf("load failed\n"); return 2; }
    set_enabled = (void (*)(int))GetProcAddress(w, "winwf_set_enabled");
    frames(2500);

    mark("1_hidden");
    alt_g(); frames(700);
    mark("2_open_on");
    click(160, 162); frames(500);
    mark("3_open_off");
    alt_g(); frames(700);
    mark("4_closed");
    alt_g(); frames(700);
    mark("5_reopened_still_off");
    mark("done");
    return 0;
}
