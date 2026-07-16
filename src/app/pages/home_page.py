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
"""Home page — show installed versions and launch."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QHBoxLayout, QVBoxLayout, QWidget, QLineEdit
from qfluentwidgets import (
    BodyLabel,
    CardWidget,
    IconWidget,
    PrimaryPushButton,
    PushButton,
    StrongBodyLabel,
    InfoBar,
    InfoBarPosition,
    ComboBox,
)

from src.app.common.base_page import BasePage
from src.app.common.config import APP_NAME, APP_VERSION
from src.app.common.launcher_config import cfg
from src.app.services.download_service import get_installed_versions
from src.app.services.version_manifest import GameVersion


class HomePage(BasePage):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(
            title="欢迎回来",
            subtitle=f"{APP_NAME} v{APP_VERSION} — 轻量、优雅的 Minecraft 启动器",
            parent=parent,
        )
        self._installed_versions = []
        self._selected_version = None
        self._build_content()
        self._load_versions()
    
    def _build_content(self) -> None:
        # ---- 用户信息 ----
        user_card = CardWidget(self.view)
        user_layout = QHBoxLayout(user_card)
        user_layout.setContentsMargins(20, 16, 20, 16)
        user_layout.setSpacing(16)
        
        avatar = IconWidget(":/qfluentwidgets/images/logo.png", user_card)
        avatar.setFixedSize(48, 48)
        user_layout.addWidget(avatar)
        
        user_info_layout = QVBoxLayout()
        user_info_layout.setSpacing(4)
        
        self.username_input = QLineEdit(user_card)
        self.username_input.setPlaceholderText("输入游戏内名称")
        self.username_input.setText("Player")
        user_info_layout.addWidget(self.username_input)
        
        status_label = BodyLabel("离线模式", user_card)
        status_label.setTextColor("#888888", "#888888")
        user_info_layout.addWidget(status_label)
        
        user_layout.addLayout(user_info_layout, 1)
        
        login_btn = PushButton("正版登录 (即将支持)", user_card)
        login_btn.setEnabled(False)
        user_layout.addWidget(login_btn)
        
        self.add_content(user_card)
        
        # ---- 版本选择 ----
        version_card = CardWidget(self.view)
        version_layout = QHBoxLayout(version_card)
        version_layout.setContentsMargins(20, 16, 20, 16)
        version_layout.setSpacing(16)
        
        version_label = StrongBodyLabel("游戏版本", version_card)
        version_layout.addWidget(version_label)
        
        self.version_combo = ComboBox(version_card)
        self.version_combo.setMinimumWidth(200)
        self.version_combo.currentTextChanged.connect(self._on_version_selected)
        version_layout.addWidget(self.version_combo)
        
        version_layout.addStretch(1)
        
        self.launch_btn = PrimaryPushButton("启动游戏", version_card)
        self.launch_btn.clicked.connect(self._on_launch)
        self.launch_btn.setEnabled(False)
        version_layout.addWidget(self.launch_btn)
        
        self.add_content(version_card)
        
        # ---- 快速操作 ----
        quick_card = CardWidget(self.view)
        quick_layout = QHBoxLayout(quick_card)
        quick_layout.setContentsMargins(20, 16, 20, 16)
        quick_layout.setSpacing(12)
        
        versions_btn = PushButton("浏览版本", quick_card)
        versions_btn.clicked.connect(lambda: self.window().switchTo(self.window().versions_page))
        quick_layout.addWidget(versions_btn)
        
        settings_btn = PushButton("设置", quick_card)
        settings_btn.clicked.connect(lambda: self.window().switchTo(self.window().settings_page))
        quick_layout.addWidget(settings_btn)
        
        quick_layout.addStretch(1)
        
        self.add_content(quick_card)
        self.add_stretch()
    
    def _load_versions(self):
        self._installed_versions = get_installed_versions()
        self.version_combo.clear()
        
        if self._installed_versions:
            for v in self._installed_versions:
                self.version_combo.addItem(v)
            self.version_combo.setCurrentIndex(0)
            self._selected_version = self._installed_versions[0]
            self.launch_btn.setEnabled(True)
        else:
            self.version_combo.addItem("未安装任何版本")
            self.launch_btn.setEnabled(False)
    
    def _on_version_selected(self, text: str):
        self._selected_version = text
    
    def _on_launch(self):
        """启动游戏 - 跳转到启动进度页"""
        if not self._selected_version:
            InfoBar.warning(
                title="未选择版本",
                content="请先选择要启动的游戏版本",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=3000,
                parent=self,
            )
            return
        
        # 检查 Java 是否设置
        if not cfg.javaPath.value:
            InfoBar.warning(
                title="未选择 Java",
                content="请先在设置中选择 Java 运行时",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=4000,
                parent=self,
            )
            return
        
        # 构造 GameVersion 对象
        version_obj = GameVersion(
            id=self._selected_version,
            version_type="release",
            url="",
            release_time=""
        )
        
        # 跳转到启动进度页
        main_window = self.window()
        if hasattr(main_window, 'switch_to_launch'):
            main_window.switch_to_launch(version_obj)
        else:
            InfoBar.error(
                title="错误",
                content="版本页面未初始化",
                orient=InfoBarPosition.TOP,
                isClosable=True,
                duration=3000,
                parent=self,
            )