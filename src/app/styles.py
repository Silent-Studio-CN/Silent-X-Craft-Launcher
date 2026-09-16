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
"""统一视觉规格（唯一来源）。

规矩就一条：**页面里不许出现硬编码颜色**，一律从这里取。

为什么要有这个文件：之前 36 处 setStyleSheet、22 处写死的十六进制色散在各个页面里，
于是同一种"次要说明文字"在主页是 #888、任务页是 #999、进度页是 #888888 ——
切深浅色主题时有的跟着变、有的一动不动，这就是"割裂感"的来源。

PCL 的做法是整块卡片、不画分隔线；card_qss / section_card_qss 也是这个思路：
分区感交给卡片的圆角与间距，而不是画一条灰线。
"""

from __future__ import annotations

from src.app.theme import token

__all__ = [
    "card_qss", "section_card_qss", "muted_qss", "ghost_button_qss",
    "chip_qss", "badge_color", "muted_color",
]


def card_qss(radius: int = 8, padding: str = "16px 18px") -> str:
    """标准卡片：一块底色 + 圆角，不画边框线。"""
    return (f"QWidget {{ background: {token('card')}; border: none;"
            f" border-radius: {radius}px; padding: {padding}; }}")


def section_card_qss(radius: int = 8) -> str:
    """可折叠分区（SectionCard）的外壳。"""
    return (f"QWidget#SectionCard {{ background: {token('card')};"
            f" border: 1px solid {token('border')}; border-radius: {radius}px; }}"
            f"QWidget#SectionCard:hover {{ border-color: {token('accent')}; }}")


def muted_qss(size: int = 12, margin: str = "0") -> str:
    """次要说明文字（替代散落各处的 #888 / #999 / #888888）。"""
    return f"color: {token('text_tertiary')}; font-size: {size}px; margin: {margin};"


def ghost_button_qss() -> str:
    """无边框小按钮（清除、编辑这类）。"""
    return (f"QPushButton {{ border: none; background: transparent;"
            f" color: {token('text_tertiary')}; font-weight: bold; }}"
            f"QPushButton:hover {{ color: {token('danger')}; }}")


def chip_qss(color_name: str) -> str:
    """小标签（最新 / 推荐 / Beta / 出错）。color_name 是令牌名，不是颜色值。"""
    color = token(color_name)
    return (f"QLabel {{ color: {color}; border: 1px solid {color};"
            f" border-radius: 5px; padding: 1px 8px; }}")


def badge_color(state: str) -> str:
    """状态 -> 令牌名：统一"进行中/成功/失败"三种语义色，别各页自己挑蓝绿红。"""
    return {
        "running": "accent",
        "done": "success",
        "failed": "danger",
        "warn": "warning",
    }.get(state, "text_tertiary")


def muted_color() -> str:
    """次要文字的色值（需要 setTextColor 时用）。"""
    return token("text_tertiary")
