import sys, os
from pathlib import Path
sys.path.insert(0, r"D:\SilentStudio\prog\Silent-X-Craft-Launcher")
os.chdir(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher")
from PySide6.QtWidgets import QApplication
app = QApplication([])
from src.app.icons import block_pixmap, block_icon, BLOCK_FILES, _asset_dir
print("ASSET_DIR", _asset_dir())
dst = Path(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C" + r"\build\ref\icons")
dst.mkdir(parents=True, exist_ok=True)
kinds = sorted(set(BLOCK_FILES) | {"vanilla"})
for kind in kinds:
    for size in (24, 16):
        pm = block_pixmap(kind, size)
        pm.save(str(dst / (kind + "_" + str(size) + ".png")))
        if size == 24:
            print("  %-12s %3dx%-3d dpr=%.2f" % (kind, pm.width(), pm.height(), pm.devicePixelRatio()))
print("DPR", app.primaryScreen().devicePixelRatio(), "FILES", len(list(dst.glob('*.png'))))
