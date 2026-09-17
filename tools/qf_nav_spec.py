import json, sys
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import Qt
app = QApplication(sys.argv); app.setStyle("Fusion")
import qfluentwidgets as qf
from qfluentwidgets import NavigationInterface, FluentWindow, FluentIcon as FIF, NavigationPushButton
o = {}
fw = FluentWindow(); fw.resize(1100, 750)
nav = fw.navigationInterface
nav.addItem("r", FIF.HOME, "主页", lambda: None)
o["nav"] = {"interface_w": nav.width(), "panel_w": nav.panel.width(), "panel_h": nav.panel.height()}
it = nav.panel.layout().itemAt(1)
w = it.widget() if it else None
if w is not None:
    o["nav_item"] = {"class": type(w).__name__, "size": [w.width(), w.height()], "hint": [w.sizeHint().width(), w.sizeHint().height()],
                     "iconSize": [w.iconSize().width(), w.iconSize().height()] if hasattr(w, "iconSize") else None}
o["titlebar"] = {"h": fw.titleBar.height(), "w": fw.titleBar.width()}
o["stack"] = {"geom": [fw.stackedWidget.x(), fw.stackedWidget.y(), fw.stackedWidget.width(), fw.stackedWidget.height()]}
o["nav_geom"] = [nav.x(), nav.y(), nav.width(), nav.height()]
o["theme_color"] = qf.themeColor().name()
o["font"] = {"family": app.font().family(), "px": app.font().pixelSize()}
pb = qf.PrimaryPushButton("启动")
o["primary"] = {"color_bg": qf.themeColor().name(), "sizeHint": [pb.sizeHint().width(), pb.sizeHint().height()]}
print(json.dumps(o, ensure_ascii=False))
