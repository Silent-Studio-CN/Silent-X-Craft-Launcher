# 隔离实验:裸 qf 窗口 + 卡片,测出 qf 自己渲染的颜色(排除 SXCL 的干扰)
import sys
from PySide6.QtCore import QEventLoop, QTimer
from PySide6.QtWidgets import QApplication, QWidget, QVBoxLayout
app = QApplication(sys.argv); app.setStyle("Fusion")
import qfluentwidgets as qf
from qfluentwidgets import FluentWindow, CardWidget, SimpleCardWidget, BodyLabel, TitleLabel, setTheme, Theme, setThemeColor
from PySide6.QtGui import QColor
setTheme(Theme.DARK); setThemeColor(QColor("#ff75df"))

def pump(ms):
    loop = QEventLoop(); QTimer.singleShot(ms, loop.quit); loop.exec()

w = FluentWindow(); w.resize(600, 400)
for name, val in (("setMicaEffectEnabled", False), ("setAcrylicEffectEnabled", False)):
    fn = getattr(w, name, None)
    if fn:
        try: fn(val)
        except Exception as e: print("off fail", name, e)
page = QWidget(); page.setObjectName("probePage")
lay = QVBoxLayout(page); lay.setContentsMargins(24, 24, 24, 24); lay.setSpacing(8)
card = CardWidget(page); card.setFixedHeight(56)
cl = QVBoxLayout(card); cl.setContentsMargins(20, 0, 16, 0); cl.addWidget(BodyLabel("1.21.11", card))
sc = SimpleCardWidget(page); sc.setFixedHeight(56)
lay.addWidget(TitleLabel("主页", page)); lay.addWidget(card); lay.addWidget(sc); lay.addStretch(1)
w.addSubInterface(page, qf.FluentIcon.HOME, "主页")
w.show(); pump(1200)
pm = w.grab()
img = pm.toImage()
print("grab", pm.width(), pm.height(), "dpr", pm.devicePixelRatio())
print("isDark", qf.isDarkTheme(), "accent", qf.themeColor().name())
def probe(nx, ny, tag):
    x, y = int(nx * pm.width()), int(ny * pm.height())
    c = img.pixelColor(x, y)
    print("  %-22s (%4d,%4d) rgba(%3d,%3d,%3d,%3d)" % (tag, x, y, c.red(), c.green(), c.blue(), c.alpha()))
probe(0.5, 0.02, "标题栏")
probe(0.03, 0.5, "侧边栏")
probe(0.5, 0.45, "内容(卡片外)")
probe(0.5, 0.20, "卡片 CardWidget")
probe(0.5, 0.38, "卡片 SimpleCardWidget")
card.setStyleSheet(qf.styleSheetManager.source(card) or "")
print("card styleSheet:", (card.styleSheet() or "")[:80])
