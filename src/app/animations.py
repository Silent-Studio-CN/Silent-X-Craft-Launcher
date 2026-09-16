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
"""SXCL 自己的小动画系统。

目的很单纯：让展开/收起、淡入淡出这类动作用同一套时长和缓动，别到处手写 QPropertyAnimation，
也别出现"有的地方 100ms、有的地方 400ms、有的地方干脆没动画"的割裂感。

    from src.app.animations import expand_height, collapse_height, fade

约定：
    * 时长：FAST=110ms（悬停/小状态）、NORMAL=190ms（展开收起）、SLOW=280ms（页面级）
    * 缓动：进入用 OutCubic（快起慢停），退出用 InCubic（慢起快停）—— 与 Fluent 观感一致
    * 动画对象挂在 widget 上（setParent），避免被 GC 掉导致动画"偶尔不播"
"""

from __future__ import annotations

from typing import Callable, Optional

from PySide6.QtCore import QEasingCurve, QObject, QPropertyAnimation, Qt
from PySide6.QtWidgets import QGraphicsOpacityEffect, QWidget

__all__ = ["FAST", "NORMAL", "SLOW", "expand_height", "collapse_height", "fade", "animate"]

FAST = 110
NORMAL = 190
SLOW = 280


def animate(widget: QWidget, prop: bytes, start, end, duration: int = NORMAL,
            curve: QEasingCurve.Type = QEasingCurve.Type.OutCubic,
            on_finished: Optional[Callable] = None) -> QPropertyAnimation:
    """给任意属性做一段动画。动画对象挂在 widget 上，防止被回收。"""
    animation = QPropertyAnimation(widget, prop, widget)
    animation.setDuration(int(duration))
    animation.setStartValue(start)
    animation.setEndValue(end)
    animation.setEasingCurve(curve)
    if on_finished is not None:
        animation.finished.connect(on_finished)
    existing = getattr(widget, "_sxcl_animation", None)
    if isinstance(existing, QPropertyAnimation) and existing.state() == QPropertyAnimation.State.Running:
        existing.stop()                  # 连点两次不会打架
    widget._sxcl_animation = animation
    animation.start(QPropertyAnimation.DeletionPolicy.KeepWhenStopped)
    return animation


def expand_height(widget: QWidget, duration: int = NORMAL,
                  on_finished: Optional[Callable] = None) -> QPropertyAnimation:
    """展开：把 maximumHeight 从当前值动画到内容需要的高度。"""
    target = max(widget.sizeHint().height(), widget.minimumSizeHint().height(), 1)
    widget.setVisible(True)
    return animate(widget, b"maximumHeight", max(0, widget.maximumHeight() if widget.maximumHeight() < 16777215 else 0),
                   target, duration, QEasingCurve.Type.OutCubic, on_finished)


def collapse_height(widget: QWidget, duration: int = NORMAL,
                    on_finished: Optional[Callable] = None) -> QPropertyAnimation:
    """收起：maximumHeight 动画到 0，结束后隐藏（并恢复 maximumHeight，免得下次展不开）。"""
    def _done() -> None:
        widget.setVisible(False)
        widget.setMaximumHeight(16777215)
        if on_finished is not None:
            on_finished()

    return animate(widget, b"maximumHeight", widget.height(), 0, duration,
                   QEasingCurve.Type.InCubic, _done)


def fade(widget: QWidget, start: float, end: float, duration: int = FAST) -> QPropertyAnimation:
    """透明度动画（自动挂一个 QGraphicsOpacityEffect）。"""
    effect = widget.graphicsEffect()
    if not isinstance(effect, QGraphicsOpacityEffect):
        effect = QGraphicsOpacityEffect(widget)
        widget.setGraphicsEffect(effect)
    effect.setOpacity(start)
    animation = QPropertyAnimation(effect, b"opacity", widget)
    animation.setDuration(int(duration))
    animation.setStartValue(start)
    animation.setEndValue(end)
    animation.setEasingCurve(QEasingCurve.Type.OutCubic)
    animation.start(QPropertyAnimation.DeletionPolicy.DeleteWhenStopped)
    return animation
