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
"""Cross-platform Java runtime discovery.

Searches standard system locations per OS, ``JAVA_HOME``, and ``PATH``.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import time
from dataclasses import asdict, dataclass
from pathlib import Path

from src.core.platform import (
    find_on_path,
    is_linux,
    is_macos,
    is_windows,
    java_executable_name,
    normalize_path,
)


_VersionPatterns = (
    re.compile(r'version "(?P<version>[^"]+)"'),
    re.compile(r"openjdk version \"(?P<version>[^\"]+)\""),
    re.compile(r"java version \"(?P<version>[^\"]+)\""),
)


@dataclass(frozen=True)
class JavaInstallation:
    path: Path
    version: str
    major: int
    vendor: str
    compatible: bool
    compatibility_label: str

    @property
    def display_name(self) -> str:
        return f"Java {self.version} - {self.path}"

    def to_dict(self) -> dict:
        data = asdict(self)
        data["path"] = str(self.path)
        return data


def parse_java_major(version_text: str) -> int:
    """Extract the major version number from a Java version string."""
    token = version_text.strip().split(".")[0]
    if token == "1":
        parts = version_text.strip().split(".")
        return int(parts[1]) if len(parts) > 1 else 8
    m = re.match(r"\d+", token)
    return int(m.group()) if m else 0


def compatibility_for_major(major: int) -> tuple[bool, str]:
    """Return (compatible, label) for a given Java major version."""
    if major < 8:
        return False, f"Java {major} - 版本过低"
    if major >= 21:
        return True, f"Java {major} - 兼容"
    if major >= 17:
        return True, f"Java {major} - 兼容"
    return True, f"Java {major} - 可用于旧版"


def _run_java_version(java_path: Path, timeout: float = 8.0) -> tuple[str, str]:
    creationflags = 0
    if is_windows():
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    proc = subprocess.run(
        [str(java_path), "-version"],
        capture_output=True,
        text=True,
        timeout=timeout,
        creationflags=creationflags,
    )
    output = (proc.stderr or "") + (proc.stdout or "")
    return output, proc.stderr or proc.stdout or ""


def inspect_java(java_path: str | Path) -> JavaInstallation | None:
    """Run ``java -version`` and return a structured result, or *None*."""
    path = normalize_path(java_path)
    if not path.exists():
        return None

    try:
        output, _ = _run_java_version(path)
    except (OSError, subprocess.TimeoutExpired):
        return None

    version = ""
    for pattern in _VersionPatterns:
        match = pattern.search(output)
        if match:
            version = match.group("version")
            break
    if not version:
        return None

    major = parse_java_major(version)
    compatible, label = compatibility_for_major(major)
    vendor = "OpenJDK" if "openjdk" in output.lower() else "Java"
    if "temurin" in output.lower():
        vendor = "Temurin"
    elif "zulu" in output.lower():
        vendor = "Zulu"

    return JavaInstallation(
        path=path,
        version=version,
        major=major,
        vendor=vendor,
        compatible=compatible,
        compatibility_label=label,
    )


def _dedupe_installations(items: list[JavaInstallation]) -> list[JavaInstallation]:
    seen: set[str] = set()
    result: list[JavaInstallation] = []
    for item in sorted(items, key=lambda x: (-x.major, str(x.path))):
        key = str(item.path)
        if key in seen:
            continue
        seen.add(key)
        result.append(item)
    return result


def _windows_java_candidates() -> list[Path]:
    candidates: list[Path] = []
    program_files = [
        os.environ.get("ProgramFiles"),
        os.environ.get("ProgramFiles(x86)"),
        os.environ.get("LOCALAPPDATA"),
    ]
    search_roots = []
    for root in program_files:
        if root:
            search_roots.extend([
                Path(root) / "Java",
                Path(root) / "Eclipse Adoptium",
                Path(root) / "Microsoft",
                Path(root) / "Zulu",
                Path(root) / "Programs" / "Eclipse Adoptium",
            ])

    for root in search_roots:
        if not root.exists():
            continue
        for child in root.rglob(java_executable_name()):
            if not child.is_file() or _is_candidate_excluded(child):
                continue
            candidates.append(child)
    return candidates


def _macos_java_candidates() -> list[Path]:
    candidates: list[Path] = []
    jvm_root = Path("/Library/Java/JavaVirtualMachines")
    if jvm_root.exists():
        for bundle in jvm_root.glob("*.jdk"):
            candidates.append(bundle / "Contents" / "Home" / "bin" / "java")
    usr_java = Path("/usr/bin/java")
    if usr_java.exists():
        candidates.append(usr_java)
    return candidates


def _linux_java_candidates() -> list[Path]:
    candidates: list[Path] = []
    for root in (Path("/usr/lib/jvm"), Path("/usr/local/lib/jvm")):
        if root.exists():
            for child in root.rglob(java_executable_name()):
                if child.is_file():
                    candidates.append(child)

    try:
        proc = subprocess.run(
            ["update-alternatives", "--list", "java"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        if proc.returncode == 0:
            for line in proc.stdout.splitlines():
                path = Path(line.strip())
                if path.exists():
                    candidates.append(path)
    except (OSError, subprocess.TimeoutExpired, FileNotFoundError):
        pass

    usr_java = Path("/usr/bin/java")
    if usr_java.exists():
        candidates.append(usr_java)
    return candidates


def _java_home_candidates() -> list[Path]:
    java_home = os.environ.get("JAVA_HOME")
    if not java_home:
        return []
    home = Path(java_home)
    return [home / "bin" / java_executable_name(), home / "bin" / "java"]


# Paths/substrings to exclude from Java discovery (broken symlinks, Oracle javapath, etc.)
_EXCLUDE_CANDIDATE_PATTERNS = ("javapath_target", "Common Files\\Oracle", "Oracle\\Java\\javapath")


def _is_candidate_excluded(path: Path) -> bool:
    """Check if a candidate path should be excluded."""
    path_str = str(path)
    return any(p in path_str for p in _EXCLUDE_CANDIDATE_PATTERNS)


def discover_java_installations() -> list[JavaInstallation]:
    """Discover all Java installations on the system."""
    candidates: list[Path] = []
    for c in _java_home_candidates():
        if not _is_candidate_excluded(c):
            candidates.append(c)

    path_java = find_on_path(java_executable_name())
    if path_java and not _is_candidate_excluded(path_java):
        candidates.append(path_java)

    if is_windows():
        candidates.extend(_windows_java_candidates())
    elif is_macos():
        candidates.extend(_macos_java_candidates())
    elif is_linux():
        candidates.extend(_linux_java_candidates())

    installations: list[JavaInstallation] = []
    for candidate in candidates:
        resolved = candidate.resolve() if candidate.is_symlink() else candidate
        install = inspect_java(resolved)
        if install:
            installations.append(install)

    return _dedupe_installations(installations)


def recommend_java() -> JavaInstallation | None:
    """Auto-detect and return the best Java installation for the current Minecraft needs.
    
    Like PCL2's default behaviour: pick the highest compatible version
    that satisfies typical Minecraft requirements (Java 17+).
    """
    installations = discover_java_installations()
    return best_java_installation(installations)


def best_java_installation(installations: list[JavaInstallation]) -> JavaInstallation | None:
    """Return the highest-version compatible Java installation."""
    compatible = [item for item in installations if item.compatible]
    if not compatible:
        return None
    return max(compatible, key=lambda item: item.major)
