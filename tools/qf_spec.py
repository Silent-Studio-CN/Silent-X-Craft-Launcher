# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

import json, sys
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import Qt
app = QApplication(sys.argv)
app.setStyle("Fusion")
import qfluentwidgets as qf
from qfluentwidgets import (CaptionLabel, BodyLabel, StrongBodyLabel, SubtitleLabel, TitleLabel,
                            LargeTitleLabel, DisplayLabel, PrimaryPushButton, PushButton,
                            TransparentPushButton, CardWidget, SimpleCardWidget, HeaderCardWidget,
                            NavigationInterface, FluentWindow, LineEdit, ComboBox)
out = {}
out["theme_color"] = qf.themeColor().name()
out["labels"] = {}
for c in (CaptionLabel, BodyLabel, StrongBodyLabel, SubtitleLabel, TitleLabel, LargeTitleLabel, DisplayLabel):
    w = c("Silent X Craft 主页")
    f = w.font()
    out["labels"][c.__name__] = {"family": f.family(), "px": f.pixelSize(), "weight": int(f.weight()),
                                 "hint": [w.sizeHint().width(), w.sizeHint().height()]}
out["buttons"] = {}
for name, b in (("PrimaryPushButton", PrimaryPushButton("启动游戏")), ("PushButton", PushButton("启动游戏")),
                ("TransparentPushButton", TransparentPushButton("启动游戏"))):
    b.adjustSize()
    out["buttons"][name] = {"hint": [b.sizeHint().width(), b.sizeHint().height()],
                            "minH": b.minimumHeight(), "font_px": b.font().pixelSize()}
out["cards"] = {}
for name, w in (("CardWidget", CardWidget()), ("SimpleCardWidget", SimpleCardWidget()), ("HeaderCardWidget", HeaderCardWidget())):
    w.resize(400, 100)
    out["cards"][name] = {"radius": getattr(w, "borderRadius", None)}
nav = NavigationInterface(None, True)
out["nav"] = {"width_expanded": nav.width(), "panel_w": nav.panel.width(), "panel_h_policy": str(nav.panel.sizeHint())}
fw = FluentWindow()
out["window"] = {"titleBar_h": fw.titleBar.height(), "nav_w": fw.navigationInterface.width(),
                 "nav_panel_w": fw.navigationInterface.panel.width(), "size": [fw.width(), fw.height()]}
out["nav_item"] = {}
try:
    it = qf.NavigationWidget(qf.FluentIcon.HOME, "主页", True)
    out["nav_item"] = {"hint": [it.sizeHint().width(), it.sizeHint().height()], "minH": it.minimumHeight()}
except Exception as e:
    out["nav_item"] = {"err": str(e)[:60]}
print(json.dumps(out, ensure_ascii=False, indent=1))
