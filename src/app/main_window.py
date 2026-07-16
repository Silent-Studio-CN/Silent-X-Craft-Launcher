# Silent X Craft Launcher (SXCL)
# Copyright (C) SilentStudio / SilentCodeTeams / Silent X Craft Launcher
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
"""Main window — navigation and page management."""

import sys
import importlib
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
from src.app.common.config_manager import config as config_manager

# 只导入实际存在的页面（不导入 VersionsPage）
from src.app.pages.home_page import HomePage
from src.app.pages.settings_page import SettingsPage


class MainWindow(FluentWindow):
    """主窗口 — 管理所有页面和导航"""

    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"{APP_NAME} {APP_VERSION}")

        # 窗口尺寸
        self.resize(1100, 750)
        self.setMinimumSize(900, 600)
        
        # 下载配置页引用
        self._download_config_page = None
        self._download_progress_page = None
        # 启动页引用
        self._launch_page = None
        
        # 初始化页面
        self._init_pages()

        # 设置导航
        self._init_navigation()

        # 应用配置
        self._apply_config()

        # 应用全局样式
        self._apply_global_style()

        # 主题变化时更新样式
        cfg.themeMode.valueChanged.connect(self._on_theme_changed)

        # 检查更新（异步）
        QTimer.singleShot(3000, self._check_updates)

    def _init_pages(self):
        """创建所有页面实例"""
        self.home_page = HomePage(self)
        
        # 动态加载 VersionsPage，强制使用最新版本
        self._load_versions_page()
        
        self.settings_page = SettingsPage(self)

    def _load_versions_page(self):
        """动态加载 VersionsPage，确保使用最新代码"""
        # 彻底清除缓存
        if 'src.app.pages.versions_page' in sys.modules:
            del sys.modules['src.app.pages.versions_page']
            print("✅ 已从 sys.modules 中删除 versions_page")
        
        # 重新导入
        from src.app.pages.versions_page import VersionsPage
        self.versions_page = VersionsPage(self)
        print("✅ VersionsPage 实例创建完成")

    def _init_navigation(self):
        """配置导航栏"""
        # 主页
        self.addSubInterface(self.home_page, FIF.HOME, "主页")

        # 游戏版本
        self.addSubInterface(self.versions_page, FIF.GAME, "版本")

        # 设置（底部）
        self.addSubInterface(
            self.settings_page,
            FIF.SETTING,
            "设置",
            NavigationItemPosition.BOTTOM,
        )

        # 默认选中主页
        self.navigationInterface.setCurrentItem(self.home_page.objectName())

    def _apply_global_style(self):
        """应用全局样式表 — 使导航栏与内容区背景统一"""
        self._update_global_style()

    def _update_global_style(self):
        """根据当前主题更新全局样式 — 仅设置内容区域背景，不干预 QFluentWidgets 内部主题。"""
        dark = isDarkTheme()
        bg = "#1e1e1e" if dark else "#f5f5f5"

        # 只设置内容区域的背景色，不重写 CardWidget/QScrollArea 等 QFluentWidgets 内部组件
        self.setStyleSheet(f"""
            FluentWindow {{
                background-color: {bg};
            }}
        """)

    def _on_theme_changed(self, theme):
        """主题变更时更新全局样式"""
        self._update_global_style()

    def _apply_config(self):
        """应用配置到窗口"""
        # 从配置管理器读取主题
        theme = config_manager.get("theme", "auto")
        if theme == "light":
            qconfig.set(cfg.themeMode, Theme.LIGHT)
        elif theme == "dark":
            qconfig.set(cfg.themeMode, Theme.DARK)
        else:
            qconfig.set(cfg.themeMode, Theme.AUTO)

    def _check_updates(self):
        """检查更新（占位）"""
        pass

    # ================================================================
    # 页面切换
    # ================================================================

    def switch_to_download_config(self, version):
        """切换到下载配置页面"""
        print(f"[MainWindow] 切换到下载配置: {version.id}")
        from src.app.pages.download_config_page import DownloadConfigPage
        
        # 如果已存在，移除旧的
        if self._download_config_page is not None:
            try:
                idx = self.stackedWidget.indexOf(self._download_config_page)
                if idx >= 0:
                    self.stackedWidget.removeWidget(self._download_config_page)
                self._download_config_page.deleteLater()
            except:
                pass
            self._download_config_page = None
        
        # 创建新的下载配置页
        self._download_config_page = DownloadConfigPage(version, self)
        self.stackedWidget.addWidget(self._download_config_page)
        self.stackedWidget.setCurrentWidget(self._download_config_page)
        
        # 清除导航高亮（因为是临时页面）
        self.navigationInterface.setCurrentItem(None)

    def switch_to_download_progress(
        self,
        version,
        version_name: str,
        loader_type: str = "none",
        loader_version: str = None
    ):
        """切换到下载进度页"""
        from src.app.pages.download_progress_page import DownloadProgressPage
        
        # 移除旧的进度页
        if self._download_progress_page is not None:
            try:
                idx = self.stackedWidget.indexOf(self._download_progress_page)
                if idx >= 0:
                    self.stackedWidget.removeWidget(self._download_progress_page)
                self._download_progress_page.deleteLater()
            except:
                pass
            self._download_progress_page = None
        
        # 创建新的进度页
        self._download_progress_page = DownloadProgressPage(
            version=version,
            version_name=version_name,
            loader_type=loader_type,
            loader_version=loader_version,
            parent=self,
        )
        self.stackedWidget.addWidget(self._download_progress_page)
        self.stackedWidget.setCurrentWidget(self._download_progress_page)
        self.navigationInterface.setCurrentItem(None)

    def go_back_to_versions(self):
        """返回版本列表 - 同时清除下载配置页和进度页"""
        # 移除进度页
        if self._download_progress_page is not None:
            try:
                idx = self.stackedWidget.indexOf(self._download_progress_page)
                if idx >= 0:
                    self.stackedWidget.removeWidget(self._download_progress_page)
                self._download_progress_page.deleteLater()
            except:
                pass
            self._download_progress_page = None
        
        # 移除下载配置页
        if self._download_config_page is not None:
            try:
                idx = self.stackedWidget.indexOf(self._download_config_page)
                if idx >= 0:
                    self.stackedWidget.removeWidget(self._download_config_page)
                self._download_config_page.deleteLater()
            except:
                pass
            self._download_config_page = None
        
        self.switchTo(self.versions_page)
        self.navigationInterface.setCurrentItem(self.versions_page.objectName())

    # ================================================================
    # 启动流程
    # ================================================================

    def switch_to_launch(self, version):
        """切换到启动进度页"""
        from src.app.pages.launch_page import LaunchProgressPage

        # 移除旧的启动页
        if self._launch_page is not None:
            try:
                idx = self.stackedWidget.indexOf(self._launch_page)
                if idx >= 0:
                    self.stackedWidget.removeWidget(self._launch_page)
                self._launch_page.deleteLater()
            except Exception:
                pass
            self._launch_page = None

        self._launch_page = LaunchProgressPage(version, self)
        self.stackedWidget.addWidget(self._launch_page)
        self.stackedWidget.setCurrentWidget(self._launch_page)
        self.navigationInterface.setCurrentItem(None)

    def go_back_from_launch(self):
        """从启动页返回版本列表，同时清除临时页面"""
        if self._launch_page is not None:
            try:
                idx = self.stackedWidget.indexOf(self._launch_page)
                if idx >= 0:
                    self.stackedWidget.removeWidget(self._launch_page)
                self._launch_page.deleteLater()
            except Exception:
                pass
            self._launch_page = None

        self.switchTo(self.versions_page)
        self.navigationInterface.setCurrentItem(self.versions_page.objectName())

    # ================================================================
    # 下载源变更通知
    # ================================================================

    def on_download_source_changed(self, source):
        """下载源变更时的回调"""
        print(f"[MainWindow] 下载源变更为: {source.value}")


def create_splash(window: MainWindow):
    """创建启动画面（占位，暂时返回 None）"""
    return None