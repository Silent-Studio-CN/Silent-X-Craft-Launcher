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
"""Custom setting card for Java runtime selection."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import QFileDialog, QHBoxLayout, QLabel, QVBoxLayout
from qfluentwidgets import CaptionLabel, ComboBox, PushButton, SettingCard, FluentIcon as FIF

from src.app.common.java_finder import JavaInstallation, discover_java_installations, inspect_java
from src.app.common.platform import is_windows, java_executable_name


class JavaSettingCard(SettingCard):
    selectionChanged = Signal(str)

    def __init__(self, parent=None) -> None:
        super().__init__(
            FIF.DEVELOPER_TOOLS,
            "Java 运行路径",
            "选择用于启动 Minecraft 的 Java 运行时",
            parent,
        )
        self.setFixedHeight(96)
        self._installations: list[JavaInstallation] = []

        self.javaCombo = ComboBox(self)
        self.javaCombo.setMinimumWidth(320)
        self.importButton = PushButton("导入", self)
        self.statusLabel = CaptionLabel("", self)
        self.statusLabel.setTextColor("#52c41a", "#73d13d")

        right_layout = QVBoxLayout()
        right_layout.setSpacing(6)
        right_layout.setContentsMargins(0, 0, 0, 0)

        top_row = QHBoxLayout()
        top_row.setSpacing(8)
        top_row.addWidget(self.javaCombo)
        top_row.addWidget(self.importButton)
        top_row.setAlignment(Qt.AlignmentFlag.AlignRight)

        right_layout.addLayout(top_row)
        right_layout.addWidget(self.statusLabel, 0, Qt.AlignmentFlag.AlignRight)

        self.hBoxLayout.addLayout(right_layout, 0)
        self.hBoxLayout.addSpacing(16)

        self.importButton.clicked.connect(self._import_java)
        self.javaCombo.currentIndexChanged.connect(self._on_selection_changed)

        self.refresh()

    def refresh(self, preferred_path: str = "") -> None:
        self._installations = discover_java_installations()
        self.javaCombo.blockSignals(True)
        self.javaCombo.clear()

        if not self._installations:
            self.javaCombo.addItem("未检测到 Java，请手动导入")
            self.statusLabel.setText("未找到可用的 Java 运行时")
            self.statusLabel.setTextColor("#fa8c16", "#ffa940")
            self.javaCombo.blockSignals(False)
            return

        for install in self._installations:
            self.javaCombo.addItem(install.display_name, userData=str(install.path))

        selected_index = 0
        if preferred_path:
            for index in range(self.javaCombo.count()):
                if self.javaCombo.itemData(index) == preferred_path:
                    selected_index = index
                    break
        else:
            # 自动选择推荐的 Java
            from src.app.services.java_service import JavaService
            best = JavaService.find_best("", self._installations)
            if best:
                for index in range(self.javaCombo.count()):
                    if self.javaCombo.itemData(index) == str(best.path):
                        selected_index = index
                        break

        self.javaCombo.setCurrentIndex(selected_index)
        self.javaCombo.blockSignals(False)
        if self._installations:
            self._update_status(self._installations[selected_index])

    def selected_path(self) -> str:
        if self.javaCombo.count() == 0:
            return ""
        data = self.javaCombo.currentData()
        return str(data) if data else ""

    def _on_selection_changed(self, index: int) -> None:
        if index < 0 or index >= len(self._installations):
            return
        install = self._installations[index]
        self._update_status(install)
        self.selectionChanged.emit(str(install.path))

    def _update_status(self, install: JavaInstallation) -> None:
        if install.compatible:
            self.statusLabel.setText(f"✓ {install.compatibility_label}")
            self.statusLabel.setTextColor("#52c41a", "#73d13d")
        else:
            self.statusLabel.setText(f"✗ {install.compatibility_label}")
            self.statusLabel.setTextColor("#ff4d4f", "#ff7875")

    def _import_java(self) -> None:
        file_filter = "Java 可执行文件 (java.exe)" if is_windows() else "Java 可执行文件 (java)"
        path, _ = QFileDialog.getOpenFileName(
            self,
            "选择 Java 可执行文件",
            "",
            file_filter,
        )
        if not path:
            return

        install = inspect_java(Path(path))
        if not install:
            self.statusLabel.setText("无法识别所选 Java 运行时")
            self.statusLabel.setTextColor("#ff4d4f", "#ff7875")
            return

        existing_paths = {str(item.path) for item in self._installations}
        if str(install.path) not in existing_paths:
            self._installations.insert(0, install)

        self.refresh(preferred_path=str(install.path))
        self.selectionChanged.emit(str(install.path))
