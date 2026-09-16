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

from PySide6.QtCore import Qt, QThread, Signal
from PySide6.QtWidgets import QFileDialog, QHBoxLayout, QLabel, QVBoxLayout
from qfluentwidgets import (
    CaptionLabel, ComboBox, PushButton, SettingCard, RoundMenu, Action,
    FluentIcon as FIF,
)

from src.core.logger import log
from src.services.java.mojang_runtime import COMPONENT_PREVIEW, install_runtime

from src.services.java.finder import JavaInstallation, discover_java_installations, inspect_java
from src.core.platform import is_windows, java_executable_name


class JavaDownloadWorker(QThread):
    """后台下载官方 JRE（Mojang java-runtime，逐文件 SHA1 校验）。"""

    status = Signal(str)
    finished_ok = Signal(bool, str)

    def __init__(self, component: str, parent=None):
        super().__init__(parent)
        self.component = component
        self._cancel = None

    def run(self):
        try:
            import threading
            from src.core.download import ProgressInfo, default_engine, limiter

            self._cancel = threading.Event()
            last = {"t": 0.0}

            def on_progress(p: ProgressInfo):
                import time as _t
                now = _t.time()
                if now - last["t"] < 0.3:
                    return
                last["t"] = now
                self.status.emit(
                    f"下载 Java: {p.files_done}/{p.files_total} 个文件 "
                    f"| {p.bytes_done / 1024 / 1024:.1f}/{p.bytes_total / 1024 / 1024:.1f} MB "
                    f"| {p.speed_bps / 1024 / 1024:.1f} MB/s | {p.current[:40]}")

            java_home = install_runtime(
                component=self.component,
                on_status=self.status.emit,
                on_progress=on_progress,
                cancel=self._cancel,
            )
            if java_home:
                self.finished_ok.emit(True, str(java_home))
            else:
                self.finished_ok.emit(False, "下载失败，详见日志")
        except Exception as exc:  # noqa: BLE001
            log.exception("Java 下载失败")
            self.finished_ok.emit(False, str(exc))

    def cancel(self):
        if self._cancel is not None:
            self._cancel.set()


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
        self.downloadButton = PushButton("下载 Java", self)
        self._download_worker = None
        self.statusLabel = CaptionLabel("", self)
        self.statusLabel.setTextColor("#52c41a", "#73d13d")

        right_layout = QVBoxLayout()
        right_layout.setSpacing(6)
        right_layout.setContentsMargins(0, 0, 0, 0)

        top_row = QHBoxLayout()
        top_row.setSpacing(8)
        top_row.addWidget(self.javaCombo)
        top_row.addWidget(self.downloadButton)
        top_row.addWidget(self.importButton)
        top_row.setAlignment(Qt.AlignmentFlag.AlignRight)

        right_layout.addLayout(top_row)
        right_layout.addWidget(self.statusLabel, 0, Qt.AlignmentFlag.AlignRight)

        self.hBoxLayout.addLayout(right_layout, 0)
        self.hBoxLayout.addSpacing(16)

        self.importButton.clicked.connect(self._import_java)
        self.downloadButton.clicked.connect(self._show_download_menu)
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
            # 路径比较要归一化：Windows 短名（HITEVI~1）与长名是同一个目录
            wanted = self._norm(preferred_path)
            for index in range(self.javaCombo.count()):
                if self._norm(str(self.javaCombo.itemData(index) or "")) == wanted:
                    selected_index = index
                    break
        else:
            # 自动选择推荐的 Java
            from src.services.java.finder import best_java_installation
            best = best_java_installation(self._installations)
            if best:
                for index in range(self.javaCombo.count()):
                    if self.javaCombo.itemData(index) == str(best.path):
                        selected_index = index
                        break

        self.javaCombo.setCurrentIndex(selected_index)
        self.javaCombo.blockSignals(False)
        if self._installations:
            self._update_status(self._installations[selected_index])

    @staticmethod
    def _norm(path: str) -> str:
        """归一化路径用于比较（解析短名/符号链接/大小写）。"""
        if not path:
            return ""
        try:
            return str(Path(path).resolve()).lower()
        except Exception:
            return path.lower()

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
        from src.app.theme import token
        if install.compatible:
            self.statusLabel.setText(f"✓ {install.compatibility_label}")
            self.statusLabel.setTextColor(token("success"))
        else:
            self.statusLabel.setText(f"✗ {install.compatibility_label}")
            self.statusLabel.setTextColor(token("danger"))
        self._status_kind = "ok" if install.compatible else "bad"

    # ── 官方 JRE 下载 ────────────────────────────────────────────

    def _show_download_menu(self) -> None:
        menu = RoundMenu(parent=self)
        menu.addAction(Action(FIF.DOWNLOAD, "自动（按最新正式版选择）",
                              triggered=lambda: self._start_download("")))
        menu.addSeparator()
        for label, component in COMPONENT_PREVIEW:
            menu.addAction(Action(FIF.DOWNLOAD, label,
                                  triggered=lambda _=False, c=component: self._start_download(c)))
        menu.exec(self.downloadButton.mapToGlobal(self.downloadButton.rect().bottomLeft()))

    def _start_download(self, component: str) -> None:
        if self._download_worker is not None and self._download_worker.isRunning():
            return
        self.downloadButton.setEnabled(False)
        self.downloadButton.setText("下载中…")
        self.statusLabel.setText("正在获取官方 JRE 清单…")
        self.statusLabel.setTextColor("#0078d4", "#00bcf2")

        self._download_worker = JavaDownloadWorker(component, self)
        self._download_worker.status.connect(self._on_download_status)
        self._download_worker.finished_ok.connect(self._on_download_finished)
        self._download_worker.start()

    def _on_download_status(self, text: str) -> None:
        self.statusLabel.setText(text[:120])

    def _on_download_finished(self, ok: bool, info: str) -> None:
        self.downloadButton.setEnabled(True)
        self.downloadButton.setText("下载 Java")
        if ok:
            self.statusLabel.setText("✅ 已安装官方 JRE")
            self.statusLabel.setTextColor("#52c41a", "#73d13d")
            java_bin = Path(info) / "bin" / java_executable_name()
            self.refresh(preferred_path=str(java_bin))
            self.selectionChanged.emit(str(java_bin))
        else:
            self.statusLabel.setText(f"❌ {info[:80]}")
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
