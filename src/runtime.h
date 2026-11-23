// Process-wide state of the DLL, created once by the initialisation path and
// read by the hooks.
#pragma once
#include <string>

#include "core/config.h"
#include "core/version.h"
#include "metrics/metrics.h"
#include "timing/frame_governor.h"

namespace bll {

struct Runtime {
    Runtime(const Config& file_config, const Config& effective_config, BuildClass build_class, bool active_)
        : file(file_config),
          cfg(effective_config),
          build(build_class),
          active(active_),
          governor(effective_config, system_wait_backend()) {}

    const Config file; // as written by the user
    const Config cfg;  // what is actually applied (mode 0 when not active)
    const BuildClass build;
    // False on unrecognised builds: hooks only observe, nothing is changed.
    const bool active;
    FrameGovernor governor;
    MetricsSink metrics;
};

// Null until initialisation has finished; hooks pass through while it is.
Runtime* runtime();

// Graphics subsystem.
bool install_dxgi_hooks();
void remove_dxgi_hooks();
bool swap_chain_seen();
// Hooks Present through a throw-away swap chain. Used when the game's own
// swap chain was created before the DLL was loaded.
void attach_to_existing_swap_chain();
std::string graphics_status();

// Input subsystem.
void install_gamepad_hooks();
void remove_gamepad_hooks();
std::string gamepad_status();

} // namespace bll
