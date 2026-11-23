#!/usr/bin/env python3
"""Statistics for BedrockLowLatency measurements. Standard library only.

  analyze.py frames  <frames.csv>                 summary of a bll.dll frame log
  analyze.py camera  <trials.csv> --fps 240       high-speed camera trials
  analyze.py presentmon <presentmon.csv>          summary of a PresentMon capture
  analyze.py compare <a> <b> --kind frames|camera [--column input_age_ns] [--fps 240]
                                                  is B credibly different from A?

Camera trial files are CSV with a header and one row per trial; the column
`frames` holds the number of camera frames between the input event (LED or
contact closing) and the first visible change on screen.
"""
import argparse
import csv
import math
import random
import statistics
import sys


def percentile(values, p):
    """Nearest-rank percentile, the same definition bll.dll uses."""
    if not values:
        return float("nan")
    ordered = sorted(values)
    rank = max(1, math.ceil(p / 100.0 * len(ordered)))
    return ordered[min(rank, len(ordered)) - 1]


def describe(values, unit="ms"):
    if not values:
        return "no samples"
    sd = statistics.stdev(values) if len(values) > 1 else 0.0
    return (
        f"n={len(values)} median={statistics.median(values):.2f} mean={statistics.fmean(values):.2f} "
        f"p90={percentile(values, 90):.2f} p95={percentile(values, 95):.2f} p99={percentile(values, 99):.2f} "
        f"sd={sd:.2f} min={min(values):.2f} max={max(values):.2f} {unit}"
    )


def read_rows(path):
    with open(path, newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def column_ms(rows, name, scale=1e-6, keep=lambda v: v >= 0):
    out = []
    for row in rows:
        raw = row.get(name, "")
        if raw in ("", "NA", None):
            continue
        try:
            value = float(raw)
        except ValueError:
            continue
        if keep(value):
            out.append(value * scale)
    return out


def load_frames(path):
    # Rows with frame == 0 are presents that were not displayed (test, occluded, failed).
    return [r for r in read_rows(path) if r.get("frame", "0") != "0"]


def frames_metrics(rows):
    starts = [int(r["frame_start_ns"]) for r in rows if int(r["frame_start_ns"]) > 0]
    start_intervals = [(b - a) * 1e-6 for a, b in zip(starts, starts[1:]) if b > a]
    return {
        "frame time (present to present)": column_ms(rows, "interval_ns", keep=lambda v: v > 0),
        "frame start interval": start_intervals,
        "input age at present": column_ms(rows, "input_age_ns"),
        "present call to display": column_ms(rows, "display_latency_ns"),
        "time inside Present": [
            (int(r["present_return_ns"]) - int(r["present_call_ns"])) * 1e-6 for r in rows
        ],
    }


def cmd_frames(args):
    rows = load_frames(args.file)
    if not rows:
        sys.exit("no displayed frames in file")
    metrics = frames_metrics(rows)
    for name, values in metrics.items():
        print(f"{name:34s} {describe(values)}")

    ft = metrics["frame time (present to present)"]
    if ft:
        print(f"{'average FPS':34s} {1000.0 / statistics.fmean(ft):.1f}")
        print(f"{'1% low FPS':34s} {1000.0 / percentile(ft, 99):.1f}")
        print(f"{'0.1% low FPS':34s} {1000.0 / percentile(ft, 99.9):.1f}")
    age = metrics["input age at present"]
    disp = metrics["present call to display"]
    if age and disp:
        total = statistics.median(age) + statistics.median(disp)
        print(f"{'input poll to display (medians)':34s} {total:.2f} ms  (software part of the chain only)")
    polls = [int(r["input_polls"]) for r in rows]
    print(f"{'gamepad polls per frame':34s} {statistics.fmean(polls):.2f}")

    def gate(col):
        return statistics.fmean(int(r[col]) for r in rows) * 1e-6

    print(
        f"{'mean wait before frame start':34s} latency={gate('wait_latency_ns'):.2f} gpu={gate('wait_gpu_ns'):.2f} "
        f"pacer={gate('wait_pacer_ns'):.2f} jit={gate('wait_jit_ns'):.2f} ms"
    )
    classes = {}
    for r in rows:
        classes[r["class"]] = classes.get(r["class"], 0) + 1
    share = ", ".join(f"{k} {100.0 * v / len(rows):.0f}%" for k, v in sorted(classes.items(), key=lambda kv: -kv[1]))
    print(f"{'frame limited by':34s} {share}")
    print(f"{'missed refreshes':34s} {sum(int(r['missed_refresh']) for r in rows)}")
    print(f"{'failed presents':34s} {sum(1 for r in rows if r['hresult'].lower().startswith('0x8'))}")


def camera_latencies(path, fps):
    rows = read_rows(path)
    if rows and "frames" not in rows[0]:
        sys.exit(f"{path}: expected a 'frames' column")
    return [float(r["frames"]) * 1000.0 / fps for r in rows if r.get("frames", "") != ""]


def cmd_camera(args):
    values = camera_latencies(args.file, args.fps)
    print(describe(values))
    # Both the input event and the first screen change are only known to the
    # nearest camera frame, so each trial carries +-1 frame of quantisation.
    print(f"per-trial resolution: +-{1000.0 / args.fps:.2f} ms (one camera frame)")
    if len(values) > 1:
        sem = statistics.stdev(values) / math.sqrt(len(values))
        print(f"mean {statistics.fmean(values):.2f} ms, standard error {sem:.2f} ms over {len(values)} trials")
    if len(values) < 20:
        print("warning: fewer than 20 trials; percentiles above the median are not meaningful")


PRESENTMON_COLUMNS = [
    # (label, candidate column names across PresentMon versions)
    ("frame time", ["MsBetweenPresents", "FrameTime"]),
    ("displayed frame interval", ["MsBetweenDisplayChange", "DisplayedTime"]),
    ("time inside Present", ["MsInPresentAPI"]),
    ("present to display", ["MsUntilDisplayed", "DisplayLatency"]),
    ("GPU busy", ["MsGPUBusy", "GPUBusy"]),
    ("GPU latency", ["GPULatency", "MsRenderPresentLatency"]),
    ("CPU busy", ["MsCPUBusy", "CPUBusy"]),
]


def cmd_presentmon(args):
    rows = read_rows(args.file)
    if not rows:
        sys.exit("empty capture")
    for label, names in PRESENTMON_COLUMNS:
        name = next((n for n in names if n in rows[0]), None)
        if name:
            print(f"{label + ' (' + name + ')':44s} {describe(column_ms(rows, name, scale=1.0))}")
    for name in ("PresentMode", "SyncInterval", "AllowsTearing", "PresentRuntime"):
        if name in rows[0]:
            counts = {}
            for r in rows:
                counts[r[name]] = counts.get(r[name], 0) + 1
            print(f"{name:44s} " + ", ".join(f"{k}: {v}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1])))
    print("note: PresentMon measures presentation, not the controller; add input age from bll.dll or a camera test.")


def mann_whitney(a, b):
    """Two-sided Mann-Whitney U test, normal approximation with tie correction."""
    combined = sorted([(v, 0) for v in a] + [(v, 1) for v in b])
    ranks = [0.0] * len(combined)
    tie_term = 0.0
    i = 0
    while i < len(combined):
        j = i
        while j + 1 < len(combined) and combined[j + 1][0] == combined[i][0]:
            j += 1
        rank = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            ranks[k] = rank
        t = j - i + 1
        tie_term += t**3 - t
        i = j + 1
    n1, n2 = len(a), len(b)
    r1 = sum(rank for rank, (_, group) in zip(ranks, combined) if group == 0)
    u1 = r1 - n1 * (n1 + 1) / 2.0
    n = n1 + n2
    sigma = math.sqrt(n1 * n2 / 12.0 * ((n + 1) - tie_term / (n * (n - 1))))
    if sigma == 0:
        return u1, 1.0
    z = (u1 - n1 * n2 / 2.0) / sigma
    return u1, math.erfc(abs(z) / math.sqrt(2.0))


def bootstrap_median_difference(a, b, rounds=4000, seed=1):
    rng = random.Random(seed)
    diffs = []
    for _ in range(rounds):
        ra = [rng.choice(a) for _ in a]
        rb = [rng.choice(b) for _ in b]
        diffs.append(statistics.median(rb) - statistics.median(ra))
    diffs.sort()
    return diffs[int(0.025 * rounds)], diffs[int(0.975 * rounds) - 1]


def cmd_compare(args):
    if args.kind == "camera":
        a, b = camera_latencies(args.a, args.fps), camera_latencies(args.b, args.fps)
        floor = 1000.0 / args.fps  # differences below one camera frame cannot be resolved
    else:
        scale = 1e-6 if args.column.endswith("_ns") else 1.0
        a = column_ms(load_frames(args.a), args.column, scale)
        b = column_ms(load_frames(args.b), args.column, scale)
        floor = 0.0
    if len(a) < 2 or len(b) < 2:
        sys.exit("need at least two samples in each file")
    # Very long frame logs are thinned so the bootstrap stays fast; adjacent
    # frames are strongly correlated anyway.
    limit = 5000
    a = a[:: max(1, len(a) // limit)]
    b = b[:: max(1, len(b) // limit)]

    print(f"A: {describe(a)}")
    print(f"B: {describe(b)}")
    diff = statistics.median(b) - statistics.median(a)
    low, high = bootstrap_median_difference(a, b)
    _, p = mann_whitney(a, b)
    print(f"median difference B-A: {diff:+.2f} ms, 95% bootstrap interval [{low:+.2f}, {high:+.2f}] ms")
    print(f"Mann-Whitney U two-sided p = {p:.4g}")
    credible = p < 0.01 and (low > 0 or high < 0) and abs(diff) > floor
    if credible:
        print(f"verdict: B is credibly {'slower' if diff > 0 else 'faster'} than A")
    else:
        print("verdict: no credible difference (interval includes zero, p >= 0.01, or below measurement resolution)")
    if args.kind == "frames":
        print("note: frames within one run are not independent; repeat the run several times before trusting small differences.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("frames")
    p.add_argument("file")
    p.set_defaults(func=cmd_frames)
    p = sub.add_parser("camera")
    p.add_argument("file")
    p.add_argument("--fps", type=float, required=True, help="camera frame rate")
    p.set_defaults(func=cmd_camera)
    p = sub.add_parser("presentmon")
    p.add_argument("file")
    p.set_defaults(func=cmd_presentmon)
    p = sub.add_parser("compare")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--kind", choices=["frames", "camera"], required=True)
    p.add_argument("--column", default="input_age_ns")
    p.add_argument("--fps", type=float, default=240.0)
    p.set_defaults(func=cmd_compare)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
