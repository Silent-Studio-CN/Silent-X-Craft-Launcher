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
"""场景预设 —— 打开就能玩的几套布局（FCL 只有一个默认布局，这是我们的第一个差异点）。

设计原则
--------
1. 每个预设都**自带教学提示**（hint），进游戏长按按钮就能看到这个键干嘛用；
2. 坐标是归一化的，横竖屏各有自己的排布；
3. 预设是"起点"而不是"终点"：用户可以在此基础上改，改完可以导出分享。
"""

from __future__ import annotations

from typing import Dict

from src.core.keymap.model import Binding, ControlButton, DirectionControl, KeymapLayout

__all__ = ["PRESETS", "preset_names", "build_preset", "recommend_preset"]

LMB = "MOUSE_LEFT"
RMB = "MOUSE_RIGHT"


def _btn(control_id: str, label: str, x: float, y: float, w: float, h: float,
         *, action: str = "", keys: list[str] | None = None, hint: str = "",
         group: str = "right", shape: str = "round", long_action: str = "",
         long_keys: list[str] | None = None, click_action: str = "",
         click_keys: list[str] | None = None, double_action: str = "",
         double_keys: list[str] | None = None, opacity: float = 0.55,
         alias_of: str = "") -> ControlButton:
    button = ControlButton(id=control_id, label=label, hint=hint, x=x, y=y, w=w, h=h,
                           shape=shape, group=group, opacity=opacity, alias_of=alias_of)
    if action or keys:
        button.bind("press", Binding(action=action, keys=keys or [], behavior="hold"))
    if long_action or long_keys:
        button.bind("long_press", Binding(action=long_action, keys=long_keys or [], behavior="hold"))
    if click_action or click_keys:
        button.bind("click", Binding(action=click_action, keys=click_keys or [], behavior="tap"))
    if double_action or double_keys:
        button.bind("double_click", Binding(action=double_action, keys=double_keys or [],
                                            behavior="toggle"))
    return button


def _move(screen: str) -> DirectionControl:
    if screen == "portrait":
        return DirectionControl(id="move", label="移动", hint="推到底可以快走，配合潜行键=疾跑",
                                x=0.06, y=0.62, w=0.34, h=0.22, style="rocker", group="left")
    return DirectionControl(id="move", label="移动", hint="推到底可以快走，配合潜行键=疾跑",
                            x=0.03, y=0.58, w=0.20, h=0.36, style="rocker", group="left")


def _core_buttons(screen: str) -> list[ControlButton]:
    """移动/跳跃/挖掘/放置 —— 任何预设都该有的四个。"""
    if screen == "portrait":
        return [
            _btn("jump", "跳", 0.78, 0.66, 0.16, 0.09, action="jump", keys=["KEY_SPACE"],
                 hint="空格键：跳跃。按住可以连续跳（跑酷常用）"),
            _btn("mine", "挖", 0.60, 0.78, 0.16, 0.09, action="attack", keys=[LMB],
                 hint="左键：攻击 / 挖掘。按住就是一直挖，不用反复点"),
            _btn("place", "放", 0.78, 0.78, 0.16, 0.09, action="use", keys=[RMB],
                 hint="右键：放置方块 / 使用物品 / 开门 / 喂动物"),
            _btn("sneak", "潜行", 0.42, 0.78, 0.16, 0.09, action="sneak", keys=["KEY_LEFT_SHIFT"],
                 hint="Shift：潜行（不会掉下方块）。双击可以切换成常驻潜行",
                 double_action="sneak", double_keys=["KEY_LEFT_SHIFT"], shape="pill"),
        ]
    return [
        _btn("jump", "跳", 0.86, 0.62, 0.09, 0.16, action="jump", keys=["KEY_SPACE"],
             hint="空格键：跳跃。按住可以连续跳（跑酷常用）"),
        _btn("mine", "挖", 0.74, 0.74, 0.10, 0.18, action="attack", keys=[LMB],
             hint="左键：攻击 / 挖掘。按住就是一直挖，不用反复点"),
        _btn("place", "放", 0.86, 0.74, 0.10, 0.18, action="use", keys=[RMB],
             hint="右键：放置方块 / 使用物品 / 开门"),
        _btn("sneak", "潜行", 0.62, 0.80, 0.10, 0.10, action="sneak", keys=["KEY_LEFT_SHIFT"],
             hint="Shift：潜行。双击 = 常驻潜行（挂机搭桥很省手）",
             double_action="sneak", double_keys=["KEY_LEFT_SHIFT"], shape="pill"),
    ]


def _hotbar(screen: str) -> list[ControlButton]:
    """快捷栏 1-9：FCL 需要自己一个个加，我们放进"建造/生存"预设。"""
    buttons: list[ControlButton] = []
    count = 9 if screen == "landscape" else 5
    width = 0.045 if screen == "landscape" else 0.09
    gap = 0.008
    start_x = 0.5 - (count * (width + gap) - gap) / 2
    y = 0.94 if screen == "landscape" else 0.45
    for index in range(count):
        buttons.append(_btn(
            f"slot{index + 1}", str(index + 1),
            start_x + index * (width + gap), y, width, 0.05 if screen == "landscape" else 0.06,
            action=f"hotbar_{index + 1}", keys=[f"KEY_{index + 1}"],
            hint=f"快捷栏第 {index + 1} 格（键盘 {index + 1}）",
            group="center", shape="square", opacity=0.45))
    return buttons


def _survival(screen: str) -> KeymapLayout:
    buttons = _core_buttons(screen) + [
        _btn("inventory", "背包", 0.62, 0.60, 0.09, 0.14, action="inventory", keys=["KEY_E"],
             hint="E：打开背包。长按 = 打开聊天（发消息）",
             long_action="chat", long_keys=["KEY_T"]),
        _btn("drop", "丢弃", 0.50, 0.68, 0.09, 0.14, action="drop", keys=["KEY_Q"],
             hint="Q：丢掉手上物品（小心别把钻石扔了）"),
    ]
    return KeymapLayout(
        name="生存", screen=screen, description="探索/挖矿/打怪用的一套完整布局",
        buttons=buttons, directions=[_move(screen)],
        meta={"builtin": True, "recommended_for": "survival"},
    )


def _building(screen: str) -> KeymapLayout:
    buttons = _core_buttons(screen) + _hotbar(screen) + [
        _btn("inventory", "背包", 0.62, 0.60, 0.09, 0.14, action="inventory", keys=["KEY_E"],
             hint="E：背包；长按 = 聊天"),
        _btn("fly_up", "升", 0.50, 0.44, 0.08, 0.10, action="jump", keys=["KEY_SPACE"],
             hint="创造模式飞行上升（双击空格开始/停止飞行）",
             double_action="jump", double_keys=["KEY_SPACE"], group="center",
             alias_of="jump"),
        _btn("fly_down", "降", 0.50, 0.56, 0.08, 0.10, action="sneak", keys=["KEY_LEFT_SHIFT"],
             hint="创造模式飞行下降", group="center", alias_of="sneak"),
        _btn("sprint", "疾跑", 0.30, 0.86, 0.10, 0.09, action="sprint", keys=["KEY_LEFT_CTRL"],
             hint="Ctrl：疾跑（也可以双击前进）", shape="pill", group="left"),
    ]
    return KeymapLayout(
        name="建造", screen=screen, description="搭建筑/红石用：带快捷栏 1-9 与飞行上下",
        buttons=buttons, directions=[_move(screen)],
        meta={"builtin": True, "recommended_for": "building"},
    )


def _pvp(screen: str) -> KeymapLayout:
    buttons = _core_buttons(screen) + [
        _btn("sprint", "疾跑", 0.30, 0.86, 0.10, 0.09, action="sprint", keys=["KEY_LEFT_CTRL"],
             hint="Ctrl 疾跑：追击/逃跑必备", shape="pill", group="left"),
        _btn("swap", "副手", 0.74, 0.62, 0.08, 0.10, action="swap_offhand", keys=["KEY_F"],
             hint="F：把主手物品换到副手（盾牌/火把常用）"),
        _btn("shield", "盾", 0.62, 0.72, 0.09, 0.14, action="use", keys=[RMB],
             hint="右键举盾（副手放盾牌）", alias_of="use"),
        _btn("inventory", "背包", 0.62, 0.58, 0.09, 0.12, action="inventory", keys=["KEY_E"],
             hint="E：背包"),
        _btn("perspective", "视角", 0.50, 0.90, 0.10, 0.07, action="perspective", keys=["KEY_F5"],
             hint="F5：切换第一/第三人称（PVP 看背后很有用）", shape="pill", group="center"),
    ]
    return KeymapLayout(
        name="对战", screen=screen, description="PVP 向：疾跑、副手、盾牌、切视角都放手指边",
        buttons=buttons, directions=[_move(screen)],
        meta={"builtin": True, "recommended_for": "pvp"},
    )


def _minimal(screen: str) -> KeymapLayout:
    buttons = _core_buttons(screen) + [
        _btn("inventory", "背包", 0.62, 0.62, 0.09, 0.14, action="inventory", keys=["KEY_E"],
             hint="E：背包"),
    ]
    return KeymapLayout(
        name="极简", screen=screen, description="只留四个必用键，屏幕最干净（新手先用这套）",
        buttons=buttons, directions=[_move(screen)],
        meta={"builtin": True, "recommended_for": "newbie"},
    )


def _one_hand(screen: str) -> KeymapLayout:
    """单手：全部塞到右下角，适合站着刷东西。"""
    buttons = [
        _btn("jump", "跳", 0.86, 0.84, 0.11, 0.12, action="jump", keys=["KEY_SPACE"],
             hint="空格：跳", group="right"),
        _btn("mine", "挖", 0.72, 0.84, 0.11, 0.12, action="attack", keys=[LMB],
             hint="左键：挖/打（按住连续）", group="right"),
        _btn("place", "放", 0.86, 0.68, 0.11, 0.12, action="use", keys=[RMB],
             hint="右键：放方块/使用", group="right"),
        _btn("sneak", "潜行", 0.72, 0.68, 0.11, 0.12, action="sneak", keys=["KEY_LEFT_SHIFT"],
             hint="Shift：潜行", group="right", shape="pill"),
        _btn("inventory", "背包", 0.86, 0.52, 0.11, 0.12, action="inventory", keys=["KEY_E"],
             hint="E：背包", group="right"),
    ]
    return KeymapLayout(
        name="单手", screen="portrait", description="单手模式：所有按键集中在右下，走路用触摸板",
        buttons=buttons, directions=[_move("portrait")],
        meta={"builtin": True, "recommended_for": "one_hand"},
    )


PRESETS: Dict[str, callable] = {
    "minimal": _minimal,
    "survival": _survival,
    "building": _building,
    "pvp": _pvp,
    "one_hand": _one_hand,
}

PRESET_LABELS = {
    "minimal": "极简（推荐新手）",
    "survival": "生存",
    "building": "建造",
    "pvp": "对战",
    "one_hand": "单手",
}


def preset_names() -> list[str]:
    return list(PRESETS)


def build_preset(name: str, screen: str = "landscape", mc_version: str = "") -> KeymapLayout:
    """构造预设布局；未知名字回落到极简。"""
    builder = PRESETS.get(name) or _minimal
    layout = builder(screen)
    layout.mc_version = mc_version
    return layout


def recommend_preset(mc_version: str = "", screen: str = "landscape", installed_mods: int = 0) -> str:
    """按版本/场景推荐一个预设（FCL 不会推荐，只会给你一个默认布局）。"""
    if screen == "portrait":
        return "one_hand"
    return "minimal" if not mc_version else "survival"
