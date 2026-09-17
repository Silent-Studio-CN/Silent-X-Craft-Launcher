from PIL import Image

def dump(path, tag):
    im = Image.open(path).convert("RGB"); px = im.load()
    x0 = y0 = 69; x1 = y1 = 102   # 物理 69..102 = 逻辑 46..68
    print("== " + tag)
    print("      " + "".join("%d" % ((x // 1.5) % 10) for x in range(x0, x1)))
    for y in range(y0, y1):
        row = []
        for x in range(x0, x1):
            c = px[x, y]
            v = (c[0] + c[1] + c[2]) // 3
            row.append("#" if v < 24 else ("+" if v < 29 else ("." if v < 33 else "o")))
        print("%4d  %s" % (y, "".join(row)))
    print("  图例: # = <24(更深)  + = 24-28  . = 29-32  o = >=33")

dump(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref\baseline\py_home.png", "设计(Python)  圆角处")
dump(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref\c_home.png", "实装(C)")
