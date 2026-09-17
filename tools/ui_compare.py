# 设计图 vs 实装图 逐像素比对(1:1 验收)
import sys
from collections import Counter
from PIL import Image

design_path, actual_path = sys.argv[1], sys.argv[2]
A = Image.open(design_path).convert("RGB")
B = Image.open(actual_path).convert("RGB")
print("design %s %s" % (A.size, design_path.split(chr(92))[-1]))
print("actual %s %s" % (B.size, actual_path.split(chr(92))[-1]))
if A.size != B.size:
    B = B.resize(A.size, Image.LANCZOS)
    print("actual resized ->", A.size)
W, H = A.size
pa, pb = A.load(), B.load()
diff = 0; tot = W * H; ssum = 0
rowdiff = [0] * H
for y in range(H):
    d = 0
    for x in range(W):
        a = pa[x, y]; b = pb[x, y]
        m = max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))
        ssum += m
        if m > 12:
            d += 1
    rowdiff[y] = d
    diff += d
print("DIFF>12: %.2f%%   mean|d| %.1f" % (100.0 * diff / tot, ssum / tot))
print("TOP COLORS  design -> actual share")
ca = Counter(A.getdata()); cb = Counter(B.getdata())
for col, n in ca.most_common(8):
    sa = 100.0 * n / tot
    sb = 100.0 * cb.get(col, 0) / tot
    print("  #%02x%02x%02x  %6.2f%% -> %6.2f%%   %s" % (col[0], col[1], col[2], sa, sb,
          "OK" if abs(sa - sb) < 2 else ("缺失" if sb < 0.05 else "偏差")))
print("WORST BANDS (y, diff%)")
bands = [(y, 100.0 * rowdiff[y] / W) for y in range(0, H, 15)]
bands.sort(key=lambda t: -t[1])
for y, p in bands[:10]:
    print("  y=%4d  %5.1f%%" % (y, p))
