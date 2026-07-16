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
"""Java version detection and Minecraft compatibility rules."""

from __future__ import annotations

import re
import subprocess
from typing import Tuple


def detect_java_version(java_path: str) -> Tuple[int, str]:
    """Detect Java version, returning (major_version, full_output)."""
    try:
        result = subprocess.run(
            [java_path, "-version"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        output = result.stderr + result.stdout
        match = re.search(r'version "(\d+)', output)
        if match:
            major = int(match.group(1))
            return major, output
        return 0, output
    except Exception as e:
        print(f"[JavaVersion] 检测失败: {e}")
        return 0, ""


def get_supported_jvm_args(java_major: int) -> list:
    """Return JVM arguments supported by the given Java major version."""
    args = [
        "--enable-native-access=ALL-UNNAMED",
        "-XX:+UnlockExperimentalVMOptions",
        "-XX:+UseG1GC",
        "-XX:G1NewSizePercent=20",
        "-XX:G1ReservePercent=20",
        "-XX:G1HeapRegionSize=32M",
        "-XX:MaxGCPauseMillis=50",
        "-XX:+PerfDisableSharedMem",
        "-XX:MinHeapFreeRatio=25",
        "-XX:MaxHeapFreeRatio=40",
        "-XX:-OmitStackTraceInFastThrow",
        "-Djdk.lang.Process.allowAmbiguousCommands=True",
        "-Dfml.ignoreInvalidMinecraftCertificates=True",
        "-Dfml.ignorePatchDiscrepancies=True",
    ]

    if java_major >= 9:
        args.append("--sun-misc-unsafe-memory-access=allow")

    if java_major >= 23:
        args.append("-XX:+UseCompactObjectHeaders")

    return args


def is_java_compatible(java_major: int, mc_version: str) -> Tuple[bool, str]:
    """Check whether *java_major* satisfies the requirements of *mc_version*."""
    required = 17  # default
    try:
        parts = mc_version.split(".")
        if len(parts) >= 2:
            if int(parts[0]) >= 26:
                required = 25
            elif int(parts[0]) >= 21:
                required = 21
            elif int(parts[0]) == 1 and int(parts[1]) >= 21:
                required = 21
            elif int(parts[0]) == 1 and int(parts[1]) >= 17:
                required = 17
    except ValueError:
        pass

    if java_major >= required:
        return True, f"Java {java_major} 兼容 (需要 Java {required}+)"
    return False, f"Java {java_major} 版本过低 (需要 Java {required}+)"
