/* Scripted stand-in for xinput1_4.dll, used only by the integration harness.
 *
 * The harness imports XInputGetState from "xinput1_4.dll"; with this file in
 * its directory the import resolves here, which gives the test a controller
 * whose behaviour is known exactly:
 *
 *   - button A is tapped for FAKE_TAP_MS every FAKE_TAP_PERIOD_MS;
 *   - the right trigger is pulled for FAKE_TAP_MS every FAKE_PULL_PERIOD_MS;
 *   - the right stick X axis encodes the time of the read in milliseconds, so
 *     the harness can tell how old the analog value it received is.
 *
 * The taps are much shorter than a frame, so a game-style once-per-frame poll
 * misses most of them. */
#include <windows.h>
#include <xinput.h>

#define FAKE_TAP_MS 5
#define FAKE_TAP_PERIOD_MS 50
#define FAKE_PULL_PERIOD_MS 70

static LONGLONG g_start, g_freq;

static LONGLONG elapsed_us(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - g_start) * 1000000 / g_freq;
}

__declspec(dllexport) DWORD WINAPI XInputGetState(DWORD index, XINPUT_STATE *state) {
    LONGLONG us, ms;
    if (index != 0) return ERROR_DEVICE_NOT_CONNECTED;
    us = elapsed_us();
    ms = us / 1000;
    ZeroMemory(state, sizeof *state);
    state->dwPacketNumber = (DWORD)us;
    if (ms % FAKE_TAP_PERIOD_MS < FAKE_TAP_MS) state->Gamepad.wButtons |= XINPUT_GAMEPAD_A;
    if (ms % FAKE_PULL_PERIOD_MS < FAKE_TAP_MS) state->Gamepad.bRightTrigger = 255;
    state->Gamepad.sThumbRX = (SHORT)(ms % 30000);
    return ERROR_SUCCESS;
}

/* Number of A taps and trigger pulls that have fully completed so far. */
__declspec(dllexport) void WINAPI FakeXInputCompleted(DWORD *taps, DWORD *pulls, LONGLONG *now_ms) {
    const LONGLONG ms = elapsed_us() / 1000;
    *taps = (DWORD)((ms + FAKE_TAP_PERIOD_MS - FAKE_TAP_MS) / FAKE_TAP_PERIOD_MS);
    *pulls = (DWORD)((ms + FAKE_PULL_PERIOD_MS - FAKE_TAP_MS) / FAKE_PULL_PERIOD_MS);
    *now_ms = ms;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        g_freq = v.QuadPart;
        QueryPerformanceCounter(&v);
        g_start = v.QuadPart;
    }
    return TRUE;
}
