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
"""Main window — session-aware navigation with persistent page state."""

from __future__ import annotations

import sys
from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QLabel
from qfluentwidgets import (
    FluentWindow,
    NavigationItemPosition,
    InfoBar,
    InfoBarPosition,
    Theme,
    qconfig,
    isDarkTheme,
)
from qfluentwidgets import FluentIcon as FIF

from src.app.common.config import APP_NAME, APP_VERSION
from src.app.common.launcher_config import cfg


class MainWindow(FluentWindow):
    """主窗口 — 会话保持，页面切换不丢失状态."""

    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"{APP_NAME} {APP_VERSION}")
        self.resize(1100, 750)
        self.setMinimumSize(900, 600)

        # ── 会话页面池（页面创建后一直存活，不被销毁） ──
        self._session_pages: dict[str, QWidget] = {}
        # 当前激活的"临时页面"（配置页 / 进度页 / 启动页）
        self._active_temp_page: QWidget | None = None
        self._temp_page_key: str | None = None  # 对应 _session_pages 的 key
        self._session_active = False             # 用户未主动关闭临时页面时保持 True
        # 之前是哪个导航项（切回时恢复）
        self._last_nav_item: str | None = None

        self._init_pages()
        self._init_navigation()

        self._apply_global_style()
        cfg.themeMode.valueChanged.connect(self._on_theme_changed)
        QTimer.singleShot(3000, self._check_updates)

    # ──────────────────────────────────────────────────────────
    # 页面初始化
    # ──────────────────────────────────────────────────────────

    def _init_pages(self):
        """创建所有常驻页面（只在启动时创建一次）."""
        from src.app.pages.home_page import HomePage
        from src.app.pages.versions_page import VersionsPage
        from src.app.pages.tasks_page import TasksPage
        from src.app.pages.settings_page import SettingsPage

        self.home_page = HomePage(self)
        self.versions_page = VersionsPage(self)
        self.tasks_page = TasksPage(self)
        self.settings_page = SettingsPage(self)

        self._session_pages = {
            "home": self.home_page,
            "versions": self.versions_page,
            "tasks": self.tasks_page,
            "settings": self.settings_page,
        }

    def _init_navigation(self):
        self.addSubInterface(self.home_page, FIF.HOME, "主页")
        self.addSubInterface(self.versions_page, FIF.GAME, "版本")
        self.addSubInterface(self.tasks_page, FIF.UPDATE, "任务")

        self.addSubInterface(
            self.settings_page,
            FIF.SETTING,
            "设置",
            NavigationItemPosition.BOTTOM,
        )

        self.navigationInterface.setCurrentItem(self.home_page.objectName())
        self.navigationInterface.currentItemChanged.connect(self._on_nav_changed)

    # ──────────────────────────────────────────────────────────
    # 导航事件 — 恢复/隐藏临时页面
    # ──────────────────────────────────────────────────────────

    def _on_nav_changed(self, item):
        """导航切换时，自动恢复会话状态."""
        nav_name = item.routeKey() if item else ""
        if not nav_name:
            return

        # 切换到"版本"时，如果会话未关闭，恢复临时页面
        if nav_name == "versions" and self._session_active and self._active_temp_page:
            self.stackedWidget.setCurrentWidget(self._active_temp_page)
            self.navigationInterface.setCurrentItem(None)
            return

        # 离开时记录导航项
        self._last_nav_item = nav_name

    # ──────────────────────────────────────────────────────────
    # 会话保持 — 临时页面管理
    # ──────────────────────────────────────────────────────────

    def _show_temp_page(self, page: QWidget, key: str = ""):
        """显示一个临时页面（配置/进度/启动页），保持其在后台存活."""
        if self._active_temp_page is not None and self._active_temp_page is not page:
            self._active_temp_page.setParent(None)
            self.stackedWidget.removeWidget(self._active_temp_page)

        if self.stackedWidget.indexOf(page) < 0:
            self.stackedWidget.addWidget(page)

        self._active_temp_page = page
        self._temp_page_key = key
        self._session_active = True
        self.stackedWidget.setCurrentWidget(page)
        self.navigationInterface.setCurrentItem(None)

    def _hide_temp_page(self, end_session: bool = False):
        """隐藏当前临时页面，回到导航项. end_session=True 时结束会话."""
        if self._active_temp_page is None:
            return
        if end_session:
            self._session_active = False

        target = self._last_nav_item or "versions"
        for name, page in self._session_pages.items():
            if name == target:
                self.switchTo(page)
                self.navigationInterface.setCurrentItem(page.objectName())
                return
        self.switchTo(self.versions_page)
        self.navigationInterface.setCurrentItem(self.versions_page.objectName())

    # ──────────────────────────────────────────────────────────
    # 页面切换 API
    # ──────────────────────────────────────────────────────────

    def switch_to_download_config(self, version):
        """切换到下载配置页（会话保持：只创建一次，切换不丢失状态）."""
        from src.app.pages.download_config_page import DownloadConfigPage

        key = f"download_config_{id(version)}"
        if key in self._session_pages:
            page = self._session_pages[key]
        else:
            page = DownloadConfigPage(version, self)
            self._session_pages[key] = page

        self._show_temp_page(page, key=key)

    def switch_to_download_progress(
        self, version, version_name: str,
        loader_type: str = "none", loader_version: str = None,
    ):
        """切换到下载进度页."""
        from src.app.pages.download_progress_page import DownloadProgressPage

        key = f"download_progress_{version_name}"
        if key in self._session_pages:
            page = self._session_pages[key]
        else:
            page = DownloadProgressPage(
                version=version, version_name=version_name,
                loader_type=loader_type, loader_version=loader_version,
                parent=self,
            )
            self._session_pages[key] = page

        # 在任务页注册
        self.tasks_page.add_or_update_task(
            task_id=key, title=f"下载 {version_name}",
            progress=0, status="准备中",
        )

        self._show_temp_page(page, key=key)

    def switch_to_launch(self, version):
        """切换到启动进度页."""
        from src.app.pages.launch_page import LaunchProgressPage

        key = f"launch_{version.id}"
        if key in self._session_pages:
            page = self._session_pages[key]
        else:
            page = LaunchProgressPage(version, self)
            self._session_pages[key] = page

        self.tasks_page.add_or_update_task(
            task_id=key, title=f"启动 {version.id}",
            progress=0, status="启动中",
        )

        self._show_temp_page(page, key=key)

    def go_back_to_versions(self):
        """返回版本列表，结束会话."""
        self._hide_temp_page(end_session=True)

    def go_back_from_launch(self):
        """从启动页返回，结束会话."""
        self._hide_temp_page(end_session=True)

    def navigate_to_task(self, task_id: str):
        """任务页点击任务卡片时的回调 — 导航到对应的临时页面."""
        page = self._session_pages.get(task_id)
        if page:
            self._show_temp_page(page)

    def on_download_source_changed(self, source):
        pass

    # ──────────────────────────────────────────────────────────
    # 样式
    # ──────────────────────────────────────────────────────────

    def _apply_global_style(self):
        self._update_global_style()

    def _update_global_style(self):
        dark = isDarkTheme()
        bg = "#1e1e1e" if dark else "#f5f5f5"
        self.setStyleSheet(f"""
            FluentWindow {{
                background-color: {bg};
            }}
        """)

    def _on_theme_changed(self, theme):
        self._update_global_style()

    def _check_updates(self):
        pass
