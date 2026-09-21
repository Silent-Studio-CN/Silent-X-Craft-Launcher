#!/usr/bin/env python3
# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

"""fit_anim_trace.py - what animation did the app ACTUALLY run?

Input: the SXCL_ANIM_TRACE=1/2 output ([sxcl-ui] <tag> t=<ms>ms <metric>=<v> ...),
i.e. per-frame samples taken from the live widget - on the desktop (offscreen) and on
the Android device through logcat, with the same sampling code.

For every candidate easing curve it grid-searches the duration D and the sampling
offset t0, solving start/end by least squares for that (D, t0). The winner is the
model with the smallest residual, which is how "150ms OutQuad" / "250ms OutQuad" stop
being a claim read out of the source and become a measured property of the running app.

Usage:
  python fit_anim_trace.py trace.txt --tag nav --metric width
  python fit_anim_trace.py trace.txt --tag popup --metric y --json out.json
"""

import argparse
import json
import math
import re
import sys

# easing curves, all f(0)=0 f(1)=1 (QEasingCurve semantics)
MODELS = {
    "Linear": lambda u: u,
    "OutQuad": lambda u: 1 - (1 - u) ** 2,
    "OutCubic": lambda u: 1 - (1 - u) ** 3,
    "OutQuart": lambda u: 1 - (1 - u) ** 4,
    "OutQuint": lambda u: 1 - (1 - u) ** 5,
    "OutSine": lambda u: math.sin(u * math.pi / 2),
    "InQuad": lambda u: u * u,
    "InCubic": lambda u: u ** 3,
    "InOutQuad": lambda u: 2 * u * u if u < 0.5 else 1 - 2 * (1 - u) ** 2,
}

LINE = re.compile(r"\[sxcl-ui\]\s+(?P<tag>\S+)\s+t=(?P<t>\d+)ms\s+"
                  r"(?P<metric>y|width)=(?P<v>-?\d+)")


def load(path, tag, metric):
    out = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            m = LINE.search(raw)
            if m and m.group("tag") == tag and m.group("metric") == metric:
                out.append((int(m.group("t")), int(m.group("v"))))
    return out


def fit_two_param(ts, vs, curve, d, t0, fixed=None):
    """least-squares start/end for value = A + B*curve(u), returns (rms, A, B).

    fixed=(start,end) pins the endpoints: the sampler starts a few ms AFTER the
    animation begins (QEvent::Show fires after start()), so with free endpoints the
    fit trades duration against a wrong start value. The endpoints are ground truth
    from the widget itself (popup: shown at y=462, settles at y=533; nav: 48 -> 322)."""
    n = len(ts)
    if n < 4 or d <= 0:
        return None
    if fixed is not None:
        a, b = fixed[0], fixed[1] - fixed[0]
        err = 0.0
        for t, v in zip(ts, vs):
            u = (t - t0) / d
            u = 0.0 if u < 0 else (1.0 if u > 1 else u)
            err += (v - (a + b * curve(u))) ** 2
        return (math.sqrt(err / n), a, b)
    s0 = s1 = s2 = b0 = b1 = 0.0
    for t, v in zip(ts, vs):
        u = (t - t0) / d
        u = 0.0 if u < 0 else (1.0 if u > 1 else u)
        f = curve(u)
        s0 += 1.0
        s1 += f
        s2 += f * f
        b0 += v
        b1 += v * f
    det = s0 * s2 - s1 * s1
    if abs(det) < 1e-9:
        return None
    a = (b0 * s2 - b1 * s1) / det
    b = (s0 * b1 - s1 * b0) / det
    err = 0.0
    for t, v in zip(ts, vs):
        u = (t - t0) / d
        u = 0.0 if u < 0 else (1.0 if u > 1 else u)
        err += (v - (a + b * curve(u))) ** 2
    return (math.sqrt(err / n), a, b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--tag", default="popup")
    ap.add_argument("--metric", default="y", choices=["y", "width"])
    ap.add_argument("--dmin", type=float, default=40.0)
    ap.add_argument("--dmax", type=float, default=600.0)
    ap.add_argument("--start", type=float, default=None,
                    help="ground-truth start value (pins the endpoints)")
    ap.add_argument("--end", type=float, default=None,
                    help="ground-truth end value (pins the endpoints)")
    ap.add_argument("--json", dest="json_path", default=None)
    args = ap.parse_args()
    fixed = (args.start, args.end) if args.start is not None and args.end is not None else None

    samples = load(args.trace, args.tag, args.metric)
    if len(samples) < 4:
        print("no samples for tag=%s metric=%s in %s (%d found)"
              % (args.tag, args.metric, args.trace, len(samples)))
        return 2

    ts = [t for t, _ in samples]
    vs = [v for _, v in samples]
    changed = [t1 for (t0, v0), (t1, v1) in zip(samples, samples[1:]) if v0 != v1]

    results = []
    for name, curve in MODELS.items():
        best = None
        d = args.dmin
        while d <= args.dmax:
            for t0 in [x * 0.5 for x in range(-60, 61)]:
                r = fit_two_param(ts, vs, curve, d, t0, fixed)
                if r and (best is None or r[0] < best[0]):
                    best = (r[0], d, t0, r[1], r[2])
            d += 1.0
        if best:
            results.append((best[0], name, best[1], best[2], best[3], best[4]))
    results.sort()

    rep = {
        "trace": args.trace,
        "tag": args.tag,
        "metric": args.metric,
        "samples": len(samples),
        "value_range": [min(vs), max(vs)],
        "first_change_ms": changed[0] if changed else None,
        "last_change_ms": changed[-1] if changed else None,
        "ranking": [
            {"model": n, "rms_px": round(r, 3), "duration_ms": round(d, 1),
             "t0_ms": round(t0, 1), "start": round(a, 1), "end": round(a + b, 1)}
            for r, n, d, t0, a, b in results[:4]
        ],
    }
    print("trace      : %s  (tag=%s metric=%s, %d samples)"
          % (args.trace, args.tag, args.metric, len(samples)))
    print("values     : %d .. %d" % (min(vs), max(vs)))
    print("motion     : first change at t=%sms, last change at t=%sms (sampling starts "
          "a few ms after QEvent::Show)" % (rep["first_change_ms"], rep["last_change_ms"]))
    print("best fits  (lower rms = better):")
    for row in rep["ranking"]:
        print("   %-10s rms=%7.3f px  duration=%6.1f ms  t0=%+5.1f ms  %s -> %s"
              % (row["model"], row["rms_px"], row["duration_ms"], row["t0_ms"],
                 row["start"], row["end"]))
    win = rep["ranking"][0]
    print("=> measured: %s, %.0f ms (source of truth: the widget's own geometry)"
          % (win["model"], win["duration_ms"]))

    if args.json_path:
        with open(args.json_path, "w", encoding="utf-8") as fh:
            json.dump(rep, fh, ensure_ascii=False, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
