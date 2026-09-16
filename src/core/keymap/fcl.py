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
"""FCL（Fold Craft Launcher）布局导入/导出 —— 降低"从 FCL 换过来"的门槛。

注意
----
FCL 的布局是它自己的 Android 数据（MenuSetting / ControlButton / ControlDirection），
字段名与坐标单位（像素）都可能随版本变化。这里做的是**容错导入**：

* 坐标支持像素（配合 --width/--height 归一化）与已经归一化的 0~1 两种写法；
* 按键支持 GLFW 整数键码、上下左右/空格等常用名，以及 MOUSE_* 写法；
* 认不出的字段原样保留在 layout.meta["fcl_raw"] 里，方便对照排查。

如果你手头有 FCL 导出的布局文件，跑一次
    python scripts/keymap_tool.py import-fcl <文件>
就能看到映射结果，缺什么字段告诉我即可补齐。
"""

from __future__ import annotations

from typing import Any, Iterable, Optional

from src.core.keymap.model import Binding, ControlButton, DirectionControl, KeymapLayout

__all__ = ["import_layout", "to_fcl", "GLFW_KEYS"]

#: GLFW 常用键码 -> 我们的键名（FCL 存的是 GLFW 键码）
GLFW_KEYS: dict[int, str] = {
    32: "KEY_SPACE", 39: "KEY_APOSTROPHE", 44: "KEY_COMMA", 45: "KEY_MINUS", 46: "KEY_PERIOD",
    47: "KEY_SLASH", 48: "KEY_0", 49: "KEY_1", 50: "KEY_2", 51: "KEY_3", 52: "KEY_4",
    53: "KEY_5", 54: "KEY_6", 55: "KEY_7", 56: "KEY_8", 57: "KEY_9",
    59: "KEY_SEMICOLON", 61: "KEY_EQUAL", 65: "KEY_A", 66: "KEY_B", 67: "KEY_C", 68: "KEY_D",
    69: "KEY_E", 70: "KEY_F", 71: "KEY_G", 72: "KEY_H", 73: "KEY_I", 74: "KEY_J", 75: "KEY_K",
    76: "KEY_L", 77: "KEY_M", 78: "KEY_N", 79: "KEY_O", 80: "KEY_P", 81: "KEY_Q", 82: "KEY_R",
    83: "KEY_S", 84: "KEY_T", 85: "KEY_U", 86: "KEY_V", 87: "KEY_W", 88: "KEY_X", 89: "KEY_Y",
    90: "KEY_Z", 96: "KEY_GRAVE_ACCENT", 256: "KEY_ESCAPE", 257: "KEY_ENTER", 258: "KEY_TAB",
    259: "KEY_BACKSPACE", 260: "KEY_INSERT", 261: "KEY_DELETE", 262: "KEY_RIGHT", 263: "KEY_LEFT",
    264: "KEY_DOWN", 265: "KEY_UP", 266: "KEY_PAGE_UP", 267: "KEY_PAGE_DOWN", 268: "KEY_HOME",
    269: "KEY_END", 280: "KEY_CAPS_LOCK", 290: "KEY_F1", 291: "KEY_F2", 292: "KEY_F3",
    293: "KEY_F4", 294: "KEY_F5", 295: "KEY_F6", 296: "KEY_F7", 297: "KEY_F8", 298: "KEY_F9",
    299: "KEY_F10", 300: "KEY_F11", 301: "KEY_F12",
    340: "KEY_LEFT_SHIFT", 341: "KEY_LEFT_CONTROL", 342: "KEY_LEFT_ALT", 343: "KEY_LEFT_SUPER",
    344: "KEY_RIGHT_SHIFT", 345: "KEY_RIGHT_CONTROL", 346: "KEY_RIGHT_ALT", 347: "KEY_RIGHT_SUPER",
    -1: "MOUSE_LEFT", -2: "MOUSE_RIGHT", -3: "MOUSE_MIDDLE",
}

_TEXT_TO_KEY = {
    "space": "KEY_SPACE", "空格": "KEY_SPACE", "shift": "KEY_LEFT_SHIFT", "潜行": "KEY_LEFT_SHIFT",
    "ctrl": "KEY_LEFT_CONTROL", "control": "KEY_LEFT_CONTROL", "alt": "KEY_LEFT_ALT",
    "enter": "KEY_ENTER", "tab": "KEY_TAB", "esc": "KEY_ESCAPE", "escape": "KEY_ESCAPE",
    "left": "MOUSE_LEFT", "右键": "MOUSE_RIGHT", "right": "MOUSE_RIGHT", "左键": "MOUSE_LEFT",
    "e": "KEY_E", "q": "KEY_Q", "w": "KEY_W", "a": "KEY_A", "s": "KEY_S", "d": "KEY_D",
    "f": "KEY_F", "t": "KEY_T", "f5": "KEY_F5",
}

_ACTION_BY_KEY = {
    "KEY_SPACE": "jump", "KEY_LEFT_SHIFT": "sneak", "KEY_E": "inventory", "KEY_Q": "drop",
    "KEY_F": "swap_offhand", "KEY_F5": "perspective", "KEY_T": "chat",
    "KEY_LEFT_CONTROL": "sprint", "MOUSE_LEFT": "attack", "MOUSE_RIGHT": "use",
    "KEY_1": "hotbar_1", "KEY_2": "hotbar_2", "KEY_3": "hotbar_3", "KEY_4": "hotbar_4",
    "KEY_5": "hotbar_5", "KEY_6": "hotbar_6", "KEY_7": "hotbar_7", "KEY_8": "hotbar_8",
    "KEY_9": "hotbar_9",
}


def _norm_key(raw: Any) -> str:
    """把 FCL 里各种按键表示法统一成我们的键名。"""
    if raw is None:
        return ""
    if isinstance(raw, bool):
        return ""
    if isinstance(raw, (int, float)):
        return GLFW_KEYS.get(int(raw), f"KEY_{int(raw)}")
    text = str(raw).strip()
    if not text:
        return ""
    if text.startswith(("KEY_", "MOUSE_")):
        return text.upper()
    if text.isdigit():
        return GLFW_KEYS.get(int(text), f"KEY_{text}")
    low = text.lower()
    if low in _TEXT_TO_KEY:
        return _TEXT_TO_KEY[low]
    if len(text) == 1 and text.isalpha():
        return f"KEY_{text.upper()}"
    return f"KEY_{text.upper()}"


def _keys_of(data: dict, *names: str) -> list[str]:
    for name in names:
        value = data.get(name)
        if value is None:
            continue
        if isinstance(value, (list, tuple)):
            return [k for k in (_norm_key(item) for item in value) if k]
        key = _norm_key(value)
        if key:
            return [key]
    return []


def _pick(data: dict, *names: str, default: Any = None) -> Any:
    for name in names:
        if name in data and data[name] is not None:
            return data[name]
    return default


def _rect(data: dict, screen_w: float, screen_h: float) -> tuple[float, float, float, float]:
    """坐标兼容：FCL 用像素，我们存 0~1。"""
    x = float(_pick(data, "x", "left", default=0) or 0)
    y = float(_pick(data, "y", "top", default=0) or 0)
    w = float(_pick(data, "width", "w", default=0) or 0)
    h = float(_pick(data, "height", "h", default=0) or 0)
    if x > 1.0 or y > 1.0 or w > 1.0 or h > 1.0:
        x, w = x / screen_w, w / screen_w
        y, h = y / screen_h, h / screen_h
    return x, y, w or 0.09, h or 0.14


def import_layout(data: Any, *, screen_w: float = 2400, screen_h: float = 1080,
                  name: str = "") -> KeymapLayout:
    """把 FCL 的布局数据转成我们的 KeymapLayout。"""
    if isinstance(data, dict):
        raw_controls = (_pick(data, "views", "controls", "buttons", "widgets", default=[]) or [])
        layout_name = name or str(_pick(data, "name", "title", default="从 FCL 导入"))
        screen = "portrait" if screen_h > screen_w else "landscape"
    else:
        raw_controls = data or []
        layout_name = name or "从 FCL 导入"
        screen = "landscape" if screen_w >= screen_h else "portrait"

    buttons: list[ControlButton] = []
    directions: list[DirectionControl] = []
    for index, item in enumerate(raw_controls):
        if not isinstance(item, dict):
            continue
        kind = str(_pick(item, "type", "viewType", "kind", default="button")).lower()
        rect = _rect(item, screen_w, screen_h)
        is_direction = any(token in kind for token in ("direction", "joystick", "dpad", "rocker"))
        if is_direction:
            directions.append(DirectionControl(
                id=str(_pick(item, "id", default=f"move{index}") or f"move{index}"),
                label=str(_pick(item, "text", "label", default="移动") or "移动"),
                x=rect[0], y=rect[1], w=rect[2], h=rect[3],
                style="rocker" if "rocker" in kind or "joystick" in kind else "dpad_compact",
                keys={k: v for k, v in DirectionControl(id="x").keys.items()},
            ))
            continue

        events: dict[str, Binding] = {}
        press_keys = _keys_of(item, "keycodes", "keys", "codes", "key")
        click_keys = _keys_of(item, "clickKeycodes", "clickKeys")
        long_keys = _keys_of(item, "longPressKeycodes", "longPressKeys")
        double_keys = _keys_of(item, "doubleClickKeycodes", "doubleClickKeys")
        for kind_name, keys in (("press", press_keys), ("click", click_keys),
                                ("long_press", long_keys), ("double_click", double_keys)):
            if not keys:
                continue
            action = ""
            for key in keys:
                if key in _ACTION_BY_KEY:
                    action = _ACTION_BY_KEY[key]
                    break
            events[kind_name] = Binding(action=action, keys=keys,
                                        behavior="toggle" if kind_name == "double_click" else "hold")

        label = str(_pick(item, "text", "label", "name", default=f"键{index + 1}") or "")
        buttons.append(ControlButton(
            id=str(_pick(item, "id", default=f"fcl{index}") or f"fcl{index}"),
            label=label, x=rect[0], y=rect[1], w=rect[2], h=rect[3],
            shape="square" if "square" in kind else "round",
            opacity=float(_pick(item, "alpha", "opacity", default=0.55) or 0.55),
            events=events,
        ))

    layout = KeymapLayout(name=layout_name, screen=screen, buttons=buttons,
                          directions=directions,
                          description="由 FCL 布局导入（坐标已按屏幕尺寸归一化）")
    layout.meta["fcl_raw"] = data if isinstance(data, dict) else {"views": raw_controls}
    return layout


def to_fcl(layout: KeymapLayout, *, screen_w: int = 2400, screen_h: int = 1080) -> dict:
    """导出成 FCL 风格的视图列表（像素坐标），方便用户搬回去对比。"""
    views: list[dict] = []
    for direction in layout.directions:
        views.append({
            "type": "direction", "style": "rocker" if direction.style == "rocker" else "dpad",
            "text": direction.label,
            "x": round(direction.x * screen_w), "y": round(direction.y * screen_h),
            "width": round(direction.w * screen_w), "height": round(direction.h * screen_h),
            "keycodes": list(direction.keys.values()),
        })
    for button in layout.buttons:
        views.append({
            "type": "button", "text": button.label,
            "x": round(button.x * screen_w), "y": round(button.y * screen_h),
            "width": round(button.w * screen_w), "height": round(button.h * screen_h),
            "alpha": button.opacity,
            "keycodes": list(button.events.get("press").keys) if button.events.get("press") else [],
            "longPressKeycodes": list(button.events.get("long_press").keys)
            if button.events.get("long_press") else [],
            "doubleClickKeycodes": list(button.events.get("double_click").keys)
            if button.events.get("double_click") else [],
        })
    return {"name": layout.name, "views": views, "screenWidth": screen_w, "screenHeight": screen_h}
