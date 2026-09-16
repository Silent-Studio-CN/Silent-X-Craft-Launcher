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
"""按键教学 —— 把"这个键干嘛用"变成可以一步步跟着做的引导。

这就是"比 FCL 更好的按键帮助"的核心：FCL 给你一个编辑器和一个默认布局，
剩下的全靠自己摸索；我们给的是**按顺序讲清楚的引导 + 可验证的检查项**。

每一步都带：
* 让用户做什么（action 文案）
* 高亮哪个控件（control_id）
* 会触发什么键（keys），安卓端可以据此校验"刚才那一下确实生效了"
* 为什么（等价键盘操作），以及踩坑提示
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List

from src.core.keymap.model import ControlButton, KeymapLayout

__all__ = ["GuideStep", "build_guide", "guide_to_markdown"]

#: 动作 -> (标题, 怎么教, 为什么/提示)
_LESSONS = {
    "forward": ("会走路", "推住左边的摇杆/方向键往前走", "键盘上是 W。推到底会快走"),
    "jump": ("会跳", "点一下「跳」", "键盘是空格。长按可以连续跳，跳一格高的坎不用手忙脚乱"),
    "attack": ("会挖方块/打怪", "长按「挖」，对着方块/怪物",
               "键盘是鼠标左键。长按=一直挖，别一下一下点，手指会累"),
    "use": ("会放方块/用东西", "点一下「放」", "键盘是鼠标右键。开门、喂动物、吃食物都是它"),
    "sneak": ("不会掉下去", "按住「潜行」再走到方块边缘",
              "键盘是 Shift。边缘不会掉下去，搭桥必备；双击可以切成常驻潜行"),
    "inventory": ("会开背包", "点一下「背包」", "键盘是 E。长按一般是聊天"),
    "drop": ("会丢东西", "点一下「丢弃」", "键盘是 Q，小心别把好东西扔了"),
    "sprint": ("会疾跑", "按住「疾跑」同时往前推", "键盘是 Ctrl；也可以双击前进方向"),
    "perspective": ("会切视角", "点一下「视角」", "键盘是 F5，PVP 时看背后很有用"),
    "swap_offhand": ("会用副手", "点一下「副手」", "键盘是 F，把盾牌/火把塞到副手"),
    "chat": ("会发消息", "长按「背包」", "键盘是 T"),
    "fly_toggle": ("会飞", "双击「升」", "创造模式双击空格开始/停止飞行"),
    "hotbar_1": ("会选快捷栏", "点数字键切物品", "键盘就是 1-9，建造时最常用"),
}


@dataclass
class GuideStep:
    order: int
    title: str
    instruction: str
    why: str
    control_id: str = ""
    keys: List[str] = field(default_factory=list)
    tap: str = "press"          # press / long_press / click / double_click
    optional: bool = False

    def to_dict(self) -> dict:
        return {
            "order": self.order, "title": self.title, "instruction": self.instruction,
            "why": self.why, "control_id": self.control_id, "keys": list(self.keys),
            "tap": self.tap, "optional": self.optional,
        }


#: 教学顺序：先能走能跳能挖，再教容易卡住的细节（FCL 完全没有这一步）
_ORDER = [
    ("forward", "press", False),
    ("jump", "press", False),
    ("attack", "press", False),
    ("use", "press", False),
    ("sneak", "press", False),
    ("inventory", "press", False),
    ("sprint", "press", True),
    ("drop", "press", True),
    ("swap_offhand", "press", True),
    ("perspective", "press", True),
    ("fly_toggle", "double_click", True),
]


def build_guide(layout: KeymapLayout) -> List[GuideStep]:
    """按布局里**实际存在的绑定**生成教学步骤。"""
    found: dict[str, tuple[str, str, list[str]]] = {}
    for control in layout.buttons:
        if not isinstance(control, ControlButton):
            continue
        for kind, binding in control.events.items():
            if binding.action and binding.action not in found:
                found[binding.action] = (control.id, kind, list(binding.keys))
    if layout.directions:
        direction = layout.directions[0]
        found.setdefault("forward", (direction.id, "press", [direction.keys.get("up", "KEY_W")]))

    steps: List[GuideStep] = []
    for index, (action, default_tap, optional) in enumerate(_ORDER, start=1):
        if action not in found:
            continue
        control_id, kind, keys = found[action]
        title, instruction, why = _LESSONS.get(action, (action, f"试一下「{action}」", ""))
        steps.append(GuideStep(order=len(steps) + 1, title=title, instruction=instruction,
                               why=why, control_id=control_id, keys=keys,
                               tap=kind if kind else default_tap, optional=optional))

    # 快捷栏单独合并成一步，避免教学列表太长
    hotbar = [c.id for c in layout.buttons if c.id.startswith("slot")]
    if hotbar:
        numbers = sorted(int(cid.replace("slot", "")) for cid in hotbar)
        steps.append(GuideStep(
            order=len(steps) + 1,
            title="会换物品",
            instruction=f"点底部 {numbers[0]}-{numbers[-1]} 切换手上的物品",
            why="键盘就是数字键 1-9；建造时不用开背包翻",
            control_id=hotbar[0], keys=[f"KEY_{numbers[0]}"], optional=True,
        ))

    # 触摸板/摇杆的隐藏操作
    if layout.directions:
        steps.append(GuideStep(
            order=len(steps) + 1,
            title="会看四周",
            instruction="在屏幕空白处滑动 = 转动视角；两根手指滑动 = 移动（如果布局开了双区）",
            why="这是 FCL 里最容易卡住的一步：很多人以为要点了按钮才能转身",
            control_id="", keys=[], optional=True,
        ))
    return steps


def guide_to_markdown(layout: KeymapLayout, steps: List[GuideStep] | None = None) -> str:
    """导出成 Markdown —— 可以直接贴到帮助页/群里当教程。"""
    items = steps if steps is not None else build_guide(layout)
    lines = [f"### 「{layout.name}」按键教学（{layout.screen}）", ""]
    for step in items:
        flag = "（可选）" if step.optional else ""
        lines.append(f"{step.order}. **{step.title}**{flag}：{step.instruction}")
        if step.why:
            lines.append(f"   - {step.why}")
        if step.keys:
            lines.append(f"   - 等价键盘：{' + '.join(step.keys)}")
    return "\n".join(lines)
