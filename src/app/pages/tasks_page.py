# 版权所有 © Silent X Craft Launcher Dev 开发团队
#
# Silent X Craft Launcher (SXCL) 是一款由 Silent X Craft Launcher Dev 团队开发，
# 隶属于 SilentCodeTeams 旗下，并由 SilentStudio 管理的 Minecraft 第三方启动器。
#
# Copyright © Silent X Craft Launcher Development Team
#
# Silent X Craft Launcher (SXCL) is a third-party Minecraft launcher developed
# by the Silent X Craft Launcher Dev team, operating under the management of
# SilentCodeTeams, and overseen by SilentStudio.
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
"""Tasks page — session-aware task monitor with status icons."""

from __future__ import annotations

from PySide6.QtCore import Qt, Signal, QTimer
from PySide6.QtWidgets import (
    QHBoxLayout, QVBoxLayout, QWidget, QPushButton,
)
from qfluentwidgets import (
    BodyLabel, ProgressBar, StrongBodyLabel,
)

from src.app.common.base_page import BasePage


class _StatusIcon(QWidget):
    """任务状态图标：进行中 = 旋转，完成 = ✓，失败 = ✕，悬停显示 × 删除."""

    STATE_RUNNING = 0
    STATE_DONE = 1
    STATE_FAILED = 2

    def __init__(self, parent=None):
        super().__init__(parent)
        self._state = self.STATE_RUNNING
        self._spin = 0
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)

        self.setFixedSize(24, 24)
        self._spin_chars = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]

        self._label = BodyLabel("⠋", self)
        self._label.setAlignment(Qt.AlignCenter)
        self._label.setStyleSheet("font-size: 16px;")
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self._label)

    def set_state(self, state: int):
        self._state = state
        if state == self.STATE_RUNNING:
            self._timer.start(120)
            self._label.setStyleSheet("color: #1890ff; font-size: 16px;")
            self.setCursor(Qt.ArrowCursor)
        elif state == self.STATE_DONE:
            self._timer.stop()
            self._label.setText("✓")
            self._label.setStyleSheet("color: #52c41a; font-size: 16px; font-weight: bold;")
            self.setCursor(Qt.ArrowCursor)
        elif state == self.STATE_FAILED:
            self._timer.stop()
            self._label.setText("✕")
            self._label.setStyleSheet("color: #ff4d4f; font-size: 16px; font-weight: bold;")
            self.setCursor(Qt.PointingHandCursor)

    def _tick(self):
        self._spin = (self._spin + 1) % len(self._spin_chars)
        self._label.setText(self._spin_chars[self._spin])


class _DeleteBtn(QPushButton):
    """鼠标悬停时出现的红色 × 删除按钮."""

    def __init__(self, parent=None):
        super().__init__("×", parent)
        self.setFixedSize(20, 20)
        self.setVisible(False)
        self.setStyleSheet("""
            QPushButton { border: none; color: #ff4d4f; font-size: 18px; font-weight: bold;
                          background: transparent; }
            QPushButton:hover { color: #ff0000; }
        """)


class TaskCard(QWidget):
    """A single task item showing download/launch progress."""

    clicked = Signal(object)
    delete_requested = Signal(str)

    def __init__(self, task_id: str, title: str, parent=None):
        super().__init__(parent)
        self.task_id = task_id

        self.setFixedHeight(80)
        self.setAttribute(Qt.WA_StyledBackground, True)
        self.setCursor(Qt.PointingHandCursor)
        self.setStyleSheet("""
            TaskCard {
                background: rgba(128, 128, 128, 0.06);
                border-radius: 8px;
                margin: 2px 0;
            }
            TaskCard:hover {
                background: rgba(128, 128, 128, 0.12);
            }
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(16, 12, 16, 12)

        # Row 1: icon + title + status + delete
        row1 = QHBoxLayout()
        self.icon = _StatusIcon(self)
        row1.addWidget(self.icon)
        row1.addSpacing(8)
        self.title_label = StrongBodyLabel(title, self)
        row1.addWidget(self.title_label)
        row1.addStretch()
        self.status_label = BodyLabel("准备中", self)
        self.status_label.setStyleSheet("color: #888;")
        row1.addWidget(self.status_label)
        row1.addSpacing(4)
        self.delete_btn = _DeleteBtn(self)
        self.delete_btn.clicked.connect(lambda: self.delete_requested.emit(self.task_id))
        row1.addWidget(self.delete_btn)
        layout.addLayout(row1)

        # Row 2: progress bar
        self.progress_bar = ProgressBar(self)
        self.progress_bar.setFixedHeight(4)
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        layout.addWidget(self.progress_bar)

        # Row 3: detail text
        self.detail_label = BodyLabel("", self)
        self.detail_label.setStyleSheet("color: #999; font-size: 12px;")
        layout.addWidget(self.detail_label)

        # Hover → show delete button (only for done/failed)
        self._hover_shows_delete = False

    def update_progress(self, value: int, status: str, detail: str = ""):
        self.progress_bar.setValue(value)
        self.status_label.setText(status)
        if detail:
            self.detail_label.setText(detail)

    def set_running(self):
        self.icon.set_state(_StatusIcon.STATE_RUNNING)
        self._hover_shows_delete = False
        self.delete_btn.setVisible(False)

    def set_done(self):
        self.icon.set_state(_StatusIcon.STATE_DONE)
        self._hover_shows_delete = False
        self.progress_bar.setValue(100)
        self.delete_btn.setVisible(False)

    def set_failed(self):
        self.icon.set_state(_StatusIcon.STATE_FAILED)
        self._hover_shows_delete = True

    def mousePressEvent(self, event):
        self.clicked.emit(self)

    def enterEvent(self, event):
        if self._hover_shows_delete:
            self.delete_btn.setVisible(True)
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.delete_btn.setVisible(False)
        super().leaveEvent(event)


class TasksPage(BasePage):
    """任务页面 — 显示下载、启动等正在进行的任务."""

    def __init__(self, parent=None):
        super().__init__(title="任务", subtitle="查看和管理正在进行的操作", parent=parent)
        self._tasks: dict[str, TaskCard] = {}
        self._build_content()

    def _build_content(self):
        self._empty_label = BodyLabel("暂无进行中的任务", self.view)
        self._empty_label.setAlignment(Qt.AlignCenter)
        self._empty_label.setStyleSheet("color: #888; font-size: 14px; margin: 60px 0;")
        self.add_content(self._empty_label)

        self._task_container = QWidget(self.view)
        self._task_layout = QVBoxLayout(self._task_container)
        self._task_layout.setContentsMargins(0, 0, 0, 0)
        self._task_layout.setAlignment(Qt.AlignTop)
        self._task_container.setVisible(False)
        self.add_content(self._task_container)

        self.add_stretch()

    # ── Public API ────────────────────────────────────────────

    def add_or_update_task(
        self, task_id: str, title: str,
        progress: int = 0, status: str = "准备中", detail: str = "",
    ) -> TaskCard:
        if task_id in self._tasks:
            card = self._tasks[task_id]
            card.update_progress(progress, status, detail)
            return card

        card = TaskCard(task_id, title, self._task_container)
        card.update_progress(progress, status, detail)
        card.clicked.connect(lambda c: self._on_task_clicked(c))
        card.delete_requested.connect(self.remove_task)
        self._tasks[task_id] = card
        self._task_layout.addWidget(card)
        self._refresh_visibility()
        return card

    def remove_task(self, task_id: str):
        card = self._tasks.pop(task_id, None)
        if card:
            self._task_layout.removeWidget(card)
            card.deleteLater()
            self._refresh_visibility()

    def update_task(
        self, task_id: str, progress: int = 0,
        status: str = "", detail: str = "",
    ):
        card = self._tasks.get(task_id)
        if card:
            card.update_progress(progress, status, detail)

    def set_task_running(self, task_id: str):
        card = self._tasks.get(task_id)
        if card:
            card.set_running()

    def set_task_done(self, task_id: str):
        card = self._tasks.get(task_id)
        if card:
            card.set_done()
        # 完成后 3 秒自动移除
        QTimer.singleShot(3000, lambda: self.remove_task(task_id))

    def set_task_failed(self, task_id: str):
        card = self._tasks.get(task_id)
        if card:
            card.set_failed()

    def _refresh_visibility(self):
        has = len(self._tasks) > 0
        self._empty_label.setVisible(not has)
        self._task_container.setVisible(has)

    def _on_task_clicked(self, card: TaskCard):
        mw = self.window()
        if hasattr(mw, 'navigate_to_task'):
            mw.navigate_to_task(card.task_id)
