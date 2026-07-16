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
"""Tasks page — session-aware task monitor."""

from __future__ import annotations

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QHBoxLayout, QVBoxLayout, QWidget, QPushButton, QLabel,
)
from qfluentwidgets import (
    BodyLabel, CardWidget, InfoBar, InfoBarPosition,
    PrimaryPushButton, ProgressBar, StrongBodyLabel,
)

from src.app.common.base_page import BasePage


class TaskCard(QWidget):
    """A single task item showing download/launch progress."""

    clicked = Signal(object)

    def __init__(self, task_id: str, title: str, parent=None):
        super().__init__(parent)
        self.task_id = task_id
        self._title = title

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

        # Row 1: title + status
        row1 = QHBoxLayout()
        self.title_label = StrongBodyLabel(title, self)
        row1.addWidget(self.title_label)
        row1.addStretch()
        self.status_label = BodyLabel("准备中", self)
        self.status_label.setStyleSheet("color: #888;")
        row1.addWidget(self.status_label)
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

    def update_progress(self, value: int, status: str, detail: str = ""):
        self.progress_bar.setValue(value)
        self.status_label.setText(status)
        if detail:
            self.detail_label.setText(detail)

    def set_status(self, status: str, color: str = "#888"):
        self.status_label.setText(status)
        self.status_label.setStyleSheet(f"color: {color};")

    def mousePressEvent(self, event):
        self.clicked.emit(self)


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

    # ── Public API called by MainWindow ────────────────────────

    def add_or_update_task(
        self, task_id: str, title: str,
        progress: int = 0, status: str = "准备中", detail: str = "",
    ) -> TaskCard:
        """Create or update a task card."""
        if task_id in self._tasks:
            card = self._tasks[task_id]
            card.update_progress(progress, status, detail)
            return card

        card = TaskCard(task_id, title, self._task_container)
        card.update_progress(progress, status, detail)
        card.clicked.connect(lambda c: self._on_task_clicked(c))
        self._tasks[task_id] = card
        self._task_layout.addWidget(card)
        self._refresh_visibility()
        return card

    def remove_task(self, task_id: str):
        """Remove a finished task."""
        card = self._tasks.pop(task_id, None)
        if card:
            self._task_layout.removeWidget(card)
            card.deleteLater()
            self._refresh_visibility()

    def update_task(
        self, task_id: str, progress: int = 0,
        status: str = "", detail: str = "",
    ):
        """Update an existing task's progress."""
        card = self._tasks.get(task_id)
        if card:
            card.update_progress(progress, status, detail)

    def _refresh_visibility(self):
        has = len(self._tasks) > 0
        self._empty_label.setVisible(not has)
        self._task_container.setVisible(has)

    def _on_task_clicked(self, card: TaskCard):
        """When user clicks a task card, navigate to the relevant page."""
        mw = self.window()
        if not hasattr(mw, 'navigate_to_task'):
            return
        mw.navigate_to_task(card.task_id)
