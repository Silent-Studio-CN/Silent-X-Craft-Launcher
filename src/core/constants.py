"""Application-wide constants and enumerations."""

from __future__ import annotations

from enum import Enum


# ── Application Info ──────────────────────────────────────────────

APP_NAME = "Silent X Craft Launcher"
APP_VERSION = "0.1.0"
ORGANIZATION = "SilentXCraft"
APP_REPO_URL = "https://github.com/"

# ── Minecraft URLs ────────────────────────────────────────────────

MOJANG_VERSION_MANIFEST_URL = (
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
)
BMCLAPI_VERSION_MANIFEST_URL = (
    "https://bmclapi2.bangbang93.com/mc/game/version_manifest_v2.json"
)

# ── Download Sources ──────────────────────────────────────────────


class DownloadSource(Enum):
    MOJANG = "mojang"
    BMCLAPI = "bmclapi"

    @property
    def label(self) -> str:
        return {
            DownloadSource.MOJANG: "Mojang 官方源",
            DownloadSource.BMCLAPI: "BMCLAPI 镜像源",
        }[self]

    @property
    def manifest_url(self) -> str:
        return {
            DownloadSource.MOJANG: MOJANG_VERSION_MANIFEST_URL,
            DownloadSource.BMCLAPI: BMCLAPI_VERSION_MANIFEST_URL,
        }[self]


# ── Version Types ─────────────────────────────────────────────────


class VersionType(Enum):
    ALL = "all"
    RELEASE = "release"
    SNAPSHOT = "snapshot"
    OLD = "old_beta"


# ── Language ──────────────────────────────────────────────────────


class LauncherLanguage(Enum):
    ZH_CN = "zh-CN"
    EN_US = "en-US"


# ── Window Sizes ──────────────────────────────────────────────────


class WindowSizePreset(Enum):
    SIZE_854x480 = "854x480"
    SIZE_1280x720 = "1280x720"
    SIZE_1600x900 = "1600x900"
    SIZE_1920x1080 = "1920x1080"
    FULLSCREEN = "全屏"


# ── Refresh Intervals ─────────────────────────────────────────────


class RefreshInterval(Enum):
    INTERVAL_30 = 30
    INTERVAL_60 = 60
    INTERVAL_120 = 120
    INTERVAL_300 = 300
    INTERVAL_600 = 600
    INTERVAL_1800 = 1800

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
