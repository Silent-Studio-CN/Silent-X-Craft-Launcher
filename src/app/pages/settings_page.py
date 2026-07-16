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
"""Settings page matching Fluent launcher reference layout."""

from __future__ import annotations

import psutil
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QFileDialog, QHBoxLayout, QSlider, QSpinBox
from qfluentwidgets import (
    ComboBoxSettingCard,
    FluentIcon as FIF,
    HyperlinkCard,
    InfoBar,
    InfoBarPosition,
    PushSettingCard,
    RangeValidator,
    SettingCard,
    SettingCardGroup,
    SwitchSettingCard,
    setTheme,
    qconfig,
    Theme,
)

from src.app.common.config import APP_NAME, APP_VERSION, DownloadSource
from src.app.common.launcher_config import (
    LauncherLanguage,
    WindowSizePreset,
    cfg,
    theme_labels,
    save_config,
)
from src.app.common.base_page import BasePage
from src.app.common.platform import default_game_directory
from src.app.widgets.java_setting_card import JavaSettingCard


class MemorySettingCard(SettingCard):
    """内存设置卡片 - 滑块+输入框右对齐"""
    
    def __init__(
        self,
        configItem,
        icon,
        title,
        content=None,
        parent=None,
        min_value=2048,
        max_value=16384,
        step=2048,
    ):
        super().__init__(icon, title, content, parent)
        
        self.configItem = configItem
        self._step = step
        self._min = min_value
        self._max = max_value
        
        self.right_widget = QHBoxLayout()
        self.right_widget.setSpacing(8)
        self.right_widget.setContentsMargins(0, 0, 20, 0)
        
        self.slider = QSlider(self)
        self.slider.setOrientation(Qt.Horizontal)
        self.slider.setRange(min_value, max_value)
        self.slider.setSingleStep(step)
        self.slider.setPageStep(step)
        self.slider.setFixedWidth(180)
        self.slider.setValue(configItem.value)
        
        self.spin_box = QSpinBox(self)
        self.spin_box.setRange(min_value, max_value)
        self.spin_box.setSingleStep(step)
        self.spin_box.setSuffix(" MB")
        self.spin_box.setFixedWidth(110)
        self.spin_box.setValue(configItem.value)
        
        self.right_widget.addWidget(self.slider)
        self.right_widget.addWidget(self.spin_box)
        
        self.hBoxLayout.addStretch(1)
        self.hBoxLayout.addLayout(self.right_widget)
        
        self.slider.valueChanged.connect(self._on_slider_changed)
        self.spin_box.valueChanged.connect(self._on_spinbox_changed)
        configItem.valueChanged.connect(self._on_config_changed)
    
    def _on_slider_changed(self, value: int) -> None:
        aligned = ((value + self._step // 2) // self._step) * self._step
        aligned = max(self._min, min(aligned, self._max))
        
        if aligned != value:
            self.slider.blockSignals(True)
            self.slider.setValue(aligned)
            self.slider.blockSignals(False)
            value = aligned
        
        self.spin_box.blockSignals(True)
        self.spin_box.setValue(value)
        self.spin_box.blockSignals(False)
        
        qconfig.set(self.configItem, value)
    
    def _on_spinbox_changed(self, value: int) -> None:
        aligned = ((value + self._step // 2) // self._step) * self._step
        aligned = max(self._min, min(aligned, self._max))
        
        if aligned != value:
            self.spin_box.blockSignals(True)
            self.spin_box.setValue(aligned)
            self.spin_box.blockSignals(False)
            value = aligned
        
        self.slider.blockSignals(True)
        self.slider.setValue(value)
        self.slider.blockSignals(False)
        
        qconfig.set(self.configItem, value)
    
    def _on_config_changed(self, value: int) -> None:
        self.slider.blockSignals(True)
        self.slider.setValue(value)
        self.slider.blockSignals(False)
        
        self.spin_box.blockSignals(True)
        self.spin_box.setValue(value)
        self.spin_box.blockSignals(False)


class SettingsPage(BasePage):
    def __init__(self, parent=None) -> None:
        super().__init__(
            title="设置",
            subtitle="",
            parent=parent,
        )
        self.subtitleLabel.hide()
        self._build_content()
        self._bind_events()

    def _build_content(self) -> None:
        # ---- 通用设置 ----
        general_group = SettingCardGroup("通用设置", self.view)

        self.update_card = SwitchSettingCard(
            FIF.UPDATE,
            "启动时自动检查更新",
            configItem=cfg.autoCheckUpdate,
            parent=general_group,
        )
        self.theme_card = ComboBoxSettingCard(
            cfg.themeMode,
            FIF.BRUSH,
            "主题模式",
            texts=theme_labels(),
            parent=general_group,
        )
        self.language_card = ComboBoxSettingCard(
            cfg.language,
            FIF.LANGUAGE,
            "语言",
            texts=["zh-CN", "en-US"],
            parent=general_group,
        )
        self.source_card = ComboBoxSettingCard(
            cfg.downloadSource,
            FIF.DOWNLOAD,
            "版本下载源",
            "选择版本清单与资源文件的下载源",
            texts=[source.label for source in DownloadSource],
            parent=general_group,
        )

        from src.app.common.launcher_config import RefreshInterval
        self.refresh_interval_card = ComboBoxSettingCard(
            cfg.versionRefreshInterval,
            FIF.UPDATE,
            "版本列表刷新频率",
            "版本清单自动刷新的间隔时间",
            texts=[interval.label for interval in RefreshInterval],
            parent=general_group,
        )
        general_group.addSettingCard(self.refresh_interval_card)

        general_group.addSettingCard(self.update_card)
        general_group.addSettingCard(self.theme_card)
        general_group.addSettingCard(self.language_card)
        general_group.addSettingCard(self.source_card)

        # ---- 游戏设置 ----
        game_group = SettingCardGroup("游戏设置", self.view)

        # 版本隔离（放在最前面）
        self.isolation_card = SwitchSettingCard(
            FIF.FOLDER,
            "版本隔离",
            "每个版本使用独立的 .minecraft 目录",
            configItem=cfg.versionIsolation,
            parent=game_group,
        )
        game_group.addSettingCard(self.isolation_card)

        self.java_card = JavaSettingCard(game_group)
        game_group.addSettingCard(self.java_card)

        # 动态计算内存范围
        total_memory_mb = int(psutil.virtual_memory().total / (1024 * 1024))
        
        min_memory = min(2048, int(total_memory_mb * 0.2))
        min_memory = ((min_memory + 2047) // 2048) * 2048
        min_memory = max(min_memory, 1024)
        
        max_memory = int(total_memory_mb * 0.75)
        max_memory = (max_memory // 2048) * 2048
        max_memory = max(max_memory, 4096)
        
        default_memory = int(total_memory_mb * 0.5)
        default_memory = (default_memory // 2048) * 2048
        default_memory = max(min_memory, min(default_memory, max_memory))
        
        cfg.maxMemoryMb.validator = RangeValidator(min_memory, max_memory)
        current = cfg.maxMemoryMb.value
        if current < min_memory or current > max_memory:
            qconfig.set(cfg.maxMemoryMb, default_memory)
            save_config()
        
        self.memory_card = MemorySettingCard(
            cfg.maxMemoryMb,
            FIF.SPEED_OFF,
            "最大内存分配",
            f"系统总内存 {total_memory_mb}MB，步进 2GB",
            parent=game_group,
            min_value=min_memory,
            max_value=max_memory,
            step=2048,
        )
        self.window_card = ComboBoxSettingCard(
            cfg.windowSize,
            FIF.FULL_SCREEN,
            "游戏窗口大小",
            texts=[preset.value for preset in WindowSizePreset],
            parent=game_group,
        )
        self.game_dir_card = PushSettingCard(
            "选择目录",
            FIF.FOLDER,
            "游戏目录",
            cfg.gameDirectory.value,
            game_group,
        )

        game_group.addSettingCard(self.memory_card)
        game_group.addSettingCard(self.window_card)
        game_group.addSettingCard(self.game_dir_card)

        # ---- 高级设置 ----
        advanced_group = SettingCardGroup("高级设置", self.view)
        self.debug_card = SwitchSettingCard(
            FIF.CODE,
            "调试模式",
            configItem=cfg.debugMode,
            parent=advanced_group,
        )
        self.download_engine_card = SwitchSettingCard(
            FIF.SPEED_OFF,
            "多线程下载引擎-测试",
            "启用后使用多线程分片下载，可大幅提升下载速度；如果遇到安装问题可关闭此开关",
            configItem=cfg.useDownloadEngine,
            parent=advanced_group,
        )
        self.reset_card = PushSettingCard(
            "重置所有设置",
            FIF.CANCEL,
            "重置配置",
            "将所有设置恢复为默认值",
            advanced_group,
        )
        advanced_group.addSettingCard(self.debug_card)
        advanced_group.addSettingCard(self.download_engine_card)
        advanced_group.addSettingCard(self.reset_card)

        # ---- 关于 ----
        about_group = SettingCardGroup("关于", self.view)
        self.about_card = SettingCard(
            FIF.INFO,
            f"{APP_NAME} {APP_VERSION}",
            "基于 PySide6 与 QFluentWidgets 构建，支持 Windows / macOS / Linux",
            about_group,
        )
        self.website_card = HyperlinkCard(
            "https://github.com/",
            "访问官网",
            FIF.LINK,
            "项目主页",
            parent=about_group,
        )
        about_group.addSettingCard(self.about_card)
        about_group.addSettingCard(self.website_card)

        self.add_content(general_group)
        self.add_content(game_group)
        self.add_content(advanced_group)
        self.add_content(about_group)
        self.add_stretch()

        preferred_java = cfg.javaPath.value
        if preferred_java:
            self.java_card.refresh(preferred_path=preferred_java)
        else:
            self.java_card.refresh()

    def _bind_events(self) -> None:
        self.java_card.selectionChanged.connect(self._on_java_changed)
        self.game_dir_card.clicked.connect(self._pick_game_directory)
        self.reset_card.clicked.connect(self._reset_settings)
        cfg.themeMode.valueChanged.connect(self._on_theme_changed)
        cfg.downloadSource.valueChanged.connect(self._on_download_source_changed)
        cfg.versionIsolation.valueChanged.connect(self._on_isolation_changed)

    def _on_java_changed(self, path: str) -> None:
        qconfig.set(cfg.javaPath, path)
        save_config()

    def _pick_game_directory(self) -> None:
        path = QFileDialog.getExistingDirectory(
            self,
            "选择 Minecraft 游戏目录",
            cfg.gameDirectory.value,
        )
        if path:
            qconfig.set(cfg.gameDirectory, path)
            self.game_dir_card.setContent(path)
            save_config()

    def _reset_settings(self) -> None:
        qconfig.set(cfg.autoCheckUpdate, True)
        qconfig.set(cfg.themeMode, Theme.AUTO)
        qconfig.set(cfg.language, LauncherLanguage.ZH_CN)
        qconfig.set(cfg.downloadSource, DownloadSource.BMCLAPI)
        qconfig.set(cfg.javaPath, "")
        qconfig.set(cfg.versionIsolation, False)
        qconfig.set(cfg.useDownloadEngine, True)
        
        total_memory_mb = int(psutil.virtual_memory().total / (1024 * 1024))
        default_memory = int(total_memory_mb * 0.5)
        default_memory = (default_memory // 2048) * 2048
        min_memory = min(2048, int(total_memory_mb * 0.2))
        min_memory = ((min_memory + 2047) // 2048) * 2048
        max_memory = int(total_memory_mb * 0.75)
        max_memory = (max_memory // 2048) * 2048
        max_memory = max(max_memory, 4096)
        default_memory = max(min_memory, min(default_memory, max_memory))
        qconfig.set(cfg.maxMemoryMb, default_memory)
        
        qconfig.set(cfg.windowSize, WindowSizePreset.SIZE_1280x720)
        qconfig.set(cfg.gameDirectory, str(default_game_directory()))
        qconfig.set(cfg.debugMode, False)

        save_config()

        self.game_dir_card.setContent(cfg.gameDirectory.value)
        self.java_card.refresh()
        self.theme_card.setValue(cfg.themeMode.value)
        self.language_card.setValue(cfg.language.value)
        self.source_card.setValue(cfg.downloadSource.value)
        self.window_card.setValue(cfg.windowSize.value)
        self._on_theme_changed(cfg.themeMode.value)

    def _on_theme_changed(self, theme) -> None:
        setTheme(theme)

    def _on_download_source_changed(self, source: DownloadSource) -> None:
        from src.app.common.config_manager import config
        config.set('download_source', source.value)
        window = self.window()
        if hasattr(window, "on_download_source_changed"):
            window.on_download_source_changed(source)

    def _on_isolation_changed(self, value: bool) -> None:
        """版本隔离变更"""
        from src.app.common.config_manager import config
        qconfig.set(cfg.versionIsolation, value)
        config.set('version_isolation', value)
        save_config()