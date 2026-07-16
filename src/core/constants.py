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
