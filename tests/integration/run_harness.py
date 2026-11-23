#!/usr/bin/env python3
"""Integration test of bll.dll in a live Direct3D 11 process.

The harness (tools/harness) is a real Direct3D 11 application with a
game-shaped loop; bll.dll is loaded into it exactly as it is loaded into the
game and hooks the same DXGI / XInput entry points. This verifies the hook
plumbing, the fall-backs and the controller path end to end.

  tests/integration/run_harness.py <windows-build-dir>          on Windows
  tests/integration/run_harness.py <windows-build-dir> --wine   on Linux (wine, xvfb-run)

Under Wine on a virtual framebuffer there is no real vblank, no flip-model
presentation queue and no waitable swap chain, so that run checks behaviour,
not display latency. On Windows the real DXGI runtime is exercised.
"""
import argparse
import csv
import os
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

DIAG = "[Diagnostics]\nRecordFrameTimings=true\nSummaryIntervalSeconds=1\n"
ALLOW = "[General]\nAllowUnrecognizedBuild=true\n"


class Suite:
    def __init__(self, build, work, wine):
        self.build = Path(build).resolve()
        self.work = Path(work).resolve()
        self.wine = wine
        self.failed = 0
        self.env = dict(os.environ)
        if wine:
            self.env.setdefault("WINEPREFIX", str(self.work / "prefix"))
            self.env["WINEDEBUG"] = "-all"
            # The scripted controller next to the harness must win over Wine's built-in xinput.
            self.env["WINEDLLOVERRIDES"] = "mscoree,mshtml=;xinput1_4=n"

    def path(self, p):
        return "Z:" + str(p).replace("/", "\\") if self.wine else str(p)

    def execute(self, args, cwd, extra_env=None):
        env = dict(self.env)
        env.update(extra_env or {})
        if self.wine:
            args = ["xvfb-run", "-a", "wine"] + args
        else:
            args = [str(Path(cwd) / args[0])] + args[1:]
        return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True, timeout=240)

    def run(self, name, ini, args, with_dll=True, late=False):
        home = self.work / name
        home.mkdir(parents=True, exist_ok=True)
        if ini is not None:
            (home / "bll.ini").write_text(ini)
        cmd = ["bll-harness.exe"]
        if with_dll:
            cmd += ["--dll", self.path(self.build / "bll.dll")]
        if late:
            cmd += ["--late"]
        proc = self.execute(cmd + args, self.build / "harness", {"BLL_HOME": self.path(home)})
        log = home / "bll.log"
        (home / "harness.out").write_text(proc.stdout + proc.stderr)
        return Run(home, proc.returncode, proc.stdout, log.read_text(errors="replace") if log.exists() else "")

    def check(self, description, ok):
        print(f"  {'ok  ' if ok else 'FAIL'}  {description}", flush=True)
        if not ok:
            self.failed += 1


class Run:
    def __init__(self, home, exit_code, out, log):
        self.home, self.exit_code, self.out, self.log = home, exit_code, out, log

    def result(self, key):
        match = re.search(rf"\b{key}=(\S+)", self.out)
        return int(match.group(1), 0) if match else -1

    def frame_start_intervals(self):
        with open(self.home / "frames.csv", newline="") as handle:
            starts = [int(r["frame_start_ns"]) for r in csv.DictReader(handle) if r["frame"] != "0"]
        starts = starts[len(starts) // 4 :]  # skip start-up
        return [(b - a) * 1e-6 for a, b in zip(starts, starts[1:])]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("build")
    parser.add_argument("--wine", action="store_true", help="run the Windows binaries under Wine")
    parser.add_argument("--keep", help="directory for logs (default: a temporary directory)")
    opts = parser.parse_args()
    work = Path(opts.keep) if opts.keep else Path(tempfile.mkdtemp(prefix="bll-it."))
    work.mkdir(parents=True, exist_ok=True)
    s = Suite(opts.build, work, opts.wine)

    if opts.wine:
        print(f"preparing Wine prefix in {s.env['WINEPREFIX']}", flush=True)
        subprocess.run(["xvfb-run", "-a", "wine", "wineboot", "-i"], env=s.env, capture_output=True, timeout=600)

    print("1. harness alone (no DLL), 60 fps-like loop: once-per-frame polling loses short taps")
    r = s.run("baseline", None, ["--frames", "400", "--work-ms", "16"], with_dll=False)
    s.check("harness runs without the DLL", r.exit_code == 0)
    base_seen, base_emitted = r.result("taps_seen"), r.result("taps_emitted")
    print(f"        taps seen without the latch: {base_seen} of {base_emitted}")
    s.check("baseline loses more than half of the 5 ms taps", 0 <= base_seen * 2 < base_emitted)

    print("2. unrecognised build, default configuration: measurement only")
    r = s.run("default", DIAG, ["--frames", "200", "--work-ms", "5", "--resize"])
    s.check("no present failures", r.result("present_failures") == 0)
    s.check("log reports measurement-only mode", "measurement-only mode" in r.log)
    s.check("no creation flag was added", "added 0x0," in r.log)
    s.check("device latency was not touched", "SetMaximumFrameLatency" not in r.log)
    s.check(
        "presents were passed through unchanged",
        "game asked (sync=1 flags=0x0), issued (sync=1 flags=0x0)" in r.log,
    )
    s.check(
        "ResizeBuffers passed through with the application's flags",
        "ResizeBuffers(0, 800x450, format 0, flags 0x0) -> 0x00000000" in r.log,
    )
    s.check("input polls are observed", "polls/frame=1.00" in r.log)
    csv_path = r.home / "frames.csv"
    s.check("frame CSV was written", csv_path.exists() and len(csv_path.read_text().splitlines()) > 150)

    print("3. transition latch only: taps and trigger pulls are preserved, sticks stay fresh")
    r = s.run(
        "latch",
        ALLOW + "Mode=0\n[Input]\nBufferButtonTransitions=true\n" + DIAG,
        ["--frames", "400", "--work-ms", "16"],
    )
    seen, emitted = r.result("taps_seen"), r.result("taps_emitted")
    pulls_seen, pulls = r.result("pulls_seen"), r.result("pulls_emitted")
    age = r.result("max_analog_age_ms")
    print(f"        with the latch: taps {seen} of {emitted}, trigger pulls {pulls_seen} of {pulls}, max stick age {age} ms")
    s.check("at least 95% of taps reach the application", emitted > 0 and seen * 100 >= emitted * 95)
    s.check("no tap is invented", seen <= emitted + 1)
    s.check("at least 95% of trigger pulls reach the application", pulls > 0 and pulls_seen * 100 >= pulls * 95)
    s.check("no trigger pull is invented", pulls_seen <= pulls + 1)
    s.check("stick values are at most 2 ms old", 0 <= age <= 2)
    s.check("XInput import was redirected", "1 XInputGetState import slot(s) redirected" in r.log)

    print("4. frame pacing at 50 FPS")
    r = s.run("pacing", ALLOW + "Mode=3\n[Rendering]\nTargetFPS=50\n" + DIAG, ["--frames", "400", "--work-ms", "5"])
    s.check("no present failures", r.result("present_failures") == 0)
    intervals = sorted(r.frame_start_intervals())
    if intervals:
        median = statistics.median(intervals)
        p1, p99 = intervals[len(intervals) // 100], intervals[len(intervals) * 99 // 100]
        print(f"        frame start interval: median {median:.3f} ms, p1 {p1:.3f}, p99 {p99:.3f}, n={len(intervals)}")
        s.check("median frame start interval is 20.00 ms +- 0.05", abs(median - 20.0) <= 0.05)
        s.check("p1..p99 of the frame start interval within +-1 ms", p1 >= 19.0 and p99 <= 21.0)
    else:
        s.check("frame CSV has data", False)
    s.check("frames are classified as frame-cap limited", "limit=frame-cap" in r.log)

    print("5. combined mode with a resize: creation flags are kept consistent")
    r = s.run(
        "combined", ALLOW + "Mode=5\n[Rendering]\nTargetFPS=50\n" + DIAG, ["--frames", "300", "--work-ms", "5", "--resize"]
    )
    s.check("no present failures", r.result("present_failures") == 0)
    s.check("device-level frame latency set to 1", "IDXGIDevice1::SetMaximumFrameLatency(1) -> 0x00000000" in r.log)
    added = re.search(r"added (0x[0-9A-Fa-f]+),", r.log)
    resized = re.search(r"ResizeBuffers\(0, 800x450, format 0, flags (0x[0-9A-Fa-f]+)\) -> 0x00000000", r.log)
    s.check(
        "ResizeBuffers repeated the creation flags that were added",
        bool(added and resized and int(added.group(1), 16) != 0 and added.group(1) == resized.group(1)),
    )
    s.check("swap chain destruction is tracked", "destroyed" in r.log)
    if not opts.wine:
        # Real DXGI only: Wine has no IDXGISwapChain2.
        s.check("waitable object acquired with swap-chain latency 1", "waitable object acquired" in r.log)
        s.check("frames wait on the waitable object", "waitable=1" in r.log)

    print("6. v-sync override")
    r = s.run("vsync", ALLOW + "Mode=1\n" + DIAG, ["--frames", "200", "--work-ms", "5"])
    s.check("no present failures", r.result("present_failures") == 0)
    s.check("sync interval forced to 0", "game asked (sync=1 flags=0x0), issued (sync=0" in r.log)

    print("7. experimental just-in-time mode")
    r = s.run("jit", ALLOW + "Mode=4\n" + DIAG, ["--frames", "300", "--work-ms", "5"])
    s.check("no present failures", r.result("present_failures") == 0)
    s.check("GPU event queries active", "gpu_sync=1" in r.log)

    print("8. DLL loaded after the swap chain exists (late attach)")
    r = s.run("late", ALLOW + "Mode=2\n" + DIAG, ["--frames", "200", "--work-ms", "5"], late=True)
    s.check("no present failures", r.result("present_failures") == 0)
    s.check("existing swap chain adopted at Present", "adopted at Present" in r.log)
    s.check("device-level frame latency set to 1", "IDXGIDevice1::SetMaximumFrameLatency(1) -> 0x00000000" in r.log)

    print("9. Enabled=false hooks nothing")
    r = s.run("disabled", "[General]\nEnabled=false\n", ["--frames", "100", "--work-ms", "2"])
    s.check("no present failures", r.result("present_failures") == 0)
    s.check("nothing hooked", "nothing is hooked" in r.log and "factory hooks" not in r.log)

    print()
    print(f"{'all integration checks passed' if s.failed == 0 else str(s.failed) + ' check(s) FAILED'} (logs in {work})")
    return 1 if s.failed else 0


if __name__ == "__main__":
    sys.exit(main())
