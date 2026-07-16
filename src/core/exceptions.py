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
"""Custom exception hierarchy for Silent X Craft Launcher."""

from __future__ import annotations


class SXCLError(Exception):
    """Base exception for all launcher errors."""


# ── Configuration ─────────────────────────────────────────────────


class ConfigError(SXCLError):
    """Raised when configuration loading or validation fails."""


class ConfigNotFoundError(ConfigError):
    """Raised when a required configuration file is missing."""


# ── Network / Download ────────────────────────────────────────────


class NetworkError(SXCLError):
    """Raised when a network operation fails."""


class DownloadError(NetworkError):
    """Raised when a file download fails."""


class DownloadCancelledError(DownloadError):
    """Raised when the user cancels a download."""


class ChecksumMismatchError(DownloadError):
    """Raised when a downloaded file's hash does not match."""


# ── Java ──────────────────────────────────────────────────────────


class JavaError(SXCLError):
    """Raised when a Java-related operation fails."""


class JavaNotFoundError(JavaError):
    """Raised when no suitable Java runtime is found."""


class JavaVersionError(JavaError):
    """Raised when the Java version is incompatible."""


# ── Installation ──────────────────────────────────────────────────


class InstallationError(SXCLError):
    """Raised when game installation fails."""


class LoaderInstallationError(InstallationError):
    """Raised when mod loader installation fails."""


class VersionNotFoundError(InstallationError):
    """Raised when a requested Minecraft version is not found."""


# ── Launch ────────────────────────────────────────────────────────


class LaunchError(SXCLError):
    """Raised when game launch fails."""
