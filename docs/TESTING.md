# Testing and measurement

Nothing in this project has been measured on the real game. This document is the procedure for doing so, and the
record of what *has* been verified.

## 1. What is verified today

| Evidence | Where | What it shows | What it does not show |
|---|---|---|---|
| 66 unit tests | `tests/`, run natively with ASan + UBSan and as a Windows binary | The algorithms behave as specified: latch loses and invents nothing, pacer keeps its grid, scheduler backs off, parsers reject malformed input | Anything about Windows or the game |
| Virtual-time latency model | `tests/sim.h`, `tests/governor_tests.cpp` | Under the *documented* DXGI queue semantics the modes order as expected | That a real compositor, driver or this game behaves like the model |
| Integration suite | `tests/integration/run_harness.py`, run in CI on Windows and under Wine | `bll.dll` hooks a live Direct3D 11 process correctly: creation flags, waitable object, `Present`, `ResizeBuffers`, late attach, fall-backs, controller path | Display latency (no physical display). The harness is not Minecraft, uses an HWND rather than a CoreWindow swap chain, and is not started through the launcher |

Results of the integration suite, from the CI run on commit `40cff1c`. "Windows" is a GitHub-hosted Windows Server
2022 runner using the real DXGI runtime with the WARP software rasteriser; "Wine" is Wine 9.0 on a virtual
framebuffer. Neither has a physical display or controller: the controller is the scripted `xinput1_4.dll`.

| Check | Windows (real DXGI) | Wine |
|---|---|---|
| 5 ms button taps seen by a once-per-frame poll, no DLL | 23 of 154 | 23 of 81 |
| Same with `BufferButtonTransitions=true` | 138 of 138 | 80 of 80 |
| 5 ms trigger pulls with the latch | 97 of 97 | 57 of 57 |
| Age of the stick value handed to the application, latch on | 0 ms | ≤ 1 ms |
| Frame-start interval at `TargetFPS=25` (median / p1 / p99) | 40.000 / 39.810 / 40.190 ms | 40.000 / 40.000 / 40.000 ms |
| Waitable swap chain created, `IDXGISwapChain2::SetMaximumFrameLatency(1)`, object acquired and waited on | yes | not available in Wine (fall-back path taken) |
| Creation flags repeated on `ResizeBuffers` | yes | yes |
| Late attach (Present hooked through a temporary swap chain, game chain adopted) | yes, via a composition swap chain | yes, via a hidden-window swap chain |
| Failed `Present` calls across all nine scenarios | 0 | 0 |

Local Wine runs during development delivered 97–100 % of taps with the latch. The remaining misses were not traced
individually; one per run is inherent to the test (a tap still being replayed when the run ends).

**A limit this test exposed.** A polled interface conveys at most one transition per poll, so the latch can deliver
at most *poll rate ÷ 2* taps per second per button. The first Windows run had the harness polling at ~32 Hz with taps
every 50 ms (20 per second): the latch delivered 196 of 221 and collapsed the rest, as designed (bounded replay,
never a stuck button). The scripted taps are now 120 ms apart. For a game polling at 60 Hz the limit is 30 taps per
second per button, beyond what a hand can do; at a 20 Hz poll it would be 10 per second.

Model output (60 Hz, v-sync on, 3 ms CPU + 2 ms GPU per frame; **a model, not a measurement**):

| Configuration | Input poll → vblank, median | p99 |
|---|---|---|
| Default DXGI queue (3 frames), blocking `Present` | 66.7 ms | 66.7 ms |
| Device-level maximum frame latency 1 | 33.3 ms | 33.3 ms |
| Waitable swap chain, latency 1, waited on before the frame | 16.7 ms | 16.7 ms |
| Plus just-in-time start | 7.7 ms | 12.5 ms (4 repeated refreshes in 3000 frames) |

These are the hypotheses the procedure below is designed to confirm or refute.

## 2. Running the automated tests

```
# unit tests
cmake -S . -B build -G Ninja -DBLL_SANITIZE=ON && cmake --build build && ./build/bll_tests

# integration, Windows
python tests/integration/run_harness.py build

# integration, Linux (needs wine and xvfb-run)
python3 tests/integration/run_harness.py build-win --wine
```

## 3. Measuring on the real game

### 3.1 Keep constant

Same world, same position and view (use a fixed spot facing a wall with one block to hit), same game version, monitor,
refresh rate, resolution, window mode, graphics settings, render distance, controller, controller connection, USB
port, and nothing else running. Note all of them with the results. Warm the game up for a minute before recording.

### 3.2 Variants

Run each variant as a separate launch. Change one thing at a time; combine only what was individually confirmed.

| Variant | `bll.ini` |
|---|---|
| Baseline 0 | Game started normally, no launcher (camera test and PresentMon only) |
| Baseline 1 | `Mode=0` – must match Baseline 0 within noise; this proves the instrumentation is neutral |
| A – v-sync | `Mode=1` |
| B – queue | `Mode=0`, `LowLatencyQueue=true` (device level only); then `Mode=2` (adds the waitable swap chain) |
| C – pacing | `Mode=3` with `TargetFPS` = refresh rate, refresh − 3, and (with `DisableVSync=true`) above refresh |
| D/E – input timing | No separate switch: late sampling is a consequence of B and C. Compare `input_age_ms` across variants |
| F – transitions | `Mode=0`, `BufferButtonTransitions=true` |
| G – adaptive | `Mode=4` |
| H – combined | `Mode=5`, plus whatever was individually confirmed |

Test each at 60 Hz and at the display's highest refresh rate, and once in a GPU-heavy scene (maximum render
distance) and once in a light one.

### 3.3 Software measurements (every variant)

1. Set `RecordFrameTimings=true`, play the fixed scene for 60 s moving the right stick continuously.
2. `python tools/analyze/analyze.py frames frames.csv` – frame time percentiles, 1 % / 0.1 % lows, input age,
   present-to-display, polls per frame, what limited the frame rate, missed refreshes.
3. In parallel capture PresentMon from an elevated prompt:
   `PresentMon-<ver>-x64.exe --process_name Minecraft.Windows.exe --output_file pm.csv --timed 60 --terminate_after_timed`
   then `python tools/analyze/analyze.py presentmon pm.csv`. Check `PresentMode`, `SyncInterval`, `AllowsTearing`
   and the present-to-display column against the DLL's own numbers.
4. Record CPU and GPU utilisation (Task Manager or PresentMon's columns) to quantify overhead.

`input->display` from the log is the software part of the chain only. It moves in the same direction as
controller-to-photon latency but is not equal to it.

### 3.4 Controller diagnostics

Measure the controller's report interval and jitter with
[gamepadla-plus](https://github.com/WyvernIXTL/gamepadla-plus) for each connection type you test. This bounds what
any software can achieve: the latch cannot see a tap the controller never reported, and a 125 Hz controller adds up to
8 ms regardless of the game. A report rate is not an end-to-end latency.

### 3.5 Camera test (the primary metric)

Equipment: a camera at 240 FPS or more (a phone's slow-motion mode is enough), and a way to see the *electrical*
input. Best is an LED wired across the button's contacts or a controller with exposed test points; without that,
film the button and thumb and accept ±1–2 frames more uncertainty.

Framing: controller and screen in one shot, exposure fixed, screen brightness high.

Trials, at least 30 per action per variant:

| Action | Input event | Screen event |
|---|---|---|
| Camera | Right stick leaves centre (flick from rest) | First frame in which the view has shifted |
| Jump | A pressed | Player's view starts to rise |
| Attack | Right trigger pulled | Arm swing begins |
| Place | Left trigger pulled | Block appears / arm swing begins |

Count camera frames from input event to screen event, one row per trial in a CSV with a `frames` column, then:

```
python tools/analyze/analyze.py camera camera_baseline.csv --fps 240
python tools/analyze/analyze.py compare camera_baseline.csv camera_mode2.csv --kind camera --fps 240
```

`Latency_ms = frames × 1000 / camera_fps`; each trial is quantised to ±1 camera frame (±4.2 ms at 240 FPS). The
compare command reports the median difference with a bootstrap 95 % interval and a Mann–Whitney test, and only calls a
difference credible when the interval excludes zero, p < 0.01 and the difference exceeds one camera frame.

Report per variant: median, P95, P99, standard deviation, FPS, frame-time consistency, CPU/GPU load, and any crash,
stutter, tearing or input anomaly. Repeat a variant on another day before believing a small difference.

### 3.6 Decision rule

Keep a feature only if the camera test shows a credible improvement **and** the controller checklist below passes
**and** 1 % lows did not get worse by more than you are willing to trade. If a feature measures worse or the same,
turn it off; the project makes no claim that every mode helps on every system.

## 4. Controller compatibility checklist

For each controller and connection you care about (Xbox One / Series over USB, Bluetooth and Xbox Wireless Adapter;
other XInput pads; PlayStation pads as Windows exposes them; pads behind remapping software), with the latch off and
then on:

* Left stick: smooth analog walking in every direction, full speed at full deflection.
* Right stick: slow and fast camera movement, no added acceleration, no jitter, dead zone unchanged.
* Triggers, bumpers, face buttons, d-pad, stick clicks, menu buttons: each does what the game's mapping says.
* Inventory and menus: cursor/d-pad navigation, no skipped or doubled steps.
* Tap each button as briefly as you can 50 times: 50 actions, no more, no fewer.
* Hold each button for 10 s: exactly one press and one release; no auto-repeat that vanilla does not have.
* Mash two buttons alternately: no stuck button afterwards.
* Unplug and replug (or power-cycle) the controller in a world: input resumes, nothing is stuck.
* Alt-tab away and back, minimise and restore, resize the window, toggle full screen: input and rendering resume.
* Use keyboard/mouse and controller alternately.
* Vibration, if the game uses it, still works.
* Repeat the short-tap and hold checks at 30, 60, 120, 144 FPS and the highest rate you can reach (`TargetFPS`).

Record `taps_preserved`, `collapsed` and `stale_resets` from the log for each run with the latch on.

## 5. Reporting results

Please include: the first 30 lines of `bll.log`, the `analyze.py frames` output per variant, PresentMon summary,
camera CSVs, hardware (GPU, display and refresh rate, controller and connection), Windows build, and the variant
table. Negative results are as valuable as positive ones.
