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
"""Cross-platform detection and path resolution.

Centralises all sys.platform / os.environ / platform.machine()
checks so the rest of the codebase never queries the OS directly.
"""

from __future__ import annotations

import os
import shutil
import sys
import platform as _platform
from enum import Enum
from pathlib import Path


# ── OS & Architecture Enums ───────────────────────────────────────


class PlatformType(Enum):
    WINDOWS = "windows"
    MACOS = "macos"
    LINUX = "linux"


class ArchType(Enum):
    X86_64 = "x86_64"
    X86 = "x86"
    ARM64 = "arm64"
    ARM = "arm"
    UNKNOWN = "unknown"


# ── Detection ─────────────────────────────────────────────────────


def current_platform() -> PlatformType:
    """Detect the current operating system."""
    if sys.platform.startswith("win"):
        return PlatformType.WINDOWS
    if sys.platform.startswith("darwin"):
        return PlatformType.MACOS
    return PlatformType.LINUX


def current_arch() -> ArchType:
    """Detect the current CPU architecture."""
    mach = _platform.machine().lower()
    if mach in ("amd64", "x86_64"):
        return ArchType.X86_64
    if mach in ("x86", "i386", "i686"):
        return ArchType.X86
    if mach in ("arm64", "aarch64"):
        return ArchType.ARM64
    if mach.startswith("arm"):
        return ArchType.ARM
    return ArchType.UNKNOWN


def is_windows() -> bool:
    return current_platform() == PlatformType.WINDOWS


def is_macos() -> bool:
    return current_platform() == PlatformType.MACOS


def is_linux() -> bool:
    return current_platform() == PlatformType.LINUX


def is_arm64() -> bool:
    return current_arch() == ArchType.ARM64


# ── Platform-specific Names ───────────────────────────────────────


def java_executable_name() -> str:
    """Return the Java executable filename for the current OS."""
    return "java.exe" if is_windows() else "java"


def classpath_separator() -> str:
    """Return the classpath separator (`;` on Windows, `:` otherwise)."""
    return ";" if is_windows() else ":"


def create_no_window_flag():
    """Return ``subprocess.CREATE_NO_WINDOW`` on Windows, 0 otherwise."""
    if is_windows():
        import subprocess
        return getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)
    return 0


# ── File-system Helpers ───────────────────────────────────────────


def normalize_path(path: str | Path) -> Path:
    """Resolve a path string or Path to an absolute, expanded Path."""
    return Path(path).expanduser().resolve()


def find_on_path(name: str) -> Path | None:
    """Search ``PATH`` for an executable named *name*."""
    found = shutil.which(name)
    return Path(found).resolve() if found else None


# ── Default Directory Paths ───────────────────────────────────────


def default_config_directory(app_name: str = "SilentXCraftLauncher") -> Path:
    """Return the OS-specific configuration directory for *app_name*."""
    plat = current_platform()
    if plat == PlatformType.WINDOWS:
        return Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming")) / app_name
    if plat == PlatformType.MACOS:
        return Path.home() / "Library" / "Application Support" / app_name
    # Linux
    xdg = os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config"))
    return Path(xdg) / app_name.lower().replace(" ", "-")


def default_log_directory(app_name: str = "SilentXCraftLauncher") -> Path:
    """Return the OS-specific log directory."""
    return default_config_directory(app_name) / "logs"


def default_game_directory() -> Path:
    """Return the default Minecraft game directory (~/.minecraft)."""
    return Path.home() / ".minecraft"


def default_jvm_directory() -> Path | None:
    """Return the default JVM installation directory for the current OS, or None."""
    plat = current_platform()
    if plat == PlatformType.WINDOWS:
        return Path(os.environ.get("ProgramFiles", "C:\\Program Files")) / "Java"
    if plat == PlatformType.MACOS:
        return Path("/Library/Java/JavaVirtualMachines")
    return Path("/usr/lib/jvm")
