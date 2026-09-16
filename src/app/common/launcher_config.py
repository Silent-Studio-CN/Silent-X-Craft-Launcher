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

from src.core.constants import DownloadSource
from src.core.logger import log
from src.core.platform import default_config_directory, default_game_directory


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
    # 注意：主题（themeMode / themeColor）**不要**在这里重定义 ——
    # 它们是 QFluentWidgets 的内置项（qconfig 的 QFluentWidgets 组），
    # 重定义会出现两套主题状态，切换时界面半明半暗。
    autoCheckUpdate = ConfigItem("General", "AutoCheckUpdate", True, BoolValidator())
    
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
        DownloadSource.AUTO,          # 智能：按实测速度自动选路
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

    # Download（自研异步下载引擎）
    maxConnections = RangeConfigItem("Download", "MaxConnections", 32, RangeValidator(4, 128))
    speedLimitKbps = RangeConfigItem("Download", "SpeedLimitKbps", 0, RangeValidator(0, 1048576))
    verifySha1 = ConfigItem("Download", "VerifySha1", True, BoolValidator())


cfg = LauncherConfig()

# 尽早就把"库保存会冲掉我们设置"这道口子堵上（load_config 里还会再兜一次）
_QCONFIG_SAVE_GUARD_INSTALLED = False


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
    # 下载引擎
    "maxConnections", "speedLimitKbps", "verifySha1",
}

# 允许写入 config.json 的分组（QFluentWidgets 自己的分组不在这里）
# "System" 是后来加的：countryCode 原本在 System 组里，不在名单里就永远存不下来
_SAVED_GROUPS = ("General", "Game", "Advanced", "Download", "System")


def _guard_qconfig_save() -> None:
    """给 qconfig.save 套一层：库保存完自己那份后，我们立刻把自定义分组补回文件。

    起因（实测）：QFluentWidgets 保存自己的配置（主题/字体/主题色）时会**整份重写**
    config.json，而我们的自定义项（Java 路径、游戏目录、下载设置…）不在 qconfig 的
    注册表里，于是它每存一次，用户设置就全没了 —— 表现就是"设置重启后失效"。
    这里不跟它抢，只在它写完的瞬间把我们的分组再并回去。
    """
    original = getattr(qconfig, "save", None)
    if original is None or getattr(original, "_sxcl_guarded", False):
        return

    def guarded(*args, **kwargs):
        result = original(*args, **kwargs)
        try:
            save_config()
        except Exception as exc:          # 配置补写失败不该影响主题切换
            log.debug("补写自定义配置失败: %s", exc)
        return result

    guarded._sxcl_guarded = True          # type: ignore[attr-defined]
    qconfig.save = guarded                # type: ignore[method-assign]


def load_config() -> None:
    qconfig.file = get_config_path()
    _guard_qconfig_save()
    if qconfig.file.exists():
        qconfig.load()
        _restore_config()
        _migrate_legacy_theme()
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
    for group_name in _SAVED_GROUPS:
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
    for group_name in _SAVED_GROUPS:
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


def _migrate_legacy_theme() -> None:
    """配置兼容修复（只做一次）：旧 ThemeMode 搬迁 + 老"诡异"强调色纠正。"""
    try:
        import json
        path = get_config_path()
        if not path.exists():
            _fix_legacy_accent()
            return
        data = json.loads(path.read_text(encoding="utf-8"))
        legacy = (data.get("General") or {}).pop("ThemeMode", None)
        if legacy is not None:
            for mode in (Theme.LIGHT, Theme.DARK, Theme.AUTO):
                if str(legacy).lower() in (mode.value.lower(), mode.name.lower()):
                    qconfig.set(qconfig.themeMode, mode)
                    log.info("已迁移旧主题设置: %s", legacy)
                    break
            path.write_text(json.dumps(data, indent=4, ensure_ascii=False), encoding="utf-8")
    except Exception as exc:
        log.debug("主题迁移跳过: %s", exc)
    _fix_legacy_accent()


# 历史遗留的"诡异"强调色（8 位带 alpha 的青绿），统一换成 Fluent 默认蓝
_LEGACY_ACCENTS = {"#009faa"}


def _fix_legacy_accent() -> None:
    try:
        from PySide6.QtGui import QColor
        current = qconfig.themeColor.value
        if isinstance(current, QColor) and current.name() in _LEGACY_ACCENTS:
            from qfluentwidgets import setThemeColor
            setThemeColor(QColor("#0067c0"))
            log.info("已修正历史默认主题色: %s -> #0067c0", current.name())
    except Exception as exc:
        log.debug("主题色修正跳过: %s", exc)
    except Exception as exc:
        log.debug("主题迁移跳过: %s", exc)


def _detect_country_code() -> str:
    """从系统区域猜国家/地区（猜不出来按 CN，因为默认语言是中文）。"""
    try:
        from PySide6.QtCore import QLocale
        name = QLocale.system().name().replace("-", "_")     # zh_CN / en_US
        for part in reversed(name.split("_")):
            if len(part) == 2 and part.isalpha():
                return part.upper()
    except Exception as exc:
        log.debug("国家/地区检测失败: %s", exc)
    return "CN"


def _apply_country_preset() -> None:
    """首次启动：按国家/地区给一套合理默认值（语言 + 下载源 + 国家码）。

    * 中国：默认智能下载源（会实测并记住谁快，国内镜像通常快得多）；
    * 其它地区：默认官方源 —— 让海外用户先绕一圈 BMCLAPI 是很蠢的默认值。
    """
    code = (cfg.countryCode.value or "").strip() or _detect_country_code()
    qconfig.set(cfg.countryCode, code)
    if code.upper() == "CN":
        qconfig.set(cfg.language, LauncherLanguage.ZH_CN)
        qconfig.set(cfg.downloadSource, DownloadSource.AUTO)
    else:
        qconfig.set(cfg.language, LauncherLanguage.EN_US)
        qconfig.set(cfg.downloadSource, DownloadSource.OFFICIAL)
    log.info("首次启动预设: 国家/地区=%s 语言=%s 下载源=%s", code,
             cfg.language.value.value, cfg.downloadSource.value.value)
    save_config()


def _apply_language_from_country() -> None:
    """保留旧名字（历史调用点用得到），实际走国家预设。"""
    _apply_country_preset()


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