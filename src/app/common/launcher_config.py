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
"""Persistent launcher settings backed by QFluentWidgets qconfig."""

from __future__ import annotations

from enum import Enum
from pathlib import Path
from typing import Any

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

    @property
    def display(self) -> str:
        return {
            "zh-CN": "简体中文",
            "en-US": "English",
        }.get(self.value, self.value)


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


# ── Config items that should be serialised (lowercase = actual attr names) ──
_SAVED_KEYS = {
    "language", "downloadSource", "versionRefreshInterval",
    "countryCode",
    "javaPath", "maxMemoryMb", "windowSize", "gameDirectory", "versionIsolation",
    "username",
    "debugMode", "useDownloadEngine",
    "autoCheckUpdate",
}


def load_config() -> None:
    qconfig.file = get_config_path()
    if qconfig.file.exists():
        qconfig.load()
        _restore_config()
    else:
        # 首次启动：根据国家代码设置默认语言
        _apply_language_from_country()


def save_config() -> None:
    """Persist all custom config items to the JSON file.

    QFluentWidgets' ``qconfig.save()`` only saves items registered on the
    **global** ``qconfig`` instance (Theme/Font/etc.), NOT items on our
    ``cfg`` (LauncherConfig) instance.  This function serialises both.
    """
    import json
    path = get_config_path()

    # Load existing (QFluentWidgets' own items)
    existing: dict = {}
    if path.exists():
        existing = json.loads(path.read_text(encoding="utf-8"))

    # Merge our custom items group by group
    for group_name in ("General", "Game", "Advanced"):
        group: dict = existing.setdefault(group_name, {})
        for attr_name in dir(cfg):
            if attr_name.startswith("_"):
                continue
            cls_attr = getattr(type(cfg), attr_name, None)
            if isinstance(cls_attr, ConfigItem) and cls_attr.group == group_name:
                if attr_name in _SAVED_KEYS:
                    item = getattr(cfg, attr_name)
                    val = item.value if hasattr(item, 'value') else item
                    if isinstance(val, Enum):
                        val = val.value
                    group[attr_name] = val

    path.write_text(json.dumps(existing, indent=4, ensure_ascii=False), encoding="utf-8")


def _restore_config():
    """Restore custom config items from JSON into ``cfg`` after ``load_config``.

    ``qconfig.load()`` only restores QFluentWidgets' own items.  Our custom
    items (Language, JavaPath, …) need explicit deserialisation.
    """
    path = get_config_path()
    if not path.exists():
        return
    import json
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return

    # Map section → item name → LauncherConfig attribute name
    for group_name in ("General", "Game", "Advanced"):
        group_data = data.get(group_name, {})
        for attr_name in dir(cfg):
            if attr_name.startswith("_"):
                continue
            cls_attr = getattr(type(cfg), attr_name, None)
            if isinstance(cls_attr, ConfigItem) and cls_attr.group == group_name:
                raw = group_data.get(attr_name)
                if raw is not None:
                    try:
                        item = getattr(cfg, attr_name)
                        # Deserialize via serializer if available
                        serializer = getattr(item, 'serializer', None)
                        if serializer and hasattr(serializer, 'deserialize'):
                            value = serializer.deserialize(raw)
                        else:
                            value = raw
                        qconfig.set(item, value)
                    except Exception:
                        pass


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