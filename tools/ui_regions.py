# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

import sys
from collections import Counter
from PIL import Image

src, out_path = sys.argv[1], sys.argv[2]
im = Image.open(src).convert("RGB")
if im.size != (1100, 750):
    im = im.resize((1100, 750), Image.LANCZOS)
W, H = im.size
px = im.load()
cnt = Counter()
for y in range(H):
    for x in range(W):
        cnt[px[x, y]] += 1

def regions(col, minarea=350):
    active = []; rects = []
    for y in range(H):
        runs = []; x = 0
        while x < W:
            if px[x, y] == col:
                x0 = x
                while x < W and px[x, y] == col:
                    x += 1
                runs.append((x0, x - 1))
            else:
                x += 1
        newactive = []; used = [False] * len(active)
        for (x0, x1) in runs:
            hit = False
            for i, a in enumerate(active):
                if not used[i] and a["y1"] == y - 1 and not (x1 < a["x0"] - 1 or x0 > a["x1"] + 1):
                    a["y1"] = y; a["x0"] = min(a["x0"], x0); a["x1"] = max(a["x1"], x1)
                    used[i] = True; hit = True; newactive.append(a); break
            if not hit:
                newactive.append({"x0": x0, "x1": x1, "y0": y, "y1": y})
        for i, a in enumerate(active):
            if not used[i]:
                rects.append(a)
        active = newactive
    rects.extend(active)
    res = []
    for r in rects:
        w = r["x1"] - r["x0"] + 1; h = r["y1"] - r["y0"] + 1
        if w * h >= minarea:
            res.append((w * h, r["x0"], r["y0"], w, h))
    return sorted(res, reverse=True)

lines = ["IMAGE %s  (%dx%d)" % (src.split("\\")[-1], W, H), "TOP COLORS:"]
for col, n in cnt.most_common(12):
    lines.append("  #%02x%02x%02x  %5.2f%%  %d px" % (col[0], col[1], col[2], 100.0 * n / (W * H), n))
lines.append("BLOCKS (color: area x,y,w,h):")
for col, n in cnt.most_common(12):
    rs = regions(col)
    if not rs:
        continue
    lines.append("  #%02x%02x%02x:" % (col[0], col[1], col[2]))
    for a, x, y, w, h in rs[:8]:
        lines.append("     %7d  @ %4d,%3d  %4dx%-4d" % (a, x, y, w, h))
open(out_path, "w", encoding="utf-8").write("\n".join(lines))
print("WROTE", out_path)
