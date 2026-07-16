# Silent X Craft Launcher (SXCL)
# Copyright (C) SilentStudio / SilentCodeTeams / Silent X Craft Launcher Dev.
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
"""Persistent launcher settings backed by QFluentWidgets qconfig."""

from __future__ import annotations

from enum import Enum
from pathlib import Path

from qfluentwidgets import (
    BoolValidator,
    ConfigItem,
    EnumSerializer,
    OptionsConfigItem,
    OptionsValidator,
    QConfig,
    RangeConfigItem,
    RangeValidator,
    Theme,
    qconfig,
)

from src.app.common.config import DownloadSource
from src.app.common.platform import default_config_directory, default_game_directory


class LauncherLanguage(Enum):
    ZH_CN = "zh-CN"
    EN_US = "en-US"


class WindowSizePreset(Enum):
    SIZE_854x480 = "854x480"
    SIZE_1280x720 = "1280x720"
    SIZE_1600x900 = "1600x900"
    SIZE_1920x1080 = "1920x1080"
    FULLSCREEN = "全屏"


class RefreshInterval(Enum):
    """版本列表刷新间隔"""
    INTERVAL_30 = 30       # 30秒
    INTERVAL_60 = 60       # 1分钟
    INTERVAL_120 = 120     # 2分钟 (默认)
    INTERVAL_300 = 300     # 5分钟
    INTERVAL_600 = 600     # 10分钟
    INTERVAL_1800 = 1800   # 30分钟
    
    @property
    def label(self) -> str:
        return {
            30: "30秒",
            60: "1分钟",
            120: "2分钟",
            300: "5分钟",
            600: "10分钟",
            1800: "30分钟",
        }[self.value]
    
    @property
    def seconds(self) -> int:
        return self.value


class LauncherConfig(QConfig):
    """Application settings persisted to config.json."""

    # General
    autoCheckUpdate = ConfigItem("General", "AutoCheckUpdate", True, BoolValidator())
    
    themeMode = OptionsConfigItem(
        "General",
        "ThemeMode",
        Theme.AUTO,
        OptionsValidator(Theme),
        EnumSerializer(Theme),
    )
    
    language = OptionsConfigItem(
        "General",
        "Language",
        LauncherLanguage.ZH_CN,
        OptionsValidator(LauncherLanguage),
        EnumSerializer(LauncherLanguage),
    )
    
    downloadSource = OptionsConfigItem(
        "General",
        "DownloadSource",
        DownloadSource.BMCLAPI,
        OptionsValidator(DownloadSource),
        EnumSerializer(DownloadSource),
    )
    
    # 【新增】版本刷新间隔
    versionRefreshInterval = OptionsConfigItem(
        "General",
        "VersionRefreshInterval",
        RefreshInterval.INTERVAL_120,
        OptionsValidator(RefreshInterval),
        EnumSerializer(RefreshInterval),
    )
    
    countryCode = ConfigItem("System", "CountryCode", "CN")

    # Game
    javaPath = ConfigItem("Game", "JavaPath", "")
    maxMemoryMb = RangeConfigItem("Game", "MaxMemoryMb", 4096, RangeValidator(512, 16384))
    
    windowSize = OptionsConfigItem(
        "Game",
        "WindowSize",
        WindowSizePreset.SIZE_1280x720,
        OptionsValidator(WindowSizePreset),
        EnumSerializer(WindowSizePreset),
    )
    
    gameDirectory = ConfigItem("Game", "GameDirectory", str(default_game_directory()))
    versionIsolation = ConfigItem("Game", "VersionIsolation", False, BoolValidator())

    # Advanced
    debugMode = ConfigItem("Advanced", "DebugMode", False, BoolValidator())
    username = ConfigItem("Game", "Username", "Player")
    useDownloadEngine = ConfigItem("Advanced", "UseDownloadEngine", True, BoolValidator())


cfg = LauncherConfig()


def get_config_path() -> Path:
    config_dir = default_config_directory()
    config_dir.mkdir(parents=True, exist_ok=True)
    return config_dir / "config.json"


def load_config() -> None:
    qconfig.file = get_config_path()
    if qconfig.file.exists():
        qconfig.load()
    _apply_language_from_country()


def save_config() -> None:
    qconfig.file = get_config_path()
    qconfig.save()


def _apply_language_from_country() -> None:
    code = cfg.countryCode.value
    if code and code.upper() != "CN":
        qconfig.set(cfg.language, LauncherLanguage.EN_US)
    else:
        qconfig.set(cfg.language, LauncherLanguage.ZH_CN)


def theme_mode_label(theme: Theme) -> str:
    return {
        Theme.AUTO: "跟随系统",
        Theme.LIGHT: "浅色",
        Theme.DARK: "深色",
    }[theme]


def theme_labels() -> list[str]:
    return [theme_mode_label(Theme.AUTO), theme_mode_label(Theme.LIGHT), theme_mode_label(Theme.DARK)]


def theme_from_index(index: int) -> Theme:
    return [Theme.AUTO, Theme.LIGHT, Theme.DARK][index]


def theme_to_index(theme: Theme) -> int:
    return [Theme.AUTO, Theme.LIGHT, Theme.DARK].index(theme)