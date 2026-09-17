# 文本色块图:把两张图变成可对比的字母网格(我看不到图,只能这样"看")
import sys
from collections import Counter
from PIL import Image

design, actual = sys.argv[1], sys.argv[2]
CELL = int(sys.argv[3]) if len(sys.argv) > 3 else 75
A = Image.open(design).convert("RGB"); B = Image.open(actual).convert("RGB")
if A.size != B.size:
    B = B.resize(A.size, Image.LANCZOS)
W, H = A.size
cols, rows = W // CELL, H // CELL

def cells(img):
    px = img.load(); out = []
    for r in range(rows):
        row = []
        for c in range(cols):
            cnt = Counter()
            for y in range(r * CELL, (r + 1) * CELL, 3):
                for x in range(c * CELL, (c + 1) * CELL, 3):
                    cnt[px[x, y]] += 1
            row.append(cnt.most_common(1)[0][0])
        out.append(row)
    return out

ca, cb = cells(A), cells(B)
palette = Counter()
for row in ca + cb:
    for col in row:
        palette[col] += 1
letters = "ABCDEFGHIJKLMN"
legend = {}
for i, (col, n) in enumerate(palette.most_common(len(letters))):
    legend[col] = letters[i]
def ch(col):
    return legend.get(col, "?")
print("图例: " + "  ".join("%s=#%02x%02x%02x" % (ch(col), col[0], col[1], col[2]) for col, _n in palette.most_common(len(letters))))
print("格子 %dpx (逻辑 %.0fpx), 网格 %dx%d" % (CELL, CELL / 1.5, cols, rows))
print("     设计" + " " * (cols - 4) + "| 实装")
for r in range(rows):
    print("%4d %s | %s" % (r * CELL, "".join(ch(c) for c in ca[r]), "".join(ch(c) for c in cb[r])))
