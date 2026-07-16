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
"""Home page — installed versions browser with launch support."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QHBoxLayout, QVBoxLayout, QWidget, QPushButton, QFileDialog,
)
from qfluentwidgets import (
    BodyLabel, CardWidget, PrimaryPushButton, PushButton,
    InfoBar, InfoBarPosition, ComboBox,
)

from src.app.common.base_page import BasePage
from src.app.common.config import APP_NAME, APP_VERSION
from src.app.common.launcher_config import cfg
from src.app.services.download_service import get_installed_versions, get_version_info
from src.app.services.version_manifest import GameVersion


class HomePage(BasePage):
    """主页 — 已安装版本列表 + 快速启动."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(
            title="主页",
            subtitle=f"{APP_NAME} v{APP_VERSION}",
            parent=parent,
        )
        self._installed: list[str] = []
        self._build_content()
        self._refresh_installed()
        # 每 8 秒检测文件变化
        self._watch = QTimer(self)
        self._watch.setInterval(8000)
        self._watch.timeout.connect(self._check_fs)
        self._watch.start()

    def _build_content(self) -> None:
        # ── 游戏目录 ──
        dir_card = CardWidget(self.view)
        dir_layout = QHBoxLayout(dir_card)
        dir_layout.setContentsMargins(20, 12, 20, 12)

        dir_label = BodyLabel("游戏目录", dir_card)
        dir_layout.addWidget(dir_label)

        self.dir_display = BodyLabel(str(cfg.gameDirectory.value), dir_card)
        self.dir_display.setTextColor("#888888", "#888888")
        dir_layout.addWidget(self.dir_display, 1)

        change_dir_btn = PushButton("更改", dir_card)
        change_dir_btn.clicked.connect(self._change_directory)
        dir_layout.addWidget(change_dir_btn)

        open_dir_btn = PushButton("打开", dir_card)
        open_dir_btn.clicked.connect(self._open_directory)
        dir_layout.addWidget(open_dir_btn)

        self.add_content(dir_card)

        # ── 已安装版本（卡片网格） ──
        self._grid = QWidget(self.view)
        self._grid_layout = QVBoxLayout(self._grid)
        self._grid_layout.setContentsMargins(0, 0, 0, 0)
        self._grid_layout.setSpacing(8)

        self._empty_hint = BodyLabel("暂无已安装的版本\n前往「版本」页下载", self.view)
        self._empty_hint.setAlignment(Qt.AlignCenter)
        self._empty_hint.setStyleSheet("color: #888; font-size: 14px; margin: 60px 0;")

        self.add_content(self._empty_hint)
        self.add_content(self._grid)
        self.add_stretch()

    # ── 版本刷新 ────────────────────────────────────────────

    def _refresh_installed(self):
        """刷新已安装版本列表."""
        self._installed = get_installed_versions()
        self._render_cards()

    def _render_cards(self):
        """根据 _installed 重建版本卡片."""
        # 清除旧卡片
        while self._grid_layout.count():
            item = self._grid_layout.takeAt(0)
            if item and item.widget():
                item.widget().deleteLater()

        if not self._installed:
            self._empty_hint.setVisible(True)
            self._grid.setVisible(False)
            return

        self._empty_hint.setVisible(False)
        self._grid.setVisible(True)

        for vid in self._installed:
            card = self._create_version_card(vid)
            self._grid_layout.addWidget(card)

    def _create_version_card(self, version_id: str) -> CardWidget:
        card = CardWidget(self._grid)
        card.setFixedHeight(56)
        card.setCursor(Qt.PointingHandCursor)

        layout = QHBoxLayout(card)
        layout.setContentsMargins(20, 0, 16, 0)
        layout.setSpacing(16)

        # 版本名
        name = BodyLabel(version_id, card)
        name.setStyleSheet("font-size: 15px; font-weight: 600;")
        layout.addWidget(name)

        # 版本信息
        info = self._get_version_info(version_id)
        if info:
            detail = BodyLabel(info, card)
            detail.setTextColor("#888888", "#888888")
            detail.setStyleSheet("font-size: 12px;")
            layout.addWidget(detail)

        layout.addStretch()

        # 启动按钮
        launch_btn = PrimaryPushButton("启动", card)
        launch_btn.setFixedSize(80, 32)
        launch_btn.clicked.connect(lambda checked, v=version_id: self._launch(v))
        layout.addWidget(launch_btn)

        card.launch_btn = launch_btn
        return card

    def _get_version_info(self, version_id: str) -> str:
        """获取版本简要信息."""
        try:
            data = get_version_info(version_id)
            if not data:
                return ""
            players = data.get("mainClass", "")
            loader = ""
            if "forge" in version_id.lower():
                loader = "Forge"
            elif "neoforge" in version_id.lower():
                loader = "NeoForge"
            elif "fabric" in version_id.lower():
                loader = "Fabric"
            if loader:
                return loader
            return ""
        except Exception:
            return ""

    # ── 文件监控 ────────────────────────────────────────────

    def _check_fs(self):
        """检测文件系统变化（用户手动增删版本后自动刷新）."""
        current = set(get_installed_versions())
        last = set(getattr(self, "_last_snapshot", []))
        if current != last:
            self._last_snapshot = list(current)
            self._installed = list(current)
            self._render_cards()

    # ── 操作 ────────────────────────────────────────────────

    def _launch(self, version_id: str):
        """启动指定版本."""
        if not cfg.javaPath.value:
            InfoBar.warning(title="未选择 Java", content="请先在设置中选择 Java 运行时",
                            orient=InfoBarPosition.TOP, isClosable=True, duration=4000, parent=self)
            return

        v = GameVersion(id=version_id, version_type="release", url="", release_time="")
        mw = self.window()
        if hasattr(mw, 'switch_to_launch'):
            mw.switch_to_launch(v)
        else:
            InfoBar.error(title="错误", content="启动器未初始化",
                          orient=InfoBarPosition.TOP, isClosable=True, duration=3000, parent=self)

    def _change_directory(self):
        """更改游戏目录."""
        d = QFileDialog.getExistingDirectory(self, "选择 .minecraft 目录", str(cfg.gameDirectory.value))
        if d:
            from qfluentwidgets import qconfig
            qconfig.set(cfg.gameDirectory, d)
            self.dir_display.setText(d)
            self._refresh_installed()

    def _open_directory(self):
        """打开游戏目录."""
        import subprocess
        subprocess.Popen(["explorer", str(cfg.gameDirectory.value)])
