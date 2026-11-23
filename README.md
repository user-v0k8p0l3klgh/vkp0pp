# BedrockLowLatency

A native, reversible low-latency layer for **Minecraft Bedrock Edition 1.16.100 (UWP, x64, Windows 10/11)** with
first-class gamepad support. It shortens the path from a controller input to the frame that shows it by controlling
the DXGI presentation queue, by placing every wait *before* the game samples input instead of after it has rendered,
and by preserving button and trigger taps that are shorter than the game's poll interval.

It is a DLL (`bll.dll`) loaded into the game at start-up by a small launcher. It works exclusively through documented
Windows interfaces (DXGI, Direct3D 11, Windows.Gaming.Input, XInput): **no game address, signature or offset is used,
no game file is modified, and nothing persists after the game closes.**

## Status — read this first

| | |
|---|---|
| Builds | MinGW-w64, cross-compiled on Linux and natively with MSYS2 (both in CI), warning-free with `-Werror` |
| Unit tests | 66 tests of the timing, input, configuration and PE logic; on Linux with ASan/UBSan and on Windows |
| Integration tests | `bll.dll` loaded into a real Direct3D 11 process (the bundled harness), in CI on Windows (real DXGI runtime) and under Wine: waitable swap chain, creation flags, `ResizeBuffers`, late attach, frame pacing, controller path |
| **Tested against Minecraft 1.16.100** | **No.** No copy of the game and no Windows machine with a display and a controller were available during development |
| Measured latency improvement in the game | **None claimed.** [docs/TESTING.md](docs/TESTING.md) is the protocol to measure it |

Everything that depends on how this particular game build behaves (which graphics API it uses, how it presents,
which controller API it polls and when) is detected at run time and written to the log; the first launch tells you
which of the assumptions in [docs/REPORT.md](docs/REPORT.md) hold on your installation. On any build other than
the 1.16.100 release (package `1.16.10004.x`) the DLL only measures and changes nothing.

## What it does

| Mode | Feature | Mechanism |
|---|---|---|
| 0 | Vanilla | Measurement only: frame times, input age, bottleneck classification |
| 1 | V-Sync optimisation | Sync interval 0 with `DXGI_PRESENT_ALLOW_TEARING` on a tearing-capable flip-model swap chain |
| 2 (default) | Low-latency queue | `IDXGIDevice1::SetMaximumFrameLatency(1)` and a waitable swap chain (`IDXGISwapChain2::SetMaximumFrameLatency(1)`), waited on *before* the next frame starts |
| 3 | Frame pacing | Deadline-based limiter with timer + adaptive busy-wait tail, applied at frame start |
| 4 | Just-in-time (experimental) | Delays the frame start towards the refresh deadline, learns its safety margin from missed refreshes; waits for the GPU to finish the previous frame |
| 5 | Combined | 1 (tearing only when the game itself presents unsynchronised) + 2 + 3 + transition latch |

Independent of the mode:

* **Gamepad instrumentation** – every controller poll the game makes is timestamped; the log reports polls per frame
  and how old the input was when each frame was presented.
* **Transition latch** (`BufferButtonTransitions`) – a 1000 Hz sampler preserves button and trigger taps that begin
  and end between two game polls. Stick values are never buffered or smoothed.

## Quick start

1. Build (or download the CI artifact): see [docs/USAGE.md](docs/USAGE.md#building).
2. Put `bll.dll` and `bll-launcher.exe` in one folder and run `bll-launcher.exe`.
3. Read the log the launcher points to, then follow [docs/TESTING.md](docs/TESTING.md) – baseline first.

To stop using it, start Minecraft normally. Nothing else needs to be undone.

## Documentation

* [docs/REPORT.md](docs/REPORT.md) – research, source audit, API analysis, design, results, limitations
* [docs/USAGE.md](docs/USAGE.md) – building, installing, configuration reference, reading the log, rollback
* [docs/TESTING.md](docs/TESTING.md) – measurement protocol, controller compatibility checklist, analysis tool

## Repository layout

```
src/core        configuration, logging, statistics, build identification
src/timing      frame governor, frame pacer, precise sleeper, just-in-time scheduler, bottleneck detector
src/input       transition latch, gamepad hooks (Windows.Gaming.Input and XInput)
src/graphics    DXGI hooks, GPU event-query synchronisation
src/metrics     per-frame records, CSV writer, periodic summary
src/platform    vtable / import redirection, ABI facts, PE reader, package identity
tools/launcher  starts the game with the DLL loaded
tools/probe     static inspection of an executable's graphics and input imports
tools/harness   Direct3D 11 test application and scripted controller
tools/analyze   statistics for frame logs, PresentMon captures and camera trials
tests           unit tests, virtual-time latency model, integration driver
```

## Licence

MIT. No third-party code is included; the projects studied are listed with their licences in
[docs/REPORT.md](docs/REPORT.md#section-3--github-source-analysis).
