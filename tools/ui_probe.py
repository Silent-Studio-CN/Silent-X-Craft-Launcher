# 像素级量测：把参考图变成可执行的规格数字
import sys
from collections import Counter
from PIL import Image

path, out_path = sys.argv[1], sys.argv[2]
im = Image.open(path).convert("RGB")
if im.size != (1100, 750):
    im = im.resize((1100, 750), Image.LANCZOS)
W, H = im.size
px = im.load()
lines = []
lines.append("IMAGE %dx%d  (%s)" % (W, H, path.split("\\")[-1]))

c = Counter(im.getdata())
tot = W * H
lines.append("TOP COLORS:")
for col, n in c.most_common(14):
    lines.append("  #%02x%02x%02x  %5.1f%%  %d px" % (col[0], col[1], col[2], 100.0*n/tot, n))

def segs(y, tol=8, minw=4):
    res = []; cur = px[0, y]; start = 0
    for x in range(1, W):
        p = px[x, y]
        if max(abs(p[i]-cur[i]) for i in range(3)) > tol:
            res.append((start, x-1, cur)); cur = p; start = x
    res.append((start, W-1, cur))
    return [s for s in res if s[1]-s[0]+1 >= minw]

lines.append("H-SCAN (row: segments x0-x1 #color):")
for y in [0, 3, 12, 24, 30, 47, 48, 55, 60, 100, 122, 170, 200, 250, 380, 520, 700, 735, 749]:
    ss = segs(y)
    txt = " ".join("%d-%d #%02x%02x%02x" % (a, b, cc[0], cc[1], cc[2]) for a, b, cc in ss[:10])
    lines.append("  y=%3d (%2d): %s" % (y, len(ss), txt))

def vsegs(x, tol=8, minh=4):
    res = []; cur = px[x, 0]; start = 0
    for y in range(1, H):
        p = px[x, y]
        if max(abs(p[i]-cur[i]) for i in range(3)) > tol:
            res.append((start, y-1, cur)); cur = p; start = y
    res.append((start, H-1, cur))
    return [s for s in res if s[1]-s[0]+1 >= minh]

lines.append("V-SCAN (col: segments y0-y1 #color):")
for x in [2, 20, 60, 150, 300, 316, 320, 330, 500, 900, 1080, 1097]:
    ss = vsegs(x)
    txt = " ".join("%d-%d #%02x%02x%02x" % (a, b, cc[0], cc[1], cc[2]) for a, b, cc in ss[:10])
    lines.append("  x=%4d (%2d): %s" % (x, len(ss), txt))

open(out_path, "w", encoding="utf-8").write("\n".join(lines))
print("WROTE", out_path, len(lines), "lines")
