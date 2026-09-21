# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

from PIL import Image
for tag, path in (("设计", r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref\py_home.png"), ("实装", r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref\c_home.png")):
    im = Image.open(path)
    print(tag, im.mode, im.size)
    rgba = im.convert("RGBA")
    px = rgba.load()
    pts = {"标题栏(700,20)": (700, 20), "侧边栏(30,500)": (30, 500), "内容(700,400)": (700, 400),
           "底部(700,1100)": (700, 1100), "导航项(40,180)": (40, 180)}
    if im.mode != "RGBA":
        print("   无 alpha 通道(完全不透明)")
    for name, (x, y) in pts.items():
        r, g, b, a = px[x, y]
        print("   %-16s rgba(%3d,%3d,%3d,%3d)" % (name, r, g, b, a))
    if im.mode == "RGBA":
        alphas = im.getchannel("A")
        print("   alpha=0 像素占比 %.1f%%,  alpha=255 占比 %.1f%%" % (
            100.0 * sum(1 for v in alphas.getdata() if v == 0) / (im.width * im.height),
            100.0 * sum(1 for v in alphas.getdata() if v == 255) / (im.width * im.height)))
