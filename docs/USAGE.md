# Usage

## Requirements

* Windows 10 (1803 or later) or Windows 11, x64.
* Minecraft Bedrock Edition **1.16.100.x**, UWP package `Microsoft.MinecraftUWP`, x64, legitimately installed.
* For gamepad features: an Xbox-compatible controller, or any controller Windows exposes through
  Windows.Gaming.Input / XInput.

On other Minecraft UWP builds the DLL loads only with `bll-launcher --force` and then measures without changing
anything. GDK builds (current Minecraft for Windows) are not supported at all.

## Building

The only dependency is a MinGW-w64 toolchain with the Windows SDK headers it ships; no third-party library is used.

**On Windows (MSYS2 UCRT64 shell)**

```
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bll_tests.exe
```

**On Linux (cross-compile)**

```
sudo apt-get install cmake ninja-build g++-mingw-w64-x86-64-posix mingw-w64-x86-64-dev
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win
```

Outputs: `bll.dll`, `bll-launcher.exe`, `bll-probe.exe`, `bll_tests.exe`, and `harness/` (test application).
The runtime is linked statically, so the DLL depends only on system DLLs.

The portable logic also builds and tests natively on Linux:
`cmake -S . -B build -G Ninja -DBLL_SANITIZE=ON && cmake --build build && ./build/bll_tests`.

MSVC is not a tested toolchain. The code avoids compiler extensions, but it has never been built with it.

## Installing and launching

1. Copy `bll.dll` and `bll-launcher.exe` into one folder of your choice (not inside the game's folder).
2. Run `bll-launcher.exe status` to see which Minecraft build is installed and whether it is the supported one.
3. Run `bll-launcher.exe`. It closes a running Minecraft, starts it with the DLL, and prints the first log lines.

What the launcher does, in order:

1. Grants *ALL APPLICATION PACKAGES* read/execute on `bll.dll` (added to the file's existing ACL). The game runs in
   an AppContainer and cannot load the file otherwise.
2. Registers itself as the package's debugger (`IPackageDebugSettings::EnableDebugging`), activates the app, and is
   then started by Windows with the game's suspended main thread. It queues `LoadLibraryW(bll.dll)` on that thread
   and resumes it, so the DLL is in place before the game creates its swap chain.
3. Removes the debugger registration again (`DisableDebugging`) whether or not the launch worked.

The injector refuses any process that is not the Minecraft UWP package, and resumes the game unmodified if anything
fails.

## Files

Inside the game's sandbox `%LOCALAPPDATA%` is redirected, so the files are at

```
%LOCALAPPDATA%\Packages\Microsoft.MinecraftUWP_8wekyb3d8bbwe\AC\BedrockLowLatency\
    bll.ini      configuration (created with defaults on first launch)
    bll.log      rewritten at every launch
    frames.csv   per-frame log, only with RecordFrameTimings=true
```

`%TEMP%\bll-inject.log` records what the injector did. No world, resource pack or game settings file is read or
written; `options.txt` is never touched.

## Configuration reference

Every key maps to implemented behaviour. `Mode` selects a preset; any key written explicitly overrides the preset.
Changes take effect at the next launch.

### [General]

| Key | Default | Effect |
|---|---|---|
| `Enabled` | `true` | `false`: the DLL loads and hooks nothing |
| `EnableLogging` | `true` | Write `bll.log` |
| `SafeMode` | `true` | If DXGI rejects a modified `Present`, the game's own arguments are used from then on |
| `AllowUnrecognizedBuild` | `false` | Enable behaviour-changing features on builds other than 1.16.100.x (untested, at your own risk) |
| `Mode` | `2` | Preset 0–5, see below |

### Presets

| Mode | LowLatencyQueue | WaitableSwapChain | AllowTearing | DisableVSync | AdaptiveFramePacing | BufferButtonTransitions | AdaptiveCpuGpuScheduling | GpuSync |
|---|---|---|---|---|---|---|---|---|
| 0 Vanilla | | | | | | | | |
| 1 V-Sync optimisation | | | ✓ | ✓ | | | | |
| 2 Low-latency queue | ✓ | ✓ | | | | | | |
| 3 Frame pacing | | | | | ✓ (`TargetFPS=-3` unless set) | | | |
| 4 Just-in-time | ✓ | ✓ | | | | | ✓ | ✓ |
| 5 Combined | ✓ | ✓ | ✓ | | ✓ | ✓ | | |

### [Rendering]

| Key | Effect | Conditions and limits |
|---|---|---|
| `LowLatencyQueue` | `IDXGIDevice1::SetMaximumFrameLatency(TargetQueueDepth)`; re-applied if the game changes it; restored when the swap chain goes away | Direct3D 11 only. Lower queue depth can reduce throughput when the game runs slower than the refresh rate |
| `TargetQueueDepth` | 1–16, default 1 | |
| `WaitableSwapChain` | Adds `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` at creation, sets `IDXGISwapChain2::SetMaximumFrameLatency`, and waits on the object after each `Present` | Flip-model swap chains only, and only when the DLL is loaded before the swap chain is created. If creation fails with the flag it is retried without it. Waits are bounded (≥ 50 ms) and bypassed after three time-outs |
| `AllowTearing` | Adds `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` at creation and `DXGI_PRESENT_ALLOW_TEARING` on presents whose sync interval is 0 | Only if `IDXGIFactory5::CheckFeatureSupport` reports tearing support and the swap chain is flip-model. Never combined with a non-zero sync interval |
| `DisableVSync` | Forces sync interval 0 | Tearing is visible without a variable-refresh display. Try the game's own `gfx_vsync` option first; the log shows whether the game honours it |
| `AdaptiveFramePacing` | Frame limiter at frame start | Needs `TargetFPS` |
| `TargetFPS` | `0` none, `N` cap at N, `-N` refresh rate minus N | The relative form waits until the refresh rate has been measured from DXGI frame statistics; nothing is assumed |
| `SpinMaxUs` | Upper bound of the busy-wait that ends each timed wait (default 1500, 0 = never spin) | The actual length follows the measured lateness of the system timer. With 0, pacing precision is that of the timer (typically 0.5–1 ms) |

### [Input]

| Key | Effect | Conditions and limits |
|---|---|---|
| `OptimizeGamepad` | Install the gamepad hooks (needed for all input statistics and for the latch) | The Windows.Gaming.Input hook is installed once a gamepad is connected |
| `BufferButtonTransitions` | Sampler thread plus transition latch: a tap that starts and ends between two game polls is reported as pressed for one poll and released on the next | Cannot recover a tap shorter than the sampler period or one the controller itself never reported. A recovered tap's *release* is seen one poll later than it happened; normal presses are not delayed. Pending transitions are dropped after 250 ms without a game poll |
| `SamplerHz` | 125–2000, default 1000 | Sampling faster than the controller reports gains nothing |
| `TriggerPressThreshold` / `TriggerReleaseThreshold` | Hysteresis used to treat a trigger pull as a tap (0.5 / 0.3) | Only decides which pulls are *replayed*; trigger values otherwise pass through unchanged |

### [Experimental]

| Key | Effect | Conditions and limits |
|---|---|---|
| `AdaptiveCpuGpuScheduling` | Just-in-time frame start for refresh-locked presentation | Needs a measured refresh period and v-sync or a waitable swap chain. Accepts roughly one repeated refresh per thousand frames; becomes inert when frames keep missing |
| `GpuSync` | Do not start a frame while the previous one is still on the GPU (D3D11 event query) | Assumes `Present` is called on the thread that owns the immediate context. Wait bounded to 30 ms |
| `JitMarginUs` | Minimum safety margin of the just-in-time scheduler (default 2000) | The learned margin is added on top |

### [Diagnostics]

| Key | Effect |
|---|---|
| `RecordFrameTimings` | Write `frames.csv` (one row per presented frame) |
| `RecordInputTiming` | Include input-age statistics in the summary |
| `SummaryIntervalSeconds` | Period of the summary line (default 10) |

## Reading the log

The start of `bll.log` answers the questions that could not be answered without your installation:

```
process package: Microsoft.MinecraftUWP_1.16.10004.0_x64__8wekyb3d8bbwe -> target (...), game version 1.16.100.4
relevant modules loaded at initialisation: d3d11.dll dxgi.dll ...
CreateSwapChainForCoreWindow: 1920x1080 format=.. buffers=.. swap_effect=.. flags=0x..     <- how the game creates its swap chain
graphics backend: Direct3D 11, feature level 0x....                                        <- the renderer really in use
IDXGIDevice1::SetMaximumFrameLatency(1) -> 0x00000000 (was 3)
waitable swap chain: SetMaximumFrameLatency(1) -> 0x00000000, waitable object acquired
Present: game asked (sync=1 flags=0x0), issued (sync=1 flags=0x0) -> 0x00000000             <- what the game passes to Present
Windows.Gaming.Input: IGamepad::GetCurrentReading hook installed
```

Every `SummaryIntervalSeconds` a line like this follows:

```
summary: frames=600 fps=60.0 frame_ms p50=16.67 p95=16.9 p99=17.4 sd=0.2 low1%=57.5 low0.1%=55.0
 | input_age_ms p50=5.1 p95=6.0 p99=7.2 polls/frame=1.00
 | display_ms p50=12.3 p95=13.0 input->display p50=17.4
 | present_block_ms p50=0.1 gate_ms=9.8 gpu_pending=0% missed=0 limit=display-refresh
 | d3d11=1 waitable=1 tearing=0 device_latency=1 modify_ok=1
 | pad: wgi_hooked=1 wgi_polls=6000 xinput_slots=0 xinput_polls=0 latch=0 taps_preserved=0 ...
```

| Field | Meaning |
|---|---|
| `input_age_ms` | Time from the game's most recent gamepad poll to the `Present` call of the frame |
| `polls/frame` | How often the game polls the pad. ≈ 1 means once per frame; a value well below 1 means polling is tied to something slower than rendering (for example the 20 Hz tick) |
| `display_ms` | `Present` call to the vblank that showed the frame, from DXGI frame statistics; `-1` if the system does not provide them |
| `input->display` | Sum of the two medians: the software part of the latency chain. It excludes the controller, USB/Bluetooth, scan-out and the panel |
| `gate_ms` | Mean wait inserted before a frame starts |
| `limit` | What bounded the frame rate: `cpu`, `gpu`, `present`, `display-refresh`, `frame-cap` or `mixed` |
| `wgi_polls` / `xinput_polls` | Which controller API the game really uses |
| `taps_preserved` | Transitions the latch had to deliver one poll late to keep a tap from being lost |

**Verifying activation:** the log contains `initialisation complete`, a `governing swap chain` line, and summary lines
with non-zero `frames`. With a pad connected and in use, one of `wgi_polls` / `xinput_polls` grows. If both stay at 0
the game uses a controller API the DLL does not cover; rendering features still work, input statistics and the latch
do not.

## Using a gamepad

Nothing has to be configured: connect the controller before or after starting the game. The controller's mapping,
dead zones, sensitivity, vibration and the game's own controller settings are untouched. With the latch off (the
default) the game receives exactly what Windows reports. With the latch on, only buttons and triggers during a
replayed tap differ from the raw reading.

## Disabling and rollback

| Goal | How |
|---|---|
| Play without it | Start Minecraft normally (Start menu). The DLL is only loaded when the launcher starts the game |
| Keep the launcher, change nothing in game | `Mode=0`, or `Enabled=false` |
| Turn one feature off | Set its key to `false` |
| Reset the configuration | Delete `bll.ini`; defaults are written at the next launch |
| Remove everything | Delete the folder with `bll.dll` and `bll-launcher.exe` and the `BedrockLowLatency` data folder |
| A launch was interrupted and Minecraft will not start normally | `bll-launcher.exe disable` clears the debugger registration |

Nothing is installed system-wide: no driver, no service, no registry change, no change to the game package.

## Game settings worth testing (not changed by this project)

`options.txt` (`...\LocalState\games\com.mojang\minecraftpe\options.txt`) is reported by community sources to contain
`gfx_vsync`; whether 1.16.100 reads it, and whether a frame-rate key exists in this version, could not be confirmed.
Change it with the game closed, one key at a time, and check the `Present: game asked (sync=...)` log line to see
whether it had any effect. Driver-level features (NVIDIA Low Latency Mode, AMD Anti-Lag) are documented by their
vendors for Direct3D 11; whether they engage for this UWP title has to be measured, not assumed.
