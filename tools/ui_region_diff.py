# 分区比对:把 1:1 的差距定位到具体区域
import sys
from collections import Counter
from PIL import Image

design, actual = sys.argv[1], sys.argv[2]
regions = {
    "侧边栏": (0, 72, 72, 1125),
    "标题栏": (0, 0, 1650, 72),
    "内容区": (72, 72, 1650, 1125),
}
A = Image.open(design).convert("RGB"); B = Image.open(actual).convert("RGB")
if A.size != B.size:
    B = B.resize(A.size, Image.LANCZOS)
for name, box in regions.items():
    a = A.crop(box); b = B.crop(box)
    pa, pb = a.load(), b.load()
    W, H = a.size
    diff = 0; ssum = 0
    for y in range(H):
        for x in range(W):
            p = pa[x, y]; q = pb[x, y]
            m = max(abs(p[0]-q[0]), abs(p[1]-q[1]), abs(p[2]-q[2]))
            ssum += m
            if m > 12: diff += 1
    tot = W * H
    print("%s %s  差异 %.1f%%  平均|d| %.1f" % (name, a.size, 100.0*diff/tot, ssum/tot))
    ca = Counter(a.getdata()); cb = Counter(b.getdata())
    for col, n in ca.most_common(3):
        print("   设计 #%02x%02x%02x %5.1f%% | 实装 %5.1f%%" % (col[0], col[1], col[2],
              100.0*n/tot, 100.0*cb.get(col, 0)/tot))
