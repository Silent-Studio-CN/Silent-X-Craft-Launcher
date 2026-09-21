#!/usr/bin/env python3
# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

"""selftest_measure_swipe.py - calibrate the ruler before trusting its numbers.

measure_swipe.py is the only instrument used for the "before fix / after fix"
displacement table, so it gets its own test: take a REAL device screenshot, shift it
by a known number of pixels, and check that the measurement recovers exactly that
number. Also checks the two degenerate cases (identical frames, flat/black frame),
because "0 px" and "cannot measure" must never be confused in the report.

Usage: python selftest_measure_swipe.py <base.png> [--outdir DIR]
"""

import argparse
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
RULER = os.path.join(HERE, "measure_swipe.py")


def shift_up(arr, dy):
    """after[y] = before[y + dy]  ->  content moved UP by dy (user scrolled down)."""
    out = arr.copy()
    if dy > 0:
        out[:-dy] = arr[dy:]
        out[-dy:] = arr[-1]
    elif dy < 0:
        out[-dy:] = arr[:dy]
        out[: -dy] = arr[0]
    return out


def run(pre, post, outdir, tag):
    jp = os.path.join(outdir, "selftest_%s.json" % tag)
    p = subprocess.run([sys.executable, RULER, pre, post, "--json", jp],
                       capture_output=True, text=True)
    got = None
    if os.path.exists(jp):
        with open(jp, encoding="utf-8") as fh:
            got = json.load(fh)
    return p.returncode, p.stdout.strip().splitlines(), got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("--outdir", default=None)
    args = ap.parse_args()
    outdir = args.outdir or os.path.dirname(os.path.abspath(args.base))
    os.makedirs(outdir, exist_ok=True)

    img = Image.open(args.base).convert("RGBA")
    arr = np.asarray(img).copy()
    h, w = arr.shape[:2]
    print("base image: %s  %dx%d" % (args.base, w, h))
    base_png = os.path.join(outdir, "selftest_base.png")
    img.save(base_png)

    cases = []
    for dy in (0, 11, 37, 137, -64, 260):
        after = Image.fromarray(shift_up(arr, dy))
        ap_png = os.path.join(outdir, "selftest_shift_%+d.png" % dy)
        after.save(ap_png)
        cases.append((dy, ap_png))

    ok = True
    report = []
    for dy, path in cases:
        rc, out, got = run(base_png, path, outdir, "%+d" % dy)
        measured = None if got is None else got.get("displacement_px")
        usable = None if got is None else got.get("usable")
        verdict = "OK" if (measured == dy) else "MISMATCH"
        if measured != dy:
            ok = False
        report.append({"injected_px": dy, "measured_px": measured, "usable": usable,
                       "verdict": verdict, "exit": rc})
        print("  injected %+4d px -> measured %-6s usable=%-5s %s"
              % (dy, measured, usable, verdict))

    # degenerate 1: identical frames -> must be a MEASURED 0, not "cannot measure"
    rc, out, got = run(base_png, base_png, outdir, "identical")
    zero_ok = got is not None and got.get("usable") and got.get("displacement_px") == 0
    print("  identical frames          -> %s (usable=%s, dy=%s)"
          % ("OK" if zero_ok else "PROBLEM",
             None if got is None else got.get("usable"),
             None if got is None else got.get("displacement_px")))
    if not zero_ok:
        ok = False

    # degenerate 2: flat frames -> must be reported as NOT measurable (exit 2)
    flat = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    flat_a = os.path.join(outdir, "selftest_flat_a.png")
    flat_b = os.path.join(outdir, "selftest_flat_b.png")
    flat.save(flat_a)
    flat.save(flat_b)
    rc, out, got = run(flat_a, flat_b, outdir, "flat")
    flat_ok = (rc == 2) and (got is not None) and (not got.get("usable"))
    print("  flat black frames         -> %s (exit=%d, usable=%s)"
          % ("OK" if flat_ok else "PROBLEM", rc,
             None if got is None else got.get("usable")))
    if not flat_ok:
        ok = False

    print("RULER CALIBRATION: %s" % ("PASS" if ok else "FAIL"))
    with open(os.path.join(outdir, "selftest_report.json"), "w", encoding="utf-8") as fh:
        json.dump({"base": args.base, "cases": report, "pass": ok}, fh, indent=2)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
