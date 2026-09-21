# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

import os, sys, traceback
from pathlib import Path

PY = Path(os.environ.get("SXCL_PY_DIR", r"D:\SilentStudio\prog\Silent-X-Craft-Launcher"))
OUT = Path(sys.argv[1] if len(sys.argv) > 1 else r"D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref")
os.chdir(PY)
sys.path.insert(0, str(PY))
OUT.mkdir(parents=True, exist_ok=True)

from PySide6.QtCore import Qt, QEventLoop, QTimer
from PySide6.QtWidgets import QApplication

QApplication.setHighDpiScaleFactorRoundingPolicy(Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)
app = QApplication(sys.argv)
app.setStyle("Fusion")

from src.app.common.launcher_config import load_config, cfg
from src.core.lang import init_language
load_config()
try:
    init_language(cfg.language.value.value.lower())
except Exception as e:
    print("LANG_FAIL", e)

# Python main.py:150 会调 apply_theme()(库主题 + 应用调色板 + global_qss)。
# 以前抓图漏了这一步 → 参考图没有 global_qss,而 C 版一直装着等价的应用级 QSS,
# 两边口径不一致(临时页代理实测:download_config 的输入框因此差 9px)。
from src.app.theme import apply_theme
apply_theme()

from src.app.main_window import MainWindow
from qfluentwidgets import isDarkTheme

def pump(ms):
    loop = QEventLoop(); QTimer.singleShot(ms, loop.quit); loop.exec()

w = MainWindow()
w.resize(1100, 750)
# 1:1 基准必须与壁纸无关:Mica/亚克力透出的是系统背景,渲染结果随桌面变化。
# 关掉它们,拿到的才是"纯令牌 + qf 样式"的确定性画面。
for name, val in (("setMicaEffectEnabled", False), ("setAcrylicEffectEnabled", False)):
    fn = getattr(w, name, None)
    if fn is not None:
        try:
            fn(val)
            print("已关闭", name)
        except Exception as exc:
            print("关闭失败", name, exc)
# SXCL 的 global_qss 把 StackedWidget 设成 transparent,而 qf 开 Mica 时窗口背景由系统画 ——
# 于是内容区会透出**桌面**(截到壁纸色,卡片也跟着被桌面染色)。
# 1:1 基准必须与桌面无关。注意:**不能**给 stackedWidget 设样式表,
# 那会破坏 qf 的页面切换(实测 6 张图全变成同一页)。正确做法是让顶层窗口自己画底色。
from src.app.theme import token as _token
w.show()
pump(1500)

pages = [("home", w.home_page), ("versions", w.versions_page), ("tasks", w.tasks_page),
         ("keymap", w.keymap_page), ("multiplayer", w.multiplayer_page), ("settings", w.settings_page)]
def stable_bytes(widget, interval=400, max_rounds=12):
    """等到连续两帧完全一致再返回 —— 页面里的版本列表是异步填充的,
    固定等待 1.5s 可能抓到"还没排完序"的中间态(实测导致参考图与代码不一致)。"""
    prev = None
    for i in range(max_rounds):
        pump(interval)
        img = widget.grab().toImage()
        data = bytes(img.bits())
        if prev is not None and data == prev:
            return img, i + 1
        prev = data
    return widget.grab().toImage(), max_rounds

# ── 临时页分支(SXCL_GRAB_TEMP=1)─────────────────────────────────────────────
# 三个"临时页"(下载配置 / 下载进度 / 启动进度)不在常驻导航里,只能经主窗口的
# 会话机制进入(switch_to_download_config / switch_to_download_progress / switch_to_launch)。
# 后两个页面一建好就会起工作线程去做网络/进程动作,C 版没有这些服务(核心库未移植),
# 页面停在**初始态**;为了让参考图确定可复现,这里把三个 worker 的 start() 换成空操作
# (页面构造、布局、控件全部照原样,只是不进入后续异步状态)。
TEMP = os.environ.get("SXCL_GRAB_TEMP")
if TEMP:
    from src.services.minecraft.manifest import GameVersion
    import src.app.pages.download_config_page as _dcp
    import src.app.pages.download_progress_page as _dpp
    import src.app.pages.launch_page as _lp
    # SXCL_GRAB_APPTHEME=1:补上 main.py:150 的 apply_theme()(全局 QSS)。
    # 默认**不**装 —— 保持与既有 6 张参考图完全相同的抓图条件;
    # 装上是"与真实启动器一致"的那一套,用来核对 C 版(它总是装了应用级 QSS)。
    if os.environ.get("SXCL_GRAB_APPTHEME") == "1":
        from src.app.theme import apply_theme
        apply_theme()
        print("临时页抓图: 已 apply_theme()(全局 QSS 与真实启动一致)")
    _dcp.LoaderFetchWorker.start = lambda self: None
    _dpp.InstallWorker.start = lambda self: None
    _lp.LaunchWorker.start = lambda self: None
    print("临时页抓图: 已冻结三个后台 worker(页面停在初始态)")

    ver = GameVersion(id="1.21.11", version_type="release", url="", release_time="2026-01-01T00:00:00+00:00")
    temp_pages = [
        ("download_config", lambda: w.switch_to_download_config(ver)),
        ("download_progress", lambda: w.switch_to_download_progress(ver, "1.21.11")),
        ("launch", lambda: w.switch_to_launch(ver)),
    ]
    for name, enter in temp_pages:
        try:
            enter()
            page = w._active_temp_page
            if page is None:
                print("FAIL", name, "没有活动临时页")
                continue
            if os.environ.get("SXCL_GRAB_NOQSS") != "1":
                page.setStyleSheet("QWidget { background: %s; }" % _token("bg"))
            ok = False
            for _ in range(20):
                pump(150)
                if w.stackedWidget.currentWidget() is page:
                    ok = True
                    break
            if not ok:
                cur = w.stackedWidget.currentWidget()
                print("FAIL", name, "栈没切过去,当前是",
                      type(cur).__name__ if cur is not None else None)
                continue
            img, rounds = stable_bytes(w)
            if w.stackedWidget.currentWidget() is not page:
                print("WARN", name, "抓图后栈又变了")
            suffix = ""
            if os.environ.get("SXCL_GRAB_NOQSS") == "1":
                suffix = "_noqss"
            elif os.environ.get("SXCL_GRAB_APPTHEME") == "1":
                suffix = "_apptheme"
            p = OUT / ("py_" + name + suffix + ".png")
            img.save(str(p))
            print("SAVED", p.name, img.width(), "x", img.height(), p.stat().st_size, "稳定于第", rounds, "轮")
        except Exception:
            print("FAIL", name, traceback.format_exc().splitlines()[-1])
    print("DARK", isDarkTheme(), "SIZE", w.width(), "x", w.height())
    sys.exit(0)

for name, page in pages:
    try:
        # 关键:qf 的页面切换挂在导航项的 onClick 回调上
        # (addSubInterface(..., onClick=lambda: self.switchTo(interface))),
        # setCurrentItem() 只改选中态**不会触发切换** —— 踩过这个坑,6 张图全成了同一页。
        w.switchTo(page)
        w.navigationInterface.setCurrentItem(page.objectName())
        # 页面本身设为不透明:否则 SXCL 的 transparent 内容栈会透出桌面(参考图会随壁纸变)。
        page.setStyleSheet("QWidget { background: %s; }" % _token("bg"))
        ok = False
        for _ in range(20):
            pump(150)
            if w.stackedWidget.currentWidget() is page:
                ok = True
                break
        if not ok:
            cur = w.stackedWidget.currentWidget()
            print("FAIL", name, "栈没切过去,当前是",
                  type(cur).__name__ if cur is not None else None,
                  getattr(cur, "objectName", lambda: "")())
            continue
        img, rounds = stable_bytes(w)
        # 二次确认:抓完还在这一页(切换动画/懒加载可能把它拉回去)
        if w.stackedWidget.currentWidget() is not page:
            print("WARN", name, "抓图后栈又变了")
        p = OUT / ("py_" + name + ".png")
        img.save(str(p))
        print("SAVED", p.name, img.width(), "x", img.height(), p.stat().st_size, "稳定于第", rounds, "轮")
    except Exception:
        print("FAIL", name, traceback.format_exc().splitlines()[-1])
print("DARK", isDarkTheme(), "SIZE", w.width(), "x", w.height())
