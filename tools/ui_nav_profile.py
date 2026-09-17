# 侧边栏图标墨迹剖面:每一行导航项里"非底色像素"的数量与包围盒
import sys
from PIL import Image

def profile(path, tag):
    im = Image.open(path).convert("RGB")
    px = im.load()
    W, H = im.size
    BG = (32, 32, 32)
    print("== " + tag + " (" + str(W) + "x" + str(H) + ")")
    # 侧边栏 x 0..72(逻辑 0..48);按 36 逻辑像素=54 物理像素一行
    row = 54
    for i in range(14):
        y0 = 48 * 1 + i * row  # 从标题栏下方开始
        y1 = y0 + row
        if y1 > H:
            break
        cnt = 0; minx = 10**9; maxx = -1; miny = 10**9; maxy = -1; colors = {}
        for y in range(y0, y1):
            for x in range(0, 72):
                c = px[x, y]
                if max(abs(c[0]-BG[0]), abs(c[1]-BG[1]), abs(c[2]-BG[2])) > 14:
                    cnt += 1
                    minx = min(minx, x); maxx = max(maxx, x)
                    miny = min(miny, y); maxy = max(maxy, y)
                    colors[c] = colors.get(c, 0) + 1
        top = sorted(colors.items(), key=lambda t: -t[1])[:2]
        box = "无" if cnt == 0 else "%d,%d %dx%d" % (minx, miny, maxx-minx+1, maxy-miny+1)
        print("  行%-2d y%4d-%4d  墨迹%5d  包围盒 %-14s %s" % (
            i, y0, y1, cnt, box, " ".join("#%02x%02x%02x" % c for c, _ in top)))

profile(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref\py_home.png", "设计")
profile(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref\c_home.png", "实装")
