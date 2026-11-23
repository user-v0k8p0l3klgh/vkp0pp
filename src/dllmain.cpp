// DLL entry and initialisation.
//
// The launcher loads this DLL into the suspended game process with a user APC
// on the main thread, so DllMain runs before the game's entry point. DllMain
// itself only schedules work: real initialisation needs the loader lock to be
// free. It is triggered from two places and runs exactly once:
//
//   - a second APC queued to the same thread, which the system delivers right
//     after the first one returns - still before the game's entry point, so
//     the hooks are in place before the game can create its swap chain;
//   - a worker thread, which covers every other way the DLL may be loaded
//     (late injection, the test harness) where no APC will be delivered.
#include <windows.h>

#include <atomic>
#include <fstream>
#include <system_error>

#include "core/log.h"
#include "platform/package.h"
#include "runtime.h"

namespace bll {

namespace {

std::atomic<Runtime*> g_runtime{nullptr};
std::atomic<int> g_init_state{0}; // 0 not started, 1 running, 2 finished
std::atomic<bool> g_apc_ran{false};
HANDLE g_init_done = nullptr;

std::string status_line() { return graphics_status() + " | " + gamepad_status(); }

void log_environment() {
    static const wchar_t* const modules[] = {L"d3d11.dll",     L"d3d12.dll",      L"dxgi.dll",
                                             L"xinput1_4.dll", L"xinput1_3.dll",  L"xinput9_1_0.dll",
                                             L"xinputuap.dll", L"windows.gaming.input.dll"};
    std::string loaded;
    for (const wchar_t* m : modules)
        if (GetModuleHandleW(m)) loaded += narrow(m) + " ";
    BLL_INFO("relevant modules loaded at initialisation: %s", loaded.empty() ? "(none yet)" : loaded.c_str());
}

void do_initialize(const char* path_taken) {
    const std::filesystem::path dir = data_directory();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    bool found = false;
    const std::string text = read_text_file(dir / L"bll.ini", &found);
    std::vector<std::string> warnings;
    const Config file_cfg = parse_config(text, &warnings);
    if (!found) {
        std::ofstream out(dir / L"bll.ini", std::ios::binary);
        out << kDefaultConfigText;
    }

    if (file_cfg.logging) log_open(dir / L"bll.log");
    BLL_INFO("BedrockLowLatency 0.1.0 initialising (%s), data directory %s", path_taken, narrow(dir.wstring()).c_str());
    if (!found) BLL_INFO("no bll.ini found; defaults written and used");
    for (const std::string& w : warnings) BLL_WARN("bll.ini %s", w.c_str());
    BLL_INFO("configured: %s", describe(file_cfg).c_str());

    if (!file_cfg.enabled) {
        BLL_INFO("Enabled=false: nothing is hooked");
        return;
    }

    const std::string full_name = current_package_full_name();
    PackageIdentity id;
    if (!full_name.empty()) parse_package_full_name(full_name, id);
    const BuildClass build = classify_build(id);
    BLL_INFO("process package: %s -> %s%s%s", full_name.empty() ? "(none)" : full_name.c_str(), to_string(build),
             id.name.empty() ? "" : ", game version ", id.name.empty() ? "" : game_version(id.version).c_str());

    const bool active = build == BuildClass::Target || file_cfg.allow_unrecognized_build;
    Config effective = file_cfg;
    if (!active) {
        // Unknown build: observe and report, change nothing.
        apply_mode(effective, 0);
        BLL_WARN("this is not the supported build; running in measurement-only mode. "
                 "Set AllowUnrecognizedBuild=true to override at your own risk");
    } else if (build != BuildClass::Target) {
        BLL_WARN("AllowUnrecognizedBuild=true: features enabled on an untested build");
    }
    BLL_INFO("effective: %s", describe(effective).c_str());
    log_environment();

    auto* rt = new Runtime(file_cfg, effective, build, active);
    MetricsSink::Options options;
    if (effective.record_frame_timings) options.csv_path = dir / L"frames.csv";
    options.summary_interval_s = effective.summary_interval_s;
    options.input_stats = effective.record_input_timing;
    rt->metrics.set_extra_provider(&status_line);
    rt->metrics.start(options);
    g_runtime.store(rt, std::memory_order_release);

    install_dxgi_hooks();
    install_gamepad_hooks();
    BLL_INFO("initialisation complete");
}

void initialize(const char* path_taken) {
    int expected = 0;
    if (!g_init_state.compare_exchange_strong(expected, 1)) {
        // Someone else is initialising: wait so that the caller does not run
        // ahead of the hooks. Bounded, so a failure can never hang the game.
        if (g_init_done) WaitForSingleObject(g_init_done, 10000);
        return;
    }
    try {
        do_initialize(path_taken);
    } catch (...) {
        BLL_ERROR("initialisation failed with an exception; the game continues unmodified");
    }
    g_init_state.store(2);
    if (g_init_done) SetEvent(g_init_done);
}

void CALLBACK init_apc(ULONG_PTR) {
    g_apc_ran.store(true);
    initialize("startup APC");
}

DWORD WINAPI init_thread(LPVOID) {
    initialize("worker thread");
    if (g_apc_ran.load()) return 0;
    // Loaded into an already running process: the swap chain may exist already.
    Sleep(2000);
    Runtime* rt = runtime();
    if (rt && !swap_chain_seen()) attach_to_existing_swap_chain();
    return 0;
}

} // namespace

Runtime* runtime() { return g_runtime.load(std::memory_order_acquire); }

} // namespace bll

// Blocks until initialisation has finished. Returns 1 if the hooks are active.
extern "C" __declspec(dllexport) int bll_wait_ready(unsigned timeout_ms) {
    if (bll::g_init_done) WaitForSingleObject(bll::g_init_done, timeout_ms);
    return bll::runtime() != nullptr ? 1 : 0;
}

// Synchronous hook of an already existing swap chain (test harness).
extern "C" __declspec(dllexport) void bll_attach_existing(void) {
    if (bll::runtime()) bll::attach_to_existing_swap_chain();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        bll::g_init_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        QueueUserAPC(&bll::init_apc, GetCurrentThread(), 0);
        if (const HANDLE thread = CreateThread(nullptr, 0, &bll::init_thread, nullptr, 0, nullptr)) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // Explicit unload: put every redirected pointer back. Threads are not
        // joined here (loader lock); the supported way to disable is to start
        // the game without the launcher.
        bll::remove_gamepad_hooks();
        bll::remove_dxgi_hooks();
    }
    return TRUE;
}
