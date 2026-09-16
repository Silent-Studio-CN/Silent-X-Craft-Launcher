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
"""按键映射（触屏虚拟按键）数据模型 —— 跨端共用的唯一格式。

为什么单独立一个跨端模块
------------------------
安卓端要做的"按键帮助"（虚拟按键 + 教学 + 冲突检查）本质是**一份布局数据**，
它不该绑在某个平台的 UI 上：

* 桌面端可以用大屏幕编辑、预览、导入导出，甚至按机型批量生成；
* 安卓端直接读同一份 JSON（schema = sxcl.keymap.v1），不重复定义；
* 以后要做 Web 练习页/分享站，也用同一套。

与 FCL 的关系
-------------
FCL 的 ControlButton / ControlDirection 概念我们沿用（按钮 + 方向键/摇杆，
按下/长按/单击/双击四种事件），但额外提供 FCL 没有的东西：
冲突检测、教学步骤（guide.py）、场景预设、跨端编辑、FCL 布局导入（fcl.py）。

坐标一律用 0~1 的**归一化值**（相对屏幕宽高），这样同一份布局能适配
任何分辨率与横竖屏，安卓端只需乘以屏幕尺寸。
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Optional

__all__ = [
    "SCHEMA_VERSION", "Binding", "ControlButton", "DirectionControl", "KeymapLayout",
    "Conflict", "load_layout", "save_layout",
]

SCHEMA_VERSION = "sxcl.keymap.v1"

#: 四种事件类型，语义与 FCL 一致（长按 400ms、双击 400ms 内两次）
EVENT_KINDS = ("press", "long_press", "click", "double_click")
EVENT_LABELS = {
    "press": "按下",
    "long_press": "长按",
    "click": "单击",
    "double_click": "双击",
}
BEHAVIORS = ("hold", "toggle", "tap")
SHAPES = ("round", "square", "pill")
DIRECTION_STYLES = ("dpad", "rocker", "dpad_compact")


def _clamp(value: float, low: float = 0.0, high: float = 1.0) -> float:
    return max(low, min(high, float(value)))


@dataclass
class Binding:
    """一个事件绑定的动作 + 按键 + 行为。"""

    action: str = ""                     # 逻辑动作：jump / sneak / forward / inventory ...
    keys: list[str] = field(default_factory=list)   # 注入的按键：KEY_SPACE / MOUSE_LEFT
    behavior: str = "hold"               # hold=按住 / toggle=切换 / tap=点一下

    def to_dict(self) -> dict:
        return {"action": self.action, "keys": list(self.keys), "behavior": self.behavior}

    @classmethod
    def from_dict(cls, data: Any) -> Optional["Binding"]:
        if not isinstance(data, dict):
            return None
        keys = data.get("keys") or []
        if isinstance(keys, str):
            keys = [keys]
        behavior = str(data.get("behavior") or "hold")
        return cls(
            action=str(data.get("action") or ""),
            keys=[str(k) for k in keys],
            behavior=behavior if behavior in BEHAVIORS else "hold",
        )


@dataclass
class ControlButton:
    """一个虚拟按键（FCL 的 ControlButton 对应物）。"""

    id: str
    label: str = ""
    hint: str = ""                        # 教学提示（FCL 没有）
    icon: str = ""
    x: float = 0.1
    y: float = 0.6
    w: float = 0.09
    h: float = 0.16
    shape: str = "round"
    opacity: float = 0.55
    group: str = ""                       # left / right / center，便于"换手"
    alias_of: str = ""                    # 别名按钮：与某个动作等价（如"盾"=右键"放"）
    events: dict[str, Binding] = field(default_factory=dict)

    # ── 便捷属性 ──────────────────────────────────────────────

    @property
    def rect(self) -> tuple[float, float, float, float]:
        return (self.x, self.y, self.w, self.h)

    @property
    def all_keys(self) -> list[str]:
        keys: list[str] = []
        for binding in self.events.values():
            keys.extend(binding.keys)
        return keys

    def bind(self, kind: str, binding: Binding) -> "ControlButton":
        if kind in EVENT_KINDS:
            self.events[kind] = binding
        return self

    # ── 序列化 ────────────────────────────────────────────────

    def to_dict(self) -> dict:
        return {
            "id": self.id, "label": self.label, "hint": self.hint, "icon": self.icon,
            "x": round(self.x, 4), "y": round(self.y, 4),
            "w": round(self.w, 4), "h": round(self.h, 4),
            "shape": self.shape, "opacity": round(self.opacity, 2), "group": self.group,
            "alias_of": self.alias_of,
            "events": {k: v.to_dict() for k, v in self.events.items()},
        }

    @classmethod
    def from_dict(cls, data: dict) -> "ControlButton":
        events = {}
        for kind in EVENT_KINDS:
            binding = Binding.from_dict((data.get("events") or {}).get(kind))
            if binding:
                events[kind] = binding
        shape = str(data.get("shape") or "round")
        return cls(
            id=str(data.get("id") or ""),
            label=str(data.get("label") or ""),
            hint=str(data.get("hint") or ""),
            icon=str(data.get("icon") or ""),
            x=_clamp(data.get("x", 0.1)), y=_clamp(data.get("y", 0.6)),
            w=_clamp(data.get("w", 0.09), 0.01, 1.0), h=_clamp(data.get("h", 0.16), 0.01, 1.0),
            shape=shape if shape in SHAPES else "round",
            opacity=_clamp(data.get("opacity", 0.55)),
            group=str(data.get("group") or ""),
            alias_of=str(data.get("alias_of") or ""),
            events=events,
        )


@dataclass
class DirectionControl:
    """方向键 / 摇杆（FCL 的 ControlDirection 对应物）。"""

    id: str
    label: str = ""
    hint: str = ""
    x: float = 0.03
    y: float = 0.55
    w: float = 0.22
    h: float = 0.38
    style: str = "dpad_compact"
    opacity: float = 0.5
    dead_zone: float = 0.18
    group: str = "left"
    keys: dict[str, str] = field(default_factory=lambda: {
        "up": "KEY_W", "down": "KEY_S", "left": "KEY_A", "right": "KEY_D",
    })
    sprint_key: str = "KEY_LEFT_SHIFT"     # 摇杆推到底 + 该键 = 疾跑

    @property
    def all_keys(self) -> list[str]:
        return [v for v in self.keys.values() if v]

    def to_dict(self) -> dict:
        return {
            "id": self.id, "label": self.label, "hint": self.hint,
            "x": round(self.x, 4), "y": round(self.y, 4),
            "w": round(self.w, 4), "h": round(self.h, 4),
            "style": self.style, "opacity": round(self.opacity, 2),
            "dead_zone": round(self.dead_zone, 3), "group": self.group,
            "keys": dict(self.keys), "sprint_key": self.sprint_key,
        }

    @classmethod
    def from_dict(cls, data: dict) -> "DirectionControl":
        style = str(data.get("style") or "dpad_compact")
        keys = data.get("keys") or {}
        return cls(
            id=str(data.get("id") or ""),
            label=str(data.get("label") or ""),
            hint=str(data.get("hint") or ""),
            x=_clamp(data.get("x", 0.03)), y=_clamp(data.get("y", 0.55)),
            w=_clamp(data.get("w", 0.22), 0.02, 1.0), h=_clamp(data.get("h", 0.38), 0.02, 1.0),
            style=style if style in DIRECTION_STYLES else "dpad_compact",
            opacity=_clamp(data.get("opacity", 0.5)),
            dead_zone=_clamp(data.get("dead_zone", 0.18), 0.0, 0.6),
            group=str(data.get("group") or "left"),
            keys={k: str(keys.get(k, v)) for k, v in
                  {"up": "KEY_W", "down": "KEY_S", "left": "KEY_A", "right": "KEY_D"}.items()},
            sprint_key=str(data.get("sprint_key") or "KEY_LEFT_SHIFT"),
        )


@dataclass
class Conflict:
    """按键帮助的核心之一：把"设置错了"提前告诉用户。"""

    level: str          # error / warning
    message: str
    controls: list[str] = field(default_factory=list)

    def __str__(self) -> str:  # pragma: no cover
        return f"[{self.level}] {self.message}"


@dataclass
class KeymapLayout:
    """一份完整的按键布局。"""

    name: str = "默认"
    screen: str = "landscape"            # landscape / portrait
    mc_version: str = ""
    description: str = ""
    buttons: list[ControlButton] = field(default_factory=list)
    directions: list[DirectionControl] = field(default_factory=list)
    schema: str = SCHEMA_VERSION
    meta: dict = field(default_factory=dict)

    # ── 查询 ──────────────────────────────────────────────────

    @property
    def controls(self) -> list:
        return [*self.directions, *self.buttons]

    def button(self, control_id: str):
        for control in self.controls:
            if control.id == control_id:
                return control
        return None

    def action_index(self) -> dict[str, list[str]]:
        """动作 -> 拥有它的控件 id 列表（用于"这个键在哪"搜索）。"""
        index: dict[str, list[str]] = {}
        for control in self.buttons:
            for binding in control.events.values():
                if binding.action:
                    index.setdefault(binding.action, []).append(control.id)
        for direction in self.directions:
            for name, key in direction.keys.items():
                index.setdefault(name, []).append(direction.id)
        return index

    def find(self, text: str) -> list[str]:
        """按动作名/标签/按键模糊查找控件 id（比 FCL 只能肉眼找强）。"""
        query = (text or "").strip().lower()
        if not query:
            return []
        hits: list[str] = []
        for control in self.controls:
            haystack = [control.id, getattr(control, "label", ""), getattr(control, "hint", "")]
            haystack.extend(getattr(control, "all_keys", []))
            if isinstance(control, ControlButton):
                haystack.extend(b.action for b in control.events.values())
            if any(query in str(item).lower() for item in haystack if item):
                hits.append(control.id)
        return hits

    # ── 校验 / 冲突 ───────────────────────────────────────────

    def validate(self) -> list[str]:
        errors: list[str] = []
        seen: set[str] = set()
        for control in self.controls:
            if not control.id:
                errors.append("存在没有 id 的控件")
                continue
            if control.id in seen:
                errors.append(f"控件 id 重复: {control.id}")
            seen.add(control.id)
            if control.x + control.w > 1.001 or control.y + control.h > 1.001:
                errors.append(f"{control.id}: 超出屏幕范围")
            if isinstance(control, ControlButton) and not control.events:
                errors.append(f"{control.id}: 没有绑定任何事件")
        return errors

    def conflicts(self) -> list[Conflict]:
        """冲突检测：同一按键被多个控件绑定 / 控件重叠 / 缺关键动作。"""
        result: list[Conflict] = []

        # 1) 同一个按键被多个控件抢 —— 但要区分"故意"和"冲突"：
        #    * 同一个控件的按下/长按绑同一个键 = 正常（FCL 的挖矿就是这样）；
        #    * 两个控件绑同一个键但动作相同 = 别名按钮（比如"盾"和"放"都是右键），不算错；
        #    * 动作不同才是真冲突（会互相打架）。
        per_key: dict[str, list[tuple[str, tuple[str, ...]]]] = {}
        for control in self.controls:
            actions_by_key: dict[str, set[str]] = {}
            events = getattr(control, "events", {}) or {}
            alias = getattr(control, "alias_of", "")
            for key in getattr(control, "all_keys", []):
                if alias:
                    acts = {alias}          # 声明了别名：按别名动作算，不算抢键
                else:
                    acts = {binding.action for binding in events.values()
                            if key in binding.keys and binding.action}
                actions_by_key.setdefault(key, set()).update(acts)
            for key, acts in actions_by_key.items():
                per_key.setdefault(key, []).append((control.id, tuple(sorted(acts))))

        for key, owners in sorted(per_key.items()):
            if len(owners) < 2:
                continue
            all_actions = {action for _cid, acts in owners for action in acts}
            if len(all_actions) <= 1 and all(len(acts) <= 1 for _cid, acts in owners):
                continue
            names = "、".join(cid for cid, _acts in owners)
            result.append(Conflict(
                "warning",
                f"按键 {key} 被 {len(owners)} 个控件绑到不同动作（{names}），会互相打架",
                [cid for cid, _acts in owners],
            ))

        # 2) 控件互相重叠（同一屏、面积重叠超过 60%）
        for i, first in enumerate(self.controls):
            for second in self.controls[i + 1:]:
                ratio = _overlap_ratio(first, second)
                if ratio > 0.6:
                    result.append(Conflict(
                        "warning",
                        f"{first.id} 与 {second.id} 位置重叠（{ratio * 100:.0f}%），触屏容易误触",
                        [first.id, second.id],
                    ))

        # 3) 关键动作缺失 —— 新手最容易卡住的地方
        actions = set(self.action_index())
        for required, tip in (("jump", "跳跃"), ("forward", "移动"), ("inventory", "打开背包")):
            if required not in actions and not (required == "forward" and self.directions):
                result.append(Conflict("error", f"没有绑定「{tip}」，进游戏后会寸步难行", []))

        return result

    # ── 序列化 ────────────────────────────────────────────────

    def to_dict(self) -> dict:
        return {
            "schema": self.schema,
            "name": self.name,
            "screen": self.screen,
            "mc_version": self.mc_version,
            "description": self.description,
            "meta": dict(self.meta),
            "buttons": [b.to_dict() for b in self.buttons],
            "directions": [d.to_dict() for d in self.directions],
        }

    def to_json(self, indent: int = 2) -> str:
        return json.dumps(self.to_dict(), ensure_ascii=False, indent=indent)

    @classmethod
    def from_dict(cls, data: dict) -> "KeymapLayout":
        return cls(
            name=str(data.get("name") or "未命名"),
            screen=str(data.get("screen") or "landscape"),
            mc_version=str(data.get("mc_version") or ""),
            description=str(data.get("description") or ""),
            buttons=[ControlButton.from_dict(item) for item in (data.get("buttons") or [])],
            directions=[DirectionControl.from_dict(item) for item in (data.get("directions") or [])],
            schema=str(data.get("schema") or SCHEMA_VERSION),
            meta=dict(data.get("meta") or {}),
        )

    def clone(self, name: str = "") -> "KeymapLayout":
        copy = KeymapLayout.from_dict(self.to_dict())
        if name:
            copy.name = name
        return copy


def _overlap_ratio(first, second) -> float:
    """两个控件矩形的重叠面积 / 较小者面积。"""
    left = max(first.x, second.x)
    top = max(first.y, second.y)
    right = min(first.x + first.w, second.x + second.w)
    bottom = min(first.y + first.h, second.y + second.h)
    if right <= left or bottom <= top:
        return 0.0
    overlap = (right - left) * (bottom - top)
    smaller = min(first.w * first.h, second.w * second.h)
    return overlap / smaller if smaller > 0 else 0.0


def load_layout(path: str | Path) -> KeymapLayout:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    return KeymapLayout.from_dict(data)


def save_layout(layout: KeymapLayout, path: str | Path) -> Path:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(layout.to_json(), encoding="utf-8")
    return target
