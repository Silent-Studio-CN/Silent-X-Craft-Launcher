import sys
from pathlib import Path
import qfluentwidgets
from PySide6.QtCore import QDir, QFile, QIODevice

dst = Path(r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C") / "assets" / "theme" / "qf_exact"
root = QDir(":/qfluentwidgets/images")
def walk(d, out):
    for e in d.entryInfoList(QDir.Filter.AllEntries | QDir.Filter.NoDotAndDotDot, QDir.SortFlag.Name):
        if e.isDir():
            walk(QDir(e.absoluteFilePath()), out)
        else:
            out.append(e.absoluteFilePath())
    return out
files = walk(root, [])
print("images total", len(files))
n = 0; icons = 0
for f in files:
    rel = f.lstrip(":/").replace("qfluentwidgets/", "", 1)
    p = dst / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    qf = QFile(f)
    if qf.open(QIODevice.ReadOnly):
        data = bytes(qf.readAll().data())
        p.write_bytes(data); n += 1
        if rel.startswith("images/icons/"):
            icons += 1
        qf.close()
print("EXTRACTED", n, "files,", icons, "icons ->", dst)
sizes = [f for f in files if "/icons/" in f]
print("sample:", [s.split("/")[-1] for s in sizes[:8]])
