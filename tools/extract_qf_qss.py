import sys
from pathlib import Path
import qfluentwidgets  # 触发 _rc.resource 注册
from PySide6.QtCore import QDir, QFile, QIODevice

dst = Path(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C") / "assets" / "theme" / "qf_exact"
total = 0
for theme in ("dark", "light"):
    d = QDir(":/qfluentwidgets/qss/" + theme)
    if not d.exists():
        print("MISSING", theme); continue
    for e in d.entryInfoList(QDir.Filter.Files, QDir.SortFlag.Name):
        name = e.fileName()
        f = QFile(e.absoluteFilePath())
        if not f.open(QIODevice.ReadOnly):
            print("OPENFAIL", theme, name); continue
        data = bytes(f.readAll().data())
        f.close()
        p = dst / theme / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        total += 1
print("EXTRACTED", total, "qss ->", dst)
