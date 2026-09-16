# 版权所有 © Silent X Craft Launcher Dev 开发团队
#
# Silent X Craft Launcher (SXCL) 是一款由 Silent X Craft Launcher Dev 团队开发，
# 隶属于 SilentCodeTeams 旗下，并由 SilentStudio 管理的 Minecraft 第三方启动器。
#
# Copyright © Silent X Craft Launcher Development Team
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version, WITH the Additional Terms described
# in the LICENSE file accompanying this program.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""主题令牌与全局主题应用 —— 全应用颜色的唯一来源。

为什么需要这一层（诊断结论）
--------------------------
之前的"颜色诡异"是四套颜色打架：

1. QFluentWidgets 自己的主题色（导航栏等库控件会跟随）；
2. MainWindow 里写死的窗口背景 #1e1e1e / #f5f5f5；
3. 各页面里 50 多处内联颜色（#d0d0d0、rgba(128,128,128,.06) 之类）；
4. Qt 默认调色板（#efefef / #ffffff）—— 页面用的是原生 QWidget + 透明背景，
   没人给它上色，于是深色模式下导航栏是深色、内容区还是浅灰。

现在统一成：apply_theme() 一次性把 库主题 + 应用调色板 + 全局 QSS 设好，
并通知所有页面用 tokens() 重新套自己的样式（不重建窗口 —— SXCL 的会话页里
可能有正在跑的下载线程，重建会杀掉它们）。

参考实现：D:/SilentStudio/分币必赚/prog/SilentSafe/app/settings.py（那里用重建窗口的办法，
我们换成就地刷新，效果等价且不打断任务）。
"""

from __future__ import annotations

from typing import Optional

from PySide6.QtCore import QObject, Signal
from PySide6.QtGui import QColor, QPalette
from PySide6.QtWidgets import QApplication, QWidget
from qfluentwidgets import Theme, isDarkTheme, qconfig, setTheme, themeColor

from src.core.logger import log

__all__ = [
    "tokens", "token", "qcolor", "apply_theme", "refresh_widgets", "theme_bus",
    "on_theme_changed",
    "THEME_LABELS", "THEME_MODES",
]

# 下拉框顺序必须与 Theme 枚举一致（LIGHT, DARK, AUTO）
THEME_LABELS = ["浅色", "深色", "跟随系统"]
THEME_MODES = [Theme.LIGHT, Theme.DARK, Theme.AUTO]

# ── 令牌表 ───────────────────────────────────────────────────────
# 背景取自 QFluentWidgets 的 FluentBackgroundTheme.DEFAULT = (#f3f3f3, #202020)，
# 其余按 Fluent 语义补齐，保证卡片、边框、文字层级和库控件在同一个色系里。

_LIGHT = {
    "bg": "#f3f3f3",
    "bg_nav": "#f0f4f9",
    "card": "#ffffff",
    "card_hover": "#f7f7f7",
    "border": "#e1e1e1",
    "border_strong": "#c8c8c8",
    "separator": "#e8e8e8",
    "hover": "rgba(0, 0, 0, 0.05)",
    "hover_strong": "rgba(0, 0, 0, 0.09)",
    "hover_bg": "rgba(0, 0, 0, 0.05)",
    "hover_bg_strong": "rgba(0, 0, 0, 0.10)",
    "text": "#1a1a1a",
    "text_secondary": "#5d5d5d",
    "text_tertiary": "#8a8a8a",
    "text_disabled": "#a6a6a6",
    "input_bg": "#ffffff",
    "input_border": "#d0d0d0",
    "track": "rgba(0, 0, 0, 0.10)",
    "success": "#0f7b0f",
    "warning": "#9d5d00",
    "danger": "#c42b1c",
    "info": "#005fb8",
    "on_accent": "#ffffff",
}

_DARK = {
    "bg": "#202020",
    "bg_nav": "#19212a",
    "card": "#2b2b2b",
    "card_hover": "#333333",
    "border": "#3d3d3d",
    "border_strong": "#555555",
    "separator": "#383838",
    "hover": "rgba(255, 255, 255, 0.06)",
    "hover_strong": "rgba(255, 255, 255, 0.11)",
    "hover_bg": "rgba(255, 255, 255, 0.05)",
    "hover_bg_strong": "rgba(255, 255, 255, 0.10)",
    "text": "#ffffff",
    "text_secondary": "#c7c7c7",
    "text_tertiary": "#9a9a9a",
    "text_disabled": "#6b6b6b",
    "input_bg": "#2d2d2d",
    "input_border": "#4a4a4a",
    "track": "rgba(255, 255, 255, 0.14)",
    "success": "#6ccb5f",
    "warning": "#fce100",
    "danger": "#ff99a4",
    "info": "#60cdff",
    "on_accent": "#000000",
}


class _ThemeBus(QObject):
    """主题变化广播（页面连上它在原地重刷自己的内联样式）。"""

    changed = Signal()


theme_bus = _ThemeBus()


def _palette_dict() -> dict:
    return _DARK if isDarkTheme() else _LIGHT


def tokens() -> dict:
    """当前主题的完整令牌表（已把强调色替换为库里的实时值）。"""
    data = dict(_palette_dict())
    data["accent"] = themeColor().name()
    # 强调色上的文字：Fluent 深色主题用深色字，浅色主题用白字
    data["on_accent"] = _DARK["on_accent"] if isDarkTheme() else _LIGHT["on_accent"]
    return data


def token(name: str, default: str = "#000000") -> str:
    """取单个令牌（颜色字符串，可能是 #RRGGBB 或 rgba(...)）。"""
    return tokens().get(name, default)


def qcolor(name: str) -> "QColor":
    """取令牌并转成 QColor（供自绘控件使用；支持 rgba 写法）。"""
    raw = token(name)
    if raw.startswith("rgba"):
        try:
            inside = raw[raw.index("(") + 1: raw.index(")")]
            parts = [p.strip() for p in inside.split(",")]
            r, g, b = (int(float(p)) for p in parts[:3])
            alpha = parts[3] if len(parts) > 3 else "1"
            a = float(alpha) if "." in alpha else float(alpha) / 255.0
            return QColor(r, g, b, int(round(a * 255)))
        except Exception:
            return QColor(raw)
    return QColor(raw)


# ── 应用 ─────────────────────────────────────────────────────────


def _build_palette(dark: bool) -> QPalette:
    """给原生 Qt 控件用的调色板（qfw 控件不吃这个，它们用自己的 QSS）。"""
    t = _DARK if dark else _LIGHT
    accent = themeColor()
    pal = QPalette()
    pal.setColor(QPalette.Window, QColor(t["bg"]))
    pal.setColor(QPalette.WindowText, QColor(t["text"]))
    pal.setColor(QPalette.Base, QColor(t["input_bg"]))
    pal.setColor(QPalette.AlternateBase, QColor(t["card"]))
    pal.setColor(QPalette.Text, QColor(t["text"]))
    pal.setColor(QPalette.PlaceholderText, QColor(t["text_tertiary"]))
    pal.setColor(QPalette.Button, QColor(t["card"]))
    pal.setColor(QPalette.ButtonText, QColor(t["text"]))
    pal.setColor(QPalette.BrightText, QColor(t["danger"]))
    pal.setColor(QPalette.ToolTipBase, QColor(t["card"]))
    pal.setColor(QPalette.ToolTipText, QColor(t["text"]))
    pal.setColor(QPalette.Highlight, accent)
    pal.setColor(QPalette.HighlightedText, QColor(t["on_accent"]))
    pal.setColor(QPalette.Link, accent)
    pal.setColor(QPalette.Mid, QColor(t["border"]))
    pal.setColor(QPalette.Dark, QColor(t["border_strong"]))
    for role in (QPalette.WindowText, QPalette.Text, QPalette.ButtonText):
        pal.setColor(QPalette.Disabled, role, QColor(t["text_disabled"]))
    return pal


def global_qss() -> str:
    """原生控件（非 qfw）的兜底样式：滚动条、输入框、列表、提示等。"""
    t = tokens()
    return f"""
    QWidget {{ color: {t['text']}; }}
    QScrollArea, QStackedWidget, QWidget#qt_scrollarea_viewport {{ background: transparent; }}
    QToolTip {{
        background: {t['card']}; color: {t['text']};
        border: 1px solid {t['border']}; padding: 4px 6px;
    }}
    QLineEdit, QPlainTextEdit, QTextEdit {{
        background: {t['input_bg']}; color: {t['text']};
        border: 1px solid {t['input_border']}; border-radius: 6px; padding: 6px 10px;
        selection-background-color: {t['accent']}; selection-color: {t['on_accent']};
    }}
    QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus {{ border-color: {t['accent']}; }}
    QListView, QListWidget, QTreeView, QTableView {{
        background: transparent; color: {t['text']}; border: none;
        selection-background-color: {t['hover_strong']}; selection-color: {t['text']};
    }}
    QScrollBar:vertical {{ background: transparent; width: 10px; margin: 2px; }}
    QScrollBar::handle:vertical {{
        background: {t['border_strong']}; border-radius: 4px; min-height: 28px;
    }}
    QScrollBar::handle:vertical:hover {{ background: {t['text_tertiary']}; }}
    QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; width: 0; }}
    QScrollBar::add-page, QScrollBar::sub-page {{ background: transparent; }}
    QScrollBar:horizontal {{ background: transparent; height: 10px; margin: 2px; }}
    QScrollBar::handle:horizontal {{
        background: {t['border_strong']}; border-radius: 4px; min-width: 28px;
    }}
    QProgressBar {{
        background: {t['track']}; border: none; border-radius: 3px; color: {t['text']};
    }}
    QProgressBar::chunk {{ background: {t['accent']}; border-radius: 3px; }}
    QMenu {{ background: {t['card']}; color: {t['text']}; border: 1px solid {t['border']}; }}
    QMenu::item:selected {{ background: {t['hover_strong']}; }}
    """


def on_theme_changed(callback) -> None:
    """注册"主题一变就重刷样式"的回调，并立刻执行一次。

    页面在 __init__ 末尾调用它最省事：构造时套一遍样式，之后每次切主题自动重刷。
    """
    theme_bus.changed.connect(callback)
    try:
        callback()
    except Exception as exc:      # 构造期控件还没建全，忽略即可
        log.debug("主题样式初始化跳过: %s", exc)


def apply_theme(mode: Optional[Theme] = None, app: Optional[QApplication] = None) -> None:
    """切换/初始化主题：库主题 + 应用调色板 + 全局 QSS + 通知页面。"""
    application = app or QApplication.instance()
    if mode is not None:
        setTheme(mode)

    dark = isDarkTheme()
    if application is not None:
        application.setPalette(_build_palette(dark))
        application.setStyleSheet(global_qss())

    refresh_widgets()
    theme_bus.changed.emit()


def refresh_widgets() -> None:
    """让已存在的控件重新套用样式（不重建窗口）。"""
    application = QApplication.instance()
    if application is None:
        return
    for widget in application.topLevelWidgets():
        _repolish_tree(widget)


def _repolish_tree(widget: QWidget) -> None:
    try:
        style = widget.style()
        style.unpolish(widget)
        style.polish(widget)
        widget.update()
    except Exception:
        pass
    for child in widget.findChildren(QWidget):
        try:
            style = child.style()
            style.unpolish(child)
            style.polish(child)
            child.update()
        except Exception:
            continue
