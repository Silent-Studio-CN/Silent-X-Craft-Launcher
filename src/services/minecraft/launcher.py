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
"""Minecraft launch command builder and process launcher."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Dict, List, Optional

from src.core.platform import classpath_separator, is_macos, is_linux, is_windows
from src.app.common.launcher_config import cfg
from src.services.java.finder import JavaInstallation
from src.services.java.compatibility import get_supported_jvm_args


def load_version_info(version_id: str, game_dir: Path) -> dict | None:
    """Load a version JSON and recursively resolve ``inheritsFrom``.

    Forge/Fabric/NeoForge versions store only their own libraries in
    ``libraries`` and inherit the rest from the base Minecraft version
    via the ``inheritsFrom`` field.  This function merges the full chain
    into a single dict so callers don't have to worry about inheritance.
    """
    json_path = game_dir / "versions" / version_id / f"{version_id}.json"
    if not json_path.exists():
        return None

    try:
        with open(json_path, encoding="utf-8") as f:
            info: dict = json.load(f)

        parent_id = info.get("inheritsFrom")
        if not parent_id:
            return info

        parent_info = load_version_info(parent_id, game_dir)
        if not parent_info:
            return info  # fall back to child-only

        # Merge parent + child
        merged = dict(parent_info)
        merged["id"] = info.get("id", parent_info.get("id"))
        merged["mainClass"] = info.get("mainClass", parent_info.get("mainClass", "net.minecraft.client.main.Main"))
        merged["assetIndex"] = info.get("assetIndex", parent_info.get("assetIndex", {}))
        merged["minecraftArguments"] = info.get("minecraftArguments", parent_info.get("minecraftArguments", ""))
        merged["arguments"] = info.get("arguments", parent_info.get("arguments", {}))

        # Merge libraries: parent first, then child (child dedup by path)
        child_libs: list[dict] = info.get("libraries", [])
        parent_libs: list[dict] = parent_info.get("libraries", [])
        child_paths = {
            lib.get("downloads", {}).get("artifact", {}).get("path", "")
            for lib in child_libs
        }
        for lib in parent_libs:
            path = lib.get("downloads", {}).get("artifact", {}).get("path", "")
            if path not in child_paths:
                child_libs.append(lib)

        merged["libraries"] = child_libs
        return merged

    except Exception:
        return None


def build_classpath(
    version_info: Dict,
    game_dir: Path,
    custom_name: str | None = None,
) -> tuple[list[str], str]:
    """Build the classpath entry list and return (entries, separator)."""
    cp_sep = classpath_separator()
    libraries_dir = game_dir / "libraries"
    version_id = custom_name or version_info.get("id", "")
    jar_path = game_dir / "versions" / version_id / f"{version_id}.jar"

    entries = [str(jar_path)] if jar_path.exists() else []

    for lib in version_info.get("libraries", []):
        artifact = lib.get("downloads", {}).get("artifact")
        if artifact:
            lib_path = libraries_dir / artifact.get("path", "")
            if lib_path.exists():
                entries.append(str(lib_path))

    return entries, cp_sep


def build_command(
    java_path: str,
    version_id: str,
    version_info: Dict,
    game_dir: Path,
    memory_mb: int,
    java_major: int,
    username: str = "Player",
    main_class: str | None = None,
    jvm_custom_args: list[str] | None = None,
    game_custom_args: list[str] | None = None,
) -> list[str]:
    """Build a complete launch command list."""
    version_dir = game_dir / "versions" / version_id
    natives_path = version_dir / f"{version_id}-natives"
    run_dir = version_dir if cfg.versionIsolation.value else game_dir

    classpath_entries, cp_sep = build_classpath(version_info, game_dir)
    classpath = cp_sep.join(classpath_entries)
    main_class = main_class or version_info.get("mainClass", "net.minecraft.client.main.Main")
    asset_index = version_info.get("assetIndex", {}).get("id", "")

    # JVM args
    jvm_args = [
        f"-Xmx{memory_mb}M",
        f"-Djava.library.path={natives_path}",
        "-Dminecraft.launcher.brand=SilentXCraftLauncher",
        "-Dminecraft.launcher.version=0.1.0",
        "-Dlog4j2.formatMsgNoLookups=true",
        "-Dstdout.encoding=UTF-8",
        "-Dstderr.encoding=UTF-8",
        "-Dfile.encoding=UTF-8",
    ]
    jvm_args.extend(get_supported_jvm_args(java_major))

    if jvm_custom_args:
        jvm_args.extend(jvm_custom_args)

    # Platform-specific
    if is_macos():
        jvm_args.extend(["-XstartOnFirstThread", "-Djava.awt.headless=true"])
    elif is_linux():
        jvm_args.append("-Djava.awt.headless=true")

    # Game args
    game_args = [
        "--username", username,
        "--version", version_id,
        "--gameDir", str(run_dir),
        "--assetsDir", str(game_dir / "assets"),
        "--assetIndex", asset_index,
        "--uuid", "00000000-0000-0000-0000-000000000000",
        "--accessToken", "0",
        "--userType", "mojang",
        "--width", "854",
        "--height", "480",
    ]
    if game_custom_args:
        game_args.extend(game_custom_args)

    command = [java_path]
    command.extend(jvm_args)
    command.extend(["-cp", classpath, main_class])
    command.extend(game_args)
    return command


def launch_process(command: list[str], run_dir: Path) -> subprocess.Popen:
    """Start the game process and return the Popen handle."""
    creationflags = 0
    if is_windows():
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)

    return subprocess.Popen(
        command,
        cwd=str(run_dir),
        shell=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
        encoding="utf-8",
        errors="replace",
        creationflags=creationflags,
    )
