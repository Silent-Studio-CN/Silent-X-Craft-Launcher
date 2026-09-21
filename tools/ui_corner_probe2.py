# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

import sys
from PIL import Image

def dump(path, tag, x0, y0, x1, y1, step=3):
    im = Image.open(path).convert("RGB"); px = im.load()
    colors = {}; grid = []
    for y in range(y0, y1, step):
        row = []
        for x in range(x0, x1, step):
            c = px[x, y]; k = "#%02x%02x%02x" % c
            if k not in colors:
                colors[k] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[len(colors)] if len(colors) < 26 else "?"
            row.append(colors[k])
        grid.append((y, row))
    print("== " + tag + " 逻辑(" + str(x0//1.5) + "," + str(y0//1.5) + ")-(" + str(x1//1.5) + "," + str(y1//1.5) + ")")
    for y, row in grid:
        print("%4d %s" % (y, "".join(row)))
    print("   图例: " + "  ".join("%s=%s" % (v, k) for k, v in colors.items()))

# 内容框左上角:逻辑 40..72 -> 物理 60..108
dump(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref\baseline\py_home.png", "设计", 60, 60, 108, 108)
dump(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref\c_home.png", "实装", 60, 60, 108, 108)
