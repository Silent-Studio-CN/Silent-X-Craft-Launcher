# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

import sys
from PIL import Image

def dump(path, tag, box=(0, 0, 39, 39), step=3):
    im = Image.open(path).convert("RGB")
    px = im.load()
    W, H = im.size
    colors = {}
    grid = []
    for y in range(box[1], box[3], step):
        row = []
        for x in range(box[0], box[2], step):
            c = px[x, y]
            key = "#%02x%02x%02x" % c
            if key not in colors:
                colors[key] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[len(colors)] if len(colors) < 26 else "?"
            row.append(colors[key])
        grid.append(row)
    print("== " + tag + "  " + str(im.size))
    print("   " + "".join("%X" % (i % 16) for i in range(len(grid[0]))))
    for i, row in enumerate(grid):
        print("%3d %s" % (box[1] + i * step, "".join(row)))
    print("   图例: " + "  ".join("%s=%s" % (v, k) for k, v in colors.items()))

dump(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref\baseline\py_home.png", "设计(Python)")
dump(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref\c_home.png", "实装(C)")
