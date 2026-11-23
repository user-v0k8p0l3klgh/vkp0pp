// Wine test host: pretends to be Minecraft Bedrock 1.16.100 closely enough for
// the original module to pass its version checks and hook four fake vtable
// slots, then checks that winwf's on/off gate routes those hooks correctly.
//
//   x86_64-w64-mingw32-gcc -O1 -o harness.exe tests/harness.c -lpsapi
//   cp target/x86_64-pc-windows-gnu/release/winwf.dll winwf_test.dll   (built with --features harness)
//   wine harness.exe

#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fakemc.h"

static int n_open, n_teardown, n_tick, n_input, n_nav;
static uint64_t last_input_e;

static uint64_t game_open(void *s) { (void)s; n_open++; return 0; }
static uint64_t game_teardown(void *s) { (void)s; n_teardown++; return 0; }
static uint64_t game_tick(void *s) { (void)s; n_tick++; return 0x1234; }
static uint64_t game_nav(void) { n_nav++; return 0; }
static uint64_t game_input(void *s, uint64_t a, uint64_t b, uint64_t c, uint64_t e) {
    (void)s; (void)a; (void)b; (void)c;
    n_input++;
    last_input_e = e;
    return 0x5A;
}

typedef uint64_t (*fn4)(void *, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*fn5)(void *, uint64_t, uint64_t, uint64_t, uint64_t);

static int failures;

#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); else { failures++; printf("  FAIL " __VA_ARGS__); } printf("\n"); } while (0)

static HMODULE find_module(const char *prefix) {
    HMODULE mods[512]; DWORD need;
    EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need);
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
        char name[MAX_PATH];
        if (GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, sizeof name) && !strncmp(name, prefix, strlen(prefix)))
            return mods[i];
    }
    return NULL;
}

static int in_module(uint64_t a, HMODULE m) {
    MODULEINFO mi;
    GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi);
    return a >= (uint64_t)mi.lpBaseOfDll && a < (uint64_t)mi.lpBaseOfDll + mi.SizeOfImage;
}

int main(void) {
    if (!fake_mc_setup()) { printf("layout: big array does not cover RVAs\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE w = LoadLibraryW(L"winwf_test.dll");
    if (!w) { printf("LoadLibrary failed %lu\n", GetLastError()); return 2; }
    int (*phase)(void) = (int (*)(void))GetProcAddress(w, "winwf_phase");
    void (*set_enabled)(int) = (void (*)(int))GetProcAddress(w, "winwf_set_enabled");
    volatile LONG *status = (volatile LONG *)GetProcAddress(w, "AutoPotionNavigationStatus");
    int ph = 0;
    for (int i = 0; i < 1000 && (ph = phase()) == 0; i++) Sleep(10);
    printf("phase=%d status=%ld\n", ph, *status);
    CHECK(ph == 1 && *status == 1, "module loaded, hooks verified and gated");
    if (ph != 1) return 1;

    HMODULE core = find_module("winwf-core-");
    CHECK(core != NULL, "original module loaded from temp file");
    if (!core) return 1;
    unsigned char *cb = (unsigned char *)core;
    volatile int64_t *tracked = (volatile int64_t *)(cb + 0x56270);
    volatile int32_t *tick_counter = (volatile int32_t *)(cb + 0x56064);

    CHECK(in_module(slot(SLOT_OPEN), w) && in_module(slot(SLOT_INPUT), w) && in_module(slot(SLOT_TICK), w), "open/input/tick slots point at winwf gates");
    CHECK(slot(SLOT_TEARDOWN) == (uint64_t)(cb + 0x2CC0), "teardown slot still the module's own detour");

    // the module verified the real prologues; now make the "game" functions callable
    put_jump(FN_OPEN, game_open); put_jump(FN_TEARDOWN, game_teardown); put_jump(FN_INPUT, game_input);
    put_jump(FN_TICK, game_tick); put_jump(FN_NAV, game_nav);

    static uint64_t obj[128];
    fn4 open = (fn4)slot(SLOT_OPEN), teardown = (fn4)slot(SLOT_TEARDOWN), tick = (fn4)slot(SLOT_TICK);
    fn5 input = (fn5)slot(SLOT_INPUT);

    printf("feature ON\n");
    open(obj, 0, 0, 0);
    CHECK(n_open == 1 && *tracked == (int64_t)obj, "open: game called, module tracks the screen");
    int32_t t0 = *tick_counter;
    uint64_t r = tick(obj, 0, 0, 0);
    CHECK(n_tick == 1 && *tick_counter == t0 + 1 && r == 0x1234, "tick: game called, module ran");
    input(obj, 1, 2, 3, 0xE5);
    CHECK(n_input == 1 && last_input_e == 0xE5, "input: game called with all 5 args");
    teardown(obj, 0, 0, 0);
    CHECK(n_teardown == 1 && *tracked == 0, "teardown: module state cleared");

    printf("feature OFF\n");
    set_enabled(0);
    open(obj, 0, 0, 0);
    CHECK(n_open == 2 && *tracked == 0, "open: game called, module bypassed");
    t0 = *tick_counter;
    r = tick(obj, 0, 0, 0);
    CHECK(n_tick == 2 && *tick_counter == t0 && r == 0x1234, "tick: game called, module bypassed, result kept");
    r = input(obj, 1, 2, 3, 0xE6);
    CHECK(n_input == 2 && last_input_e == 0xE6 && r == 0x5A, "input: straight to game, 5th arg and result kept");

    printf("feature ON again\n");
    set_enabled(1);
    t0 = *tick_counter;
    tick(obj, 0, 0, 0);
    CHECK(n_tick == 3 && *tick_counter == t0 + 1 && *tracked == (int64_t)obj, "tick: module active again and re-tracks the screen");
    teardown(obj, 0, 0, 0);
    CHECK(n_teardown == 2 && *tracked == 0, "teardown: cleared");

    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
