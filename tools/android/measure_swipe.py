#!/usr/bin/env python3
"""measure_swipe.py - vertical displacement between two device screenshots.

One ruler for "before / after" swipe evidence: give it the two PNGs taken with
`adb shell screencap` around an `adb shell input swipe`, and it reports how many
pixels the on-screen content moved, plus how trustworthy that number is.

Why not uiautomator: Qt Widgets on Android renders the whole UI into a single
SurfaceView, so the native view tree reports scrollable=0 and there is no
per-widget node to read a scroll offset from. Pixels are the only honest
source, so the measurement is done three independent ways and the report shows
all three (they must agree, otherwise the number is not trustworthy).

  A) profile-zncc : zero-mean normalized cross-correlation of the row
                    horizontal-edge profile (content translation invariant to
                    brightness/colour).
  B) row-mae      : normalized mean abs difference of raw rows per candidate
                    shift; the minimum is the best alignment.
  C) strip-match  : several 32-row strips from `before` are searched in `after`
                    (median of the per-strip optima) - independent of A/B.

Sign convention: dy > 0 means the content moved UP the screen, i.e. the user
scrolled DOWN (finger swiped up). after[y] == before[y + dy].

Usage:
  python measure_swipe.py before.png after.png [--region x0,y0,x1,y1]
                            [--max-shift 400] [--json out.json] [--quiet]

Exit code 0 = a displacement was measured above the confidence floor.
Exit code 2 = no usable signal (flat image / no match) - do NOT read it as 0 px.
"""

import argparse
import json
import sys

import numpy as np
from PIL import Image


def load_gray(path):
    img = Image.open(path).convert("L")
    return np.asarray(img, dtype=np.float32)


def row_edge_profile(a):
    """Per-row horizontal-edge energy: peaks where the content has edges."""
    if a.shape[0] < 3:
        return np.zeros(a.shape[0], dtype=np.float32)
    d = np.abs(np.diff(a, axis=0))
    return d.mean(axis=1)


def zncc_shift(pb, pa, max_shift):
    """Best dy by zero-mean normalized cross correlation of two 1-D profiles."""
    n = len(pb)
    if n <= 2 * max_shift + 8:
        max_shift = max(1, (n - 8) // 2)
    best = (-2.0, 0)
    scores = []
    for dy in range(-max_shift, max_shift + 1):
        # after[j] == before[j + dy]  (dy > 0 => content moved UP the screen)
        if dy >= 0:
            x, y = pb[dy:n], pa[0 : n - dy]
        else:
            x, y = pb[0 : n + dy], pa[-dy:n]
        if len(x) < 16:
            continue
        x = x - x.mean()
        y = y - y.mean()
        denom = float(np.sqrt((x * x).sum() * (y * y).sum()))
        s = float((x * y).sum() / denom) if denom > 1e-9 else 0.0
        scores.append((s, dy))
        if s > best[0]:
            best = (s, dy)
    return best[0], best[1], scores


def row_mae_shift(a, b, max_shift):
    """Best dy by normalized mean abs difference of raw rows."""
    n = a.shape[0]
    best = (None, 0)
    for dy in range(-max_shift, max_shift + 1):
        # same convention as zncc_shift: after[j] == before[j + dy]
        if dy >= 0:
            x, y = a[dy:n], b[0 : n - dy]
        else:
            x, y = a[0 : n + dy], b[-dy:n]
        if x.shape[0] < 16:
            continue
        m = float(np.abs(x - y).mean())
        if best[0] is None or m < best[0]:
            best = (m, dy)
    return best[0], best[1]


def strip_match(a, b, max_shift, strip=32, count=7):
    """Median dy over several strips of a matched inside b (vertical search)."""
    n = a.shape[0]
    if n < strip + 2 * max_shift + 8:
        return None, []
    dys = []
    for i in range(count):
        top = int((n - strip) * (i + 0.5) / count)
        patch = a[top : top + strip, :]
        if float(patch.std()) < 1.0:  # flat strip carries no information
            continue
        best = (None, 0)
        lo = max(0, top - max_shift)
        hi = min(n - strip, top + max_shift)
        for y in range(lo, hi + 1):
            m = float(np.abs(b[y : y + strip, :] - patch).mean())
            if best[0] is None or m < best[0]:
                best = (m, top - y)
        if best[0] is not None:
            dys.append(best[1])
    if not dys:
        return None, []
    return float(np.median(dys)), dys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--region", default=None,
                    help="x0,y0,x1,y1 crop (default: whole image)")
    ap.add_argument("--max-shift", type=int, default=400)
    ap.add_argument("--min-score", type=float, default=0.55,
                    help="zncc floor for accepting the profile result")
    ap.add_argument("--json", dest="json_path", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    a_full = load_gray(args.before)
    b_full = load_gray(args.after)
    if a_full.shape != b_full.shape:
        print("FAIL: image sizes differ: %s vs %s" % (a_full.shape, b_full.shape))
        return 2

    if args.region:
        x0, y0, x1, y1 = (int(v) for v in args.region.split(","))
        a, b = a_full[y0:y1, x0:x1], b_full[y0:y1, x0:x1]
        region = (x0, y0, x1, y1)
    else:
        a, b = a_full, b_full
        region = (0, 0, a_full.shape[1], a_full.shape[0])

    pb, pa = row_edge_profile(a), row_edge_profile(b)
    signal = float(min(pb.std(), pa.std()))
    score, dy_prof, scores = zncc_shift(pb, pa, args.max_shift)
    mae, dy_mae = row_mae_shift(a, b, args.max_shift)
    dy_strip, strips = strip_match(a, b, args.max_shift)

    mae0 = float(np.abs(a - b).mean())
    # peak sharpness: best score minus the best score outside +-3 px of the peak
    far = [s for s, d in scores if abs(d - dy_prof) > 3]
    sharp = score - (max(far) if far else -1.0)

    # Accept when the aligned frames really are more similar than the unaligned ones.
    # An absolute "peak sharpness" gate was tried first and threw away REAL swipes: a UI
    # with a repeating row pitch (instance cards, keymap rows) has a strong second
    # correlation peak one row-pitch away. The MAE ratio asks the question that matters.
    aligned_better = (mae is not None and (mae <= max(1.0, 0.5 * mae0) or mae0 <= 0.5))
    # signal floor only rules out a truly flat frame (a solid colour scores exactly 0);
    # a mostly-flat screenshot with real content still scores >0.5 and must stay usable.
    usable = signal >= 0.2 and score >= args.min_score and aligned_better
    votes = [dy_prof]
    if dy_strip is not None:
        votes.append(int(round(dy_strip)))
    agree = len(set(votes)) == 1

    rep = {
        "before": args.before,
        "after": args.after,
        "region": list(region),
        "region_size": [region[2] - region[0], region[3] - region[1]],
        "profile_std": signal,
        "profile_zncc": {"dy": int(dy_prof), "score": round(score, 4),
                         "sharpness": round(sharp, 4)},
        "row_mae": {"dy": int(dy_mae), "mae": round(float(mae), 3),
                    "mae_at_zero": round(mae0, 3)},
        "strip_match": {"dy": None if dy_strip is None else int(round(dy_strip)),
                        "per_strip": strips},
        "usable": bool(usable),
        "methods_agree": bool(agree),
        "displacement_px": int(dy_prof) if usable else None,
        "direction": None if not usable else
                     ("content moved UP (finger swiped up = scrolled down)" if dy_prof > 0
                      else "content moved DOWN (finger swiped down = scrolled up)"
                      if dy_prof < 0 else "no movement"),
    }

    if not args.quiet:
        print("before   : %s" % args.before)
        print("after    : %s" % args.after)
        print("region   : %s  (%dx%d)" % (tuple(region), region[2] - region[0],
                                          region[3] - region[1]))
        print("A profile: dy=%+d  zncc=%.4f sharpness=%.4f  (profile std %.2f)"
              % (dy_prof, score, sharp, signal))
        print("B row-mae: dy=%+d  mae=%.3f   (mae at dy=0: %.3f)"
              % (dy_mae, mae, mae0))
        print("C strips : dy=%s  per-strip=%s"
              % ("n/a" if dy_strip is None else "%+d" % int(round(dy_strip)), strips))
        if usable:
            print("=> displacement = %+d px  [%s]" % (dy_prof, rep["direction"]))
        else:
            print("=> NOT MEASURABLE (flat content or no match): "
                  "signal=%.2f score=%.4f sharpness=%.4f" % (signal, score, sharp))
        if not agree:
            print("!! methods disagree (%s) - treat the number as unreliable" % votes)

    if args.json_path:
        with open(args.json_path, "w", encoding="utf-8") as fh:
            json.dump(rep, fh, ensure_ascii=False, indent=2)

    return 0 if usable else 2


if __name__ == "__main__":
    sys.exit(main())
