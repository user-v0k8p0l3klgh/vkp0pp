# BedrockLowLatency – technical report

Conventions used throughout: **[S]** confirmed by reading source code in this audit, **[D]** stated by official
documentation, **[C]** claim by a project author or community source, **[H]** hypothesis, **[M]** measured here,
**[Sim]** output of the virtual-time model in `tests/sim.h`. Nothing in this report was measured on Minecraft itself.

---

## SECTION 1 — Executive technical summary

The time between a controller input and the frame that shows it is dominated, on a PC that renders faster than the
display refreshes, by frames waiting in the presentation queue and by *where the game loop blocks*. Three levers
address that and all three can be pulled from outside the game through documented APIs:

1. **Presentation queue depth.** A Direct3D 11 device queues up to three frames by default **[D]**. Reducing it with
   `IDXGIDevice1::SetMaximumFrameLatency(1)` is a one-call change on the existing device.
2. **Where the wait happens.** With a blocking `Present`, the game samples input, renders, and then waits; the wait
   ages a finished frame. A waitable swap chain moves the wait in front of the next frame, so the input is sampled
   after it. Microsoft documents this pattern for UWP games **[D]**. Because the game loop is
   `poll → simulate → render → Present`, a hook that waits *after* `Present` returns is waiting *before* the next
   poll – no knowledge of game internals is required. The frame limiter and the just-in-time delay use the same
   position, which is the main design difference to limiters that wait before `Present` (Glide **[S]**).
3. **Input sampling.** A polled gamepad API exposes state, not events. Taps shorter than the poll interval vanish.
   A sampler thread plus a transition latch at the API boundary preserves them without delaying normal presses and
   without ever buffering stick values.

What exists: a DLL implementing all of this with per-feature switches, a launcher, measurement tooling, 66 unit
tests, a latency model and an integration suite that loads the DLL into a real Direct3D 11 process.

What does not exist: any measurement on the game. The expected effect is a hypothesis backed by API documentation
and a model (Section 10); the procedure to test it is in [TESTING.md](TESTING.md).

What could not be done from the API boundary: moving the game's own input poll or camera update inside the frame,
or changing camera smoothing. Those need knowledge of the executable that was not available (Section 11).

---

## SECTION 2 — Target game architecture

| Aspect | What is known | Basis |
|---|---|---|
| Package | `Microsoft.MinecraftUWP_8wekyb3d8bbwe`, x64, UWP (AppContainer) | **[S]** Stonecutter targets this family for UWP builds |
| Version | 1.16.100 released November 2020; Windows package version reported as `1.16.10004.0` | **[D]** release notes for the date, **[C]** for the package number |
| Renderer | Stable 1.16.100 predates RenderDragon on Windows (stable from 1.16.200); betas of the cycle toggled it | **[D]** 1.16.200 notes; shipping without it in 1.16.100 is the accepted reading, not a quoted statement |
| Graphics API | Direct3D 11 **[H]**. No public primary source identifying the pre-RenderDragon Windows backend was found | Logged at run time: `graphics backend: ...` |
| Swap chain | `CreateSwapChainForCoreWindow` **[H]** – a CoreWindow UWP app has no other option except composition swap chains; flip model is mandatory for UWP **[D]** | Logged at run time with the full description |
| How it presents | Unknown: sync interval, flags, whether `gfx_vsync` is honoured | Logged: `Present: game asked (sync=…, flags=…)` |
| Controller API | Unknown: Windows.Gaming.Input or XInput | Both are hooked; the log counts calls to each |
| Poll timing | Unknown: per frame or per 20 Hz tick | Logged: `polls/frame`, `input_age_ms` |
| Threading | Unknown | The only assumption made is that `Present` runs on the thread owning the immediate context (needed for `GpuSync` only) |

The unknowns are deliberate: no copy of the game was available and none was downloaded, so nothing about the
executable is asserted. `bll-probe` reports the same facts statically for an executable the user can read
(sideloaded installs; Store-installed packages are encrypted at rest).

**Consequences for the design.** Every decision that depends on an unknown is made at run time and has a fall-back:
flags are only added to flip-model swap chains and creation is retried without them; Direct3D 11 features switch off
on a Direct3D 12 device; the relative frame cap and the just-in-time scheduler stay inactive until the refresh period
has been measured; both controller APIs are hooked and each is inert if unused.

Version handling: the DLL reads its own package identity. Game version 1.16.100.x on x64 enables the configured
features; any other Minecraft UWP build, foreign package or unpackaged process gets measurement-only mode unless
`AllowUnrecognizedBuild=true`. The launcher additionally refuses non-target builds without `--force`.

---

## SECTION 3 — GitHub source analysis

Commit hashes are the heads cloned on 2026-11-23. Rows marked ◆ were read in full by the author of this report;
the others were read by a delegated source audit and are summarised from its findings.

### Bedrock projects

| Project (commit) | Licence | What the source does | Portable to 1.16.100 UWP? |
|---|---|---|---|
| ◆ [Aetopia/Stonecutter](https://github.com/Aetopia/Stonecutter) (`094e948a31`, archived) | MIT | `Program.c`: `IPackageDebugSettings::EnableDebugging` with itself as debugger, `ActivateApplication`; as debugger it sets an ACL for *ALL APPLICATION PACKAGES* on the DLL, queues `LoadLibraryW` as an APC on the suspended main thread and resumes it. `Library.c`: MinHook on `RegisterClassExW` as an early trampoline, then on `IDXGIFactory2::CreateSwapChainForCoreWindow`; overwrites `Flags` with `ALLOW_TEARING`; hooks `Present` (adds `DXGI_PRESENT_ALLOW_TEARING` when interval is 0) and `ResizeBuffers` (forces the tearing flag); optional refusal of non-D3D11 devices; cursor recentering. No version check, no waitable swap chain, no frame-latency call | Launch mechanism and hook points: yes, reused as a *design* (independently implemented, with version checks, flag validation and fall-backs added). Its "input delay" fix is the tearing flag; whether 1.16.100 presents with interval 0 at all is unknown |
| [Aetopia/Bedrock.Fixes](https://github.com/Aetopia/Bedrock.Fixes) | – | **Not inspected.** The repository is not publicly readable (clone requires authentication); no fork or archive of the source was found | – |
| ◆ [Aetopia/Igneous](https://github.com/Aetopia/Igneous) (`f6d3c549f2`) | MIT | Proxy `vcruntime140_1.dll` for GDK builds; hooks `CreateSwapChainForHwnd`, `Present`, `ResizeBuffers`, `ResizeBuffers1` to add tearing flags; cursor clipping fix; `DwmEnableMMCSS` | No: HWND/GDK. Confirms the same DXGI pattern on newer builds |
| ◆ [VuloB1/glide](https://github.com/VuloB1/glide) (`9a72ec77f2`) | MIT | Vtable patch of `Present` (slot 8) and `Present1` (slot 22) taken from a dummy swap chain; limiter waits *before* `Present` on a high-resolution timer, then spins; spin margin = 99th percentile of the last 256 timer overshoots + 10 %, clamped to 0.5–2.5 ms; re-anchors instead of bursting when late; `IDXGIDevice1::SetMaximumFrameLatency(1)` re-applied every second, skipped when a frame-generation DLL is loaded | Algorithms: yes (re-implemented). Changed here: the wait is placed after `Present`, the margin window is shorter, spinning can be disabled, and the CPU cost is reported |
| ◆ [flarialmc/bootstrapper](https://github.com/flarialmc/bootstrapper) (`331ea91add`) | MIT and GPL-3.0 files, applicability not stated | Same hooks as Igneous for HWND swap chains; optional `D3D12CreateDevice` hook returning `DXGI_ERROR_UNSUPPORTED` to force Direct3D 11 | No (GDK). Forcing D3D11 is irrelevant if 1.16.100 is D3D11 already |
| [Furzide/MCBE-Tweaks](https://github.com/Furzide/MCBE-Tweaks) (`ab9861f9d4`) | none | One README; documents `gfx_vsync=0`, no version scope, no `gfx_max_framerate` | As a setting to *test*, not as evidence for 1.16.100 |
| [Aetopia/MCBE-Tweaks](https://github.com/Aetopia/MCBE-Tweaks) | – | **Not inspected** (not publicly readable) | – |
| [BedrockLauncher](https://github.com/BedrockLauncher/BedrockLauncher) (`09282ecb8f`) | GPL-3.0 | Downloads UWP packages through the Windows Update client protocol, extracts them and registers them in development mode (`RegisterPackageAsync`, fall-back `Add-AppxPackage -Register`) | Background on how old UWP versions get installed. Not used; whether its catalogue contains 1.16.100 was not checked |

### Java Edition latency mods (algorithms only – none of this code can run in Bedrock)

| Project (commit) | Licence | Core idea in the source | What transfers |
|---|---|---|---|
| [WinExp/Minecraft-Low-Latency](https://github.com/WinExp/Minecraft-Low-Latency) (`440a0aae3a`) | LGPL-3.0 | GPU time from timestamp queries and fences, median of six frames; waits before the next event poll for `max(0, G·(N+o) − C)·(1+o)`; parks until 0.5 ms before the deadline, then spins. No cap on the wait and no fall-back when the prediction is wrong; the author warns it can reduce frame rate or increase latency | The placement (wait before the input poll) and the idea of not running ahead of the GPU. Implemented differently here: an event query answers "is the previous frame still on the GPU", the wait is capped at 30 ms, and the refresh-locked case uses a learned margin with back-off |
| [incend1um3/No-Input-Lag-Tick-Rate](https://github.com/incend1um3/No-Input-Lag-Tick-Rate) (`9ac37b2f69`) | MIT | Runs key handling on a separate 20 Hz clock inside the render loop; rescales block-breaking | Nothing implementable: it needs to call engine key handling. It also changes gameplay timing, which this project does not do |
| [Kemmlow/inputoptimizer](https://github.com/Kemmlow/inputoptimizer) (`077a42894f`) | LGPL-3.0 | Raw Input on a dedicated thread; events queued and drained at tick start; mouse movement accumulated and applied per frame | Separate acquisition, and "newest analog value per frame, never a replay of old ones". Its queues are unbounded; the latch here is bounded by construction |
| [decce6/Ixeris](https://github.com/decce6/Ixeris) (`24b173cace`) | LGPL-3.0 | Moves GLFW event polling to its own thread; main-thread dispatcher; limiter sleeps to 1.1 ms before target and spins, re-syncs when more than a frame late | Limiter shape. Its cross-thread synchronous calls carry a deadlock risk the design here avoids by having no cross-thread calls at all |
| [Walksy/RawInputBuffer](https://github.com/Walksy/RawInputBuffer) (`c02d9182b8`) | MIT | Mouse-only raw input handling | Nothing for gamepads |
| [Shikaru0/sdl-input](https://github.com/Shikaru0/sdl-input) (`fe3a6f780d`) | MIT | SDL as alternative input back-end, snapshots per tick | Illustrates the limitation the latch addresses: a snapshot per tick cannot see shorter presses |
| [TensorF1eld/Input-optimizer-](https://github.com/TensorF1eld/Input-optimizer-) (`f316db6e71`) | no licence file (metadata says LGPL-3.0-only) | Ordered, timestamped event queue; bounded latency samples with P50/P95/P99; ordering and duplicate tests | The reporting shape (percentiles of input age) and the test ideas |
| [sidit77/perfect_presentation](https://github.com/sidit77/perfect_presentation) (`df992bc55d`) | MIT | Replaces the OpenGL swap with a DXGI flip-discard swap chain created with `ALLOW_TEARING` and `FRAME_LATENCY_WAITABLE_OBJECT`, latency 1; waits on the object at the start of the next frame; tearing flag only with interval 0 | The closest relative of Mode 2, and independent confirmation of the pattern. It does not check tearing support; this project does |
| [fireboy637/better-vsync-option](https://github.com/fireboy637/better-vsync-option) (`f2aed3409b`) | MIT | Present-mode selection UI | Nothing |
| [GermanBucket/faster-block-placement](https://github.com/GermanBucket/faster-block-placement) (`2d0d68f5fa`) | MIT | Changes the placement repeat rate | Deliberately not ported: it is a gameplay-rate change, not latency |

### Controller projects and tools

| Project (commit) | Licence | Finding | Use here |
|---|---|---|---|
| [Nekuzaky/better-controller](https://github.com/Nekuzaky/better-controller) (`8eef9709e0`) | MIT | Polls on the 20 Hz client tick and re-applies that sample every frame | A caution: per-tick sampling makes stick input up to 50 ms stale. Whether Bedrock does the same is what `polls/frame` reveals |
| [isXander/Controlify](https://github.com/isXander/Controlify) (`ec4f067db8`) | LGPL-3.0 | Polls per tick, applies look per frame; response curves and dead zones | Same caution. Its curve handling is not relevant: this project never alters stick values |
| [WyvernIXTL/gamepadla-plus](https://github.com/WyvernIXTL/gamepadla-plus) (`1b4e254d55`) | MIT | Measures the report rate and jitter visible to the OS | Diagnostic step in TESTING.md. Not a latency measurement |
| [LordOfMice/hidusbf](https://github.com/LordOfMice/hidusbf) (`8bfc7608d9`) | none, binaries only | Filter driver overriding the USB polling interval | Not recommended: kernel driver, conflicts with Memory Integrity, device-specific |
| [OpenStickCommunity/GP2040-CE](https://github.com/OpenStickCommunity/GP2040-CE) (`0ae800d627`) | MIT | Firmware: 1 ms HID interval, tight main loop, configurable debounce | Background on what a controller can deliver |
| [GameTechDev/PresentMon](https://github.com/GameTechDev/PresentMon) (`e580ac87f2`, 2.6.1) | MIT | Presentation telemetry from ETW; input tracking is mouse/keyboard oriented | Cross-check of the DLL's own numbers; `tools/analyze/analyze.py presentmon` reads its CSV |
| [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook) | BSD-2-Clause | Inline hooking library used by Stonecutter | Evaluated and not used: every hook needed is a table entry (vtable or import table), which avoids rewriting code and a dependency |

No code from any of these projects is included in this repository.

---

## SECTION 4 — Windows and DirectX API analysis

| API | What it does | Prerequisites and limits | Use in this project |
|---|---|---|---|
| `IDXGIDevice1::SetMaximumFrameLatency(n)` | Limits how many frames the device may queue; default 3, range 1–16 **[D]** | Per device, any D3D11 swap chain. With a blocking `Present` the wait still happens after rendering | `LowLatencyQueue`. Original value saved and restored; re-applied if changed |
| `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` | Makes the swap chain expose a waitable object signalled when a frame may be started | Flip model only; must be set at creation and cannot be added with `ResizeBuffers`; must be repeated on every `ResizeBuffers` **[D]** | Added in the creation hook when the DLL is loaded early; creation retried without it on failure; repeated in the `ResizeBuffers` hook |
| `IDXGISwapChain2::SetMaximumFrameLatency` / `GetFrameLatencyWaitableObject` | Per-swap-chain latency and the handle to wait on; default latency 1 **[D]** | Valid only on swap chains created with the flag above – distinct from the device-level call. The handle belongs to the caller | Called only when the flag is present; handle closed when the swap chain is destroyed |
| `WaitForSingleObjectEx` on that handle | Blocks until the queue has room; documented to be done before rendering each frame **[D]** | Can stop signalling when the window is occluded or suspended | Called after `Present`; time-out ≥ 50 ms; bypassed for 240 frames after three time-outs |
| `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` + `DXGI_PRESENT_ALLOW_TEARING` | Unsynchronised presentation in windowed flip model | `IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)`; flip model; present flag only with sync interval 0; flag repeated on `ResizeBuffers` **[D]** | `AllowTearing`: all four conditions are checked; never sent with a non-zero interval |
| `Present` / `Present1` sync interval | 0 = no wait, n = wait n vblanks | `DXGI_PRESENT_TEST` presents nothing | `DisableVSync` forces 0. Test presents and failed/occluded presents are never paced or counted |
| `IDXGISwapChain::GetFrameStatistics` | Vblank counter, time of the last vblank, the vblank at which the last frame was shown | May fail or be disjoint; semantics differ between presentation modes | Refresh period measurement (median of period samples) and the `display_ms` metric. Treated as unavailable whenever it fails |
| `ID3D11Query` (`D3D11_QUERY_EVENT`) | Signals when the GPU reached a point in the command stream | Immediate context, same thread as rendering | `GpuSync`: one query per frame, polled without flushing, wait capped at 30 ms |
| `Windows.Gaming.Input.Gamepad.GetCurrentReading` | Snapshot of the gamepad state with a timestamp **[D]** | A snapshot: no event history. Readings are neutral while the app is not focused | Hooked through the `IGamepad` vtable once a gamepad exists; timestamps and values pass through untouched |
| `XInputGetState` | Snapshot plus a packet number that changes when the state changes | Same limitation; four user indices | Hooked through the import table (normal and delay-load) of the game's own modules |
| `CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` | Timer that does not depend on the global timer resolution (Windows 10 1803+) | Still late by a variable amount | Coarse part of every timed wait; the busy-wait tail is sized from the measured lateness. Falls back to an ordinary timer |
| `IPackageDebugSettings::EnableDebugging` | Starts a packaged app suspended under a debugger command line | Must be undone with `DisableDebugging` | Launcher; always undone |

Not used on purpose: `timeBeginPeriod` (global side effect), thread priority or MMCSS changes on the game's threads,
registry or driver settings, anything from the RenderDragon/GDK era that assumes an HWND swap chain.

---

## SECTION 5 — Root-cause analysis

Where controller-to-photon time can go, and what this project can see or change:

| Stage | Mechanism | Status | Visible in | Addressed by |
|---|---|---|---|---|
| Controller | Report interval (USB bInterval, Bluetooth, wireless), firmware debounce | **[D]/[C]** hardware dependent | gamepadla-plus | Nothing in software. Wired or the Xbox adapter is the lever |
| Acquisition | Game polls a snapshot once per frame or per tick; taps between polls are lost; a per-tick poll makes stick data up to 50 ms old | **[H]** for this game; **[S]** that Java controller mods do exactly this | `polls/frame`, `input_age_ms` | Taps: the latch. Poll *position* cannot be moved from outside; its age shrinks when the frame start is delayed |
| Simulation | Camera applied per frame or per tick, smoothing, interpolation | **[H]**, unknown | Camera test only | Not addressed (Section 11) |
| Render-ahead | CPU prepares frames faster than they are shown; up to three wait in the queue | **[D]** default of 3; **[H]** that the game leaves it there | `present_block_ms`, `display_ms`, `limit=` | `LowLatencyQueue`, `WaitableSwapChain` |
| Wait position | Blocking in `Present` ages a finished frame by up to a refresh period | **[D]** (the rationale for waitable swap chains) | `present_block_ms` vs `gate_ms` | `WaitableSwapChain` |
| Refresh-locked slack | Frame started at the vblank is finished long before the next one | arithmetic | `input_age_ms` vs frame time | `AdaptiveCpuGpuScheduling` |
| GPU queue | CPU submits while the GPU is still on the previous frame | **[H]** for a 2020 block game on a modern GPU it is rarely the limit | `gpu_pending` | `GpuSync` |
| V-sync | Synchronised presentation adds up to one refresh; composed flip adds the compositor | **[D]** | PresentMon `PresentMode` | `AllowTearing`/`DisableVSync`, or VRR with a cap below refresh |
| Pacing | Uneven frame starts make latency vary frame to frame | **[H]** | frame-time percentiles | `AdaptiveFramePacing` |
| Display | Scan-out position, panel processing, response time | hardware | Camera test | Game mode on the display |

Observed facts about the game itself: none. The first run of Mode 0 turns the **[H]** rows above into observations.

---

## SECTION 6 — Optimisation design

### Data flow per frame

```
game:  poll gamepad ──► simulate ──► render ──► Present ───────────────────────────────► poll gamepad ...
hooks:   │ timestamp                               │ args adjusted, timed                  ▲
         │ (latch)                                 ▼                                       │
         └──────────────► FrameGovernor.on_present ── gate ────────────────────────────────┘
                                                      1 waitable object   (queue slot free)
                                                      2 GPU sync          (previous frame done)
                                                      3 frame pacer       (cap deadline)
                                                      4 just-in-time      (as late as the refresh allows)
```

Every wait sits in the gate, i.e. between the end of one frame and the input poll of the next. Nothing waits between
rendering and `Present`.

### Components

| Component | File | Responsibility |
|---|---|---|
| `FrameGovernor` | `src/timing/frame_governor.*` | The gate above; per-frame record; bounded waits and bypasses. No Windows dependency, driven by a virtual clock in tests |
| `FramePacer` | `src/timing/frame_pacer.*` | Fixed grid of frame *starts*; a slightly late frame keeps the grid, a stall re-anchors it |
| `PreciseSleeper` | `src/timing/precise_sleeper.*` | Timer wait, then busy-wait sized from the timer's measured lateness (p99 + 10 % + 50 µs, clamped to `SpinMaxUs`); accumulates spin time for reporting |
| `AdaptiveScheduler` | `src/timing/adaptive_scheduler.*` | Delay = period − (mean + 4·deviation of CPU work) − margin. Margin grows by period/16 per missed refresh and shrinks by a thousandth of that per clean frame, so the steady state is about one miss per thousand frames; delay ramps up from zero at 1 % per frame and halves on a miss |
| `BottleneckDetector` | `src/timing/bottleneck.*` | Per-frame class from where the time went, majority over 120 frames |
| `RefreshEstimator` | `src/timing/refresh_estimator.*` | Refresh period from frame-statistics pairs; glitch rejection |
| `TransitionLatch` / `LatchedPad` | `src/input/*` | Per-button pending-transition counters (bounded at 3), trigger hysteresis with peak hold, stale reset; device reads serialised with the latch under one short lock |
| DXGI hooks | `src/graphics/dxgi_hooks.cpp` | Creation (CoreWindow, HWND, composition), `Present`, `Present1`, `ResizeBuffers`, `Release`; flag validation, retry without flags, state per swap chain, device-latency save/restore, late attach |
| `GpuSync` | `src/graphics/gpu_sync.*` | Event query per frame |
| Gamepad hooks | `src/input/gamepad_hooks.cpp` | `IGamepad::GetCurrentReading` and `XInputGetState`; sampler thread; slot table with reuse |
| `MetricsSink` | `src/metrics/*` | Wait-free ring from the render thread to a diagnostics thread; CSV and summary |
| Platform | `src/platform/*` | Vtable and import-table redirection with restore, ABI facts computed from the SDK headers at compile time, bounds-checked PE reader, package identity |

### Threading and synchronisation

| Thread | Does | Shares |
|---|---|---|
| Game render thread | Runs the DXGI hooks and the governor | Frame records → diagnostics via SPSC ring (drops and counts when full); in steady state it takes no lock and allocates nothing, and it logs only on state changes |
| Game input thread (may be the same) | Runs the gamepad hook | Poll timestamp → governor via atomics; with the latch on, one mutex per pad held for one device read |
| Sampler (only when the latch is on, or until the WGI hook is installed) | Polls pads the game polled in the last second at `SamplerHz`; timer only, no busy-wait; idles when the game stops polling | The same per-pad mutex; a table mutex only against slot reuse |
| Diagnostics | Drains the ring every 50 ms, writes CSV, logs the summary | – |
| Initialisation | Runs once, from an APC on the main thread before the game's entry point or from a worker thread | Compare-and-swap; the loser waits (bounded) for the winner |

There are no cross-thread calls, no unbounded queues and no blocking of input acquisition behind presentation. The
sampler thread exists only because measurement justified it: in the integration test a once-per-frame poll saw
23 of 154 scripted 5 ms taps on Windows **[M]**.

### Failure handling

| Event | Response |
|---|---|
| Unsupported or unknown build | Measurement only |
| Swap-chain creation fails with added flags | Retried exactly as the game asked |
| `Present` returns `DXGI_ERROR_INVALID_CALL` after arguments were changed | Re-issued with the game's arguments in the same call; modification stops (safe mode) |
| `ResizeBuffers` fails with the added flags | Retried with the game's flags |
| Waitable object stops signalling | Bounded wait, then bypass |
| GPU query never completes or fails | 30 ms cap; query abandoned |
| Device removed / swap chain recreated | State follows the swap chain: destroyed on its last `Release`, rebuilt on the next creation |
| Refresh rate unknown | Relative cap and just-in-time stay off |
| Initialisation throws or a hook cannot be installed | Logged; the game continues unmodified |
| Injection fails | The game's main thread is always resumed |

---

## SECTION 7 — Implementation

The source tree is the implementation; this section is the map.

| Path | Contents |
|---|---|
| `src/dllmain.cpp` | Entry, one-time initialisation, exports `bll_wait_ready` and `bll_attach_existing` for the harness |
| `src/runtime.h` | Process-wide state shared by the hooks |
| `src/core/` | `config` (INI, presets), `log`, `stats` (rolling percentiles), `spsc_ring`, `time`, `version` (package name parsing and build classification) |
| `src/timing/` | `frame_governor`, `frame_pacer`, `precise_sleeper`, `adaptive_scheduler`, `bottleneck`, `refresh_estimator` |
| `src/input/` | `pad_state`, `transition_latch`, `latched_pad`, `gamepad_hooks` |
| `src/graphics/` | `dxgi_hooks`, `gpu_sync` |
| `src/metrics/` | `frame_record`, `metrics` |
| `src/platform/` | `hook` (vtable and import redirection), `abi` (slot indices and struct layout via `offsetof` on the SDK headers, with static assertions), `pe_image`, `package` |
| `tools/launcher/main.cpp` | Launcher and injector |
| `tools/probe/main.cpp` | Static import inspection |
| `tools/harness/` | Direct3D 11 test application; scripted `xinput1_4.dll` |
| `tools/analyze/analyze.py` | Statistics and comparison |
| `tests/` | 66 unit tests, the display model, the integration driver |

Language and toolchain: C++20 plus one C11 translation unit (the ABI facts are easiest to take from the C bindings
of the SDK headers). No third-party dependency. Built with GCC 13 (MinGW-w64) with `-Wall -Wextra -Wpedantic
-Wshadow -Wconversion -Werror`.

---

## SECTION 8 — Integration and compatibility

**How it attaches.** `bll-launcher` → package debugger registration → suspended game process → APC `LoadLibraryW`
→ `DllMain` queues a second APC and starts a worker → initialisation runs before the game's entry point → factory
vtable redirected → the game's swap-chain creation passes through the hook. If the DLL is loaded into a running
process instead, the worker hooks `Present` through a temporary swap chain and adopts the game's chain at its next
`Present`; creation-time features are then unavailable for that session.

**Compatibility checks performed at run time**

| Check | Where | Outcome if it fails |
|---|---|---|
| Package family and publisher | DLL, launcher, injector | Measurement only / launcher refuses / injector refuses |
| Game version 1.16.100.x | DLL, launcher | Measurement only / needs `--force` |
| x64 | DLL (classification), injector (`IsWow64Process`) | As above |
| Device is Direct3D 11 | at attach | D3D11 features off, warning |
| Swap effect is flip model | creation hook | No flags added |
| Tearing supported | creation hook | Tearing flag not added |
| Windowed | creation hook | Waitable flag not added |
| Interface available (`IDXGISwapChain2`, `IDXGIDevice1`, `IDXGIFactory5`) | each use | Feature off |
| Vtable identity | hook install | A second implementation class is not hooked |

**Compatibility matrix**

| Environment | State |
|---|---|
| Minecraft 1.16.100.x UWP x64, Windows 10/11 | Target. Untested |
| Other Minecraft UWP builds (incl. RenderDragon UWP) | Measurement only. Enabling features is possible and unsupported |
| Minecraft GDK builds | Not supported; the launcher mechanism does not apply |
| Direct3D 12 renderer | Detected, D3D11 features off |
| Xbox One / Series pads via Windows.Gaming.Input or XInput | Designed for; untested with hardware |
| Pads only visible through `RawGameController` or other APIs | Not hooked; rendering features unaffected |
| Exclusive full screen | Not applicable to UWP |
| Frame-generation or overlay DLLs hooking the same vtable | Coexist as long as they chain; this DLL restores a slot only if it still holds its own pointer |

**Multiplayer and gameplay.** No packet, tick rate, action rate or game state is touched. The latch changes only
*when* a short tap becomes visible to the game (one poll, so that it is seen at all); it cannot create input that did
not happen and replays at most one transition per button per poll.

---

## SECTION 9 — Build, installation and rollback

See [USAGE.md](USAGE.md). In short: build with MSYS2 or cross-compile with MinGW-w64; put `bll.dll` and
`bll-launcher.exe` in one folder; run the launcher; start the game normally to run without it. Nothing is installed
and nothing persists.

---

## SECTION 10 — Testing and results

Method and full tables are in [TESTING.md](TESTING.md). Summary of the evidence that exists:

**Unit tests [M]** – 66 tests pass natively under ASan/UBSan and as a Windows binary. They found one real design
flaw during development: the first just-in-time scheduler probed into a missed refresh about once per hundred frames
indefinitely, because it had no memory of *why* it missed. The learned margin replaced it.

**Integration [M]** – `bll.dll` in the Direct3D 11 harness, run in CI on a Windows Server 2022 machine (real DXGI
runtime, WARP rasteriser) and under Wine: all nine scenarios pass with zero failed presents. On Windows the waitable
swap chain was created through the hook, its latency set to 1 and its object waited on; added creation flags were
repeated on `ResizeBuffers`; a swap chain created before the DLL was loaded was adopted through a temporary
composition swap chain. With scripted 5 ms taps the application's once-per-frame poll saw 23 of 154 taps without the
latch and 138 of 138 with it; trigger pulls 97 of 97; stick values 0 ms old. At a 25 FPS cap the interval between
frame starts was 40.000 ms at the median, 39.810 ms at the 1st and 40.190 ms at the 99th percentile. The same suite
caught a wrong diagnosis during development: taps lost at a 32 Hz poll rate were first attributed to a starved
sampler thread; the cause was the tap rate exceeding what one transition per poll can convey (poll rate ÷ 2 taps per
second), which is a property of the latch, now documented.

These runs have no physical display, no physical controller, an HWND instead of a CoreWindow swap chain, and no
AppContainer. They establish that the hooks work against the real DXGI runtime, not what they gain.

**Model [Sim]** – 60 Hz, v-sync, game much faster than the display: input-poll-to-vblank 66.7 ms with the default
queue, 33.3 ms with device latency 1, 16.7 ms with a waitable swap chain, 7.7 ms median with just-in-time start at
four repeated refreshes in 3000 frames; with unpredictable 9 ms hitches the scheduler gives its delay up and matches
waitable mode. These numbers follow from the documented queue rules and the assumed workload. They are the
hypothesis, not the result.

**On the game** – nothing. No baseline, no before/after, no camera data.

---

## SECTION 11 — Remaining problems

| Problem | Why it is open |
|---|---|
| No test on Minecraft 1.16.100 | No game copy, no Windows machine with display and controller. The launcher, the start-up APC path, loading inside an AppContainer and the CoreWindow creation hook have never been executed; only the DLL loaded with `LoadLibrary` into the HWND harness has |
| Renderer, swap chain and controller API of the build are unconfirmed | Public sources do not state them; the log of the first run does |
| Camera update timing, smoothing and interpolation | Needs the game's internals. No address, signature or structure was invented to pretend otherwise. If `polls/frame` shows per-tick polling, the largest remaining latency is here and out of reach of this design |
| Late latching of the stick into an already started frame | Same reason |
| GPU time is not measured, only "pending or not" | D3D11 has no CPU/GPU clock correlation; the scheduler learns the margin from misses instead |
| `display_ms` semantics under tearing and composed flip | Frame statistics are loosely specified there; cross-check with PresentMon |
| Windows.Gaming.Input hook needs a connected gamepad to find the vtable | Retried every 500 ms; a pad's first readings before the hook are simply not counted |
| `RawGameController` and other pad APIs | Not hooked |
| No test with a physical display | Waitable swap chain and frame statistics ran on a CI machine without a monitor; vblank timing, the just-in-time scheduler and `display_ms` are untested against real refresh behaviour |
| MSVC build | Untested |
| Unloading the DLL at run time | Hooks are restored, threads are not joined; the supported way to disable is not to load it |

Known behaviours to expect: `Mode=1` tears on fixed-refresh displays; queue depth 1 lowers throughput when the game
cannot hold the refresh rate; a frame cap adds latency when the game would otherwise run far above it without
v-sync; the latch delays the *release* of a recovered tap by one poll.

---

## SECTION 12 — Further optimisations

Ordered by expected value, then difficulty.

1. **Run the protocol.** Mode 0 on the real game answers the open questions of Section 2 in one session. Trivial,
   and everything else depends on it.
2. **Variable refresh rate + cap below refresh + waitable swap chain.** Likely the best practical configuration on
   VRR displays; already configurable (`Mode=5`, `TargetFPS=-3`). Needs only measurement.
3. **Vblank-phase-aware just-in-time.** Aim at the measured vblank time from frame statistics instead of the moment
   the wait returned. Moderate; removes the dependence on when the waitable object happens to signal.
4. **GPU duration via timestamp-disjoint queries.** Would let the scheduler budget GPU time instead of learning it
   from misses. Moderate.
5. **Independent input source (`CreateIndependentInputSource`) style acquisition** for keyboard/mouse, following
   Microsoft's guidance. Low relevance to gamepads, moderate effort.
6. **`RawGameController` hook** for pads that do not surface as `Gamepad`. Easy once a need is shown.
7. **Camera late-latch.** Only with verified knowledge of the build's view-matrix path, a per-version compatibility
   record and a refusal to run elsewhere. Hard, and the only item that requires reverse engineering.
8. **Mojang's "Improved Input Response" (1.20.30).** The release notes say the option reduces input latency at the
   cost of battery life and nothing about how. It cannot be back-ported because it is not known what it is; Modes
   2 and 4 are an independent attempt at the same goal with documented techniques.
