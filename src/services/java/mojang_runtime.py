# 版权所有 © Silent X Craft Launcher Dev 开发团队
#
# Silent X Craft Launcher (SXCL) 是一款由 Silent X Craft Launcher Dev 团队开发，
# 隶属于 SilentCodeTeams 旗下，并由 SilentStudio 管理的 Minecraft 第三方启动器。
#
# Copyright © Silent X Craft Launcher Development Team
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
"""Mojang 官方 Java 运行时（JRE/JDK）下载与安装 —— 第一条路。

与 config/Java_index.yaml（Oracle CDN 安装包）相比，官方这条路的最大好处是
**每个文件都带 sha1**，可以逐个校验，而且不需要跑 exe/msi 安装器。

官方清单结构
------------
all.json[平台][组件] = [ {manifest: {url, sha1, size}, version: {name, released}} ]
内层 manifest.files[相对路径] = {
    type: "file" | "directory" | "link",
    executable: bool,
    downloads: {raw: {url, sha1, size}},
}

支持两条路：launchermeta 的清单与 piston-meta/piston-data 的文件都能走 BMCLAPI
（实测同路径透传，字节数与官方一致）。
"""

from __future__ import annotations

import json
import os
import stat
from pathlib import Path
from typing import Callable, Optional

from src.core.download import default_engine, specs_for_java_runtime
from src.core.net import fetch_bytes
from src.core.download.verify import sha1_file
from src.core.logger import log
from src.core.mirror import candidates

__all__ = [
    "JAVA_RUNTIME_MANIFEST", "COMPONENT_PREVIEW", "platform_key",
    "required_java_major", "choose_component", "fetch_all_json", "fetch_component_manifest",
    "runtime_dir", "install_runtime", "find_installed_runtimes",
]

JAVA_RUNTIME_MANIFEST = (
    "https://launchermeta.mojang.com/v1/products/java-runtime/"
    "2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json"
)

# 给 UI 用的候选（真正可用组件以 all.json 为准）
COMPONENT_PREVIEW: list[tuple[str, str]] = [
    ("Java 8  (1.16.5 及更早)", "jre-legacy"),
    ("Java 17 (1.18 - 1.20.4)", "java-runtime-gamma"),
    ("Java 21 (1.20.5 - 1.21.x)", "java-runtime-delta"),
]


# ── 平台 / 组件选择 ──────────────────────────────────────────────


def platform_key() -> str:
    """官方 all.json 里的平台键（唯一允许判断系统的地方是 src/core/platform.py）。"""
    from src.core.platform import ArchType, PlatformType, current_arch, current_platform

    plat = current_platform()
    arch = current_arch()
    if plat is PlatformType.WINDOWS:
        return "windows-arm64" if arch is ArchType.ARM64 else "windows-x64"
    if plat is PlatformType.MACOS:
        return "mac-os-arm64" if arch is ArchType.ARM64 else "mac-os"
    return "linux"


def required_java_major(mc_version: str) -> int:
    """某个 Minecraft 版本需要的 Java 主版本（与启动期兼容性判断保持一致）。"""
    if not mc_version:
        return 21
    try:
        parts = [int(x) for x in mc_version.split(".")[:3]]
    except ValueError:
        return 21
    major = parts[0] if parts else 1
    minor = parts[1] if len(parts) > 1 else 0
    if major >= 26:
        return 25
    if major >= 21:            # 26.x 之前的 21.x 体系不存在，这里防御性保留
        return 21
    if major == 1:
        if minor >= 21:
            return 21
        if minor >= 18:
            return 17
        if minor >= 17:
            return 17
    return 8


def _java_major_of(version_name: str) -> int:
    text = str(version_name or "").strip()
    if text.startswith("1."):
        text = text[2:]
    head = text.split(".")[0].split("-")[0].split("u")[0]
    try:
        return int(head)
    except ValueError:
        return 0


# minecraft-java-exe 是启动器用的 exe，不是 JRE；gamma-snapshot 是快照版
_EXCLUDED_COMPONENTS = frozenset({"minecraft-java-exe", "java-runtime-gamma-snapshot"})

# 同主版本时官方推荐顺序（17 有 beta/gamma 两个，gamma 才是 1.18-1.20.4 正式版）
_PREFERRED_ORDER = (
    "java-runtime-delta", "java-runtime-gamma", "java-runtime-epsilon",
    "jre-legacy", "java-runtime-beta", "java-runtime-alpha",
)


def choose_component(all_json: dict, platform: str, required: int) -> Optional[str]:
    """挑一个 Java 主版本 >= required 的组件（够用即可，同版本按官方推荐顺序）。"""
    entries = []
    for component, items in (all_json.get(platform) or {}).items():
        if not items or component in _EXCLUDED_COMPONENTS:
            continue
        name = (items[0].get("version") or {}).get("name", "")
        major = _java_major_of(name)
        if major:
            entries.append((major, component, str(name)))
    if not entries:
        return None
    enough = [item for item in entries if item[0] >= required] or entries
    best_major = min(item[0] for item in enough)
    same = [item for item in enough if item[0] == best_major]
    for preferred in _PREFERRED_ORDER:
        for _major, component, _name in same:
            if component == preferred:
                return component
    return same[0][1]


# ── 清单抓取（两条路）────────────────────────────────────────────


def _get_bytes(url: str, sha1: str = "", timeout: int = 30) -> Optional[bytes]:
    """抓清单（两条路 + 熔断 + 硬截止 + 可选 sha1 校验）。"""
    return fetch_bytes(url, sha1=sha1, deadline=max(15.0, timeout + 5))


def sha1_file_from_bytes(data: bytes) -> str:
    import hashlib
    return hashlib.sha1(data).hexdigest()


def fetch_all_json(timeout: int = 30) -> Optional[dict]:
    """拉取官方 all.json（JRE 组件总清单）。"""
    data = _get_bytes(JAVA_RUNTIME_MANIFEST, timeout=timeout)
    if not data:
        return None
    try:
        return json.loads(data.decode("utf-8"))
    except Exception as exc:
        log.error("all.json 解析失败: %s", exc)
        return None


def fetch_component_manifest(all_json: dict, platform: str, component: str,
                             timeout: int = 30) -> Optional[dict]:
    """拉取某个组件的文件清单（带 sha1 校验）。"""
    items = (all_json.get(platform) or {}).get(component) or []
    if not items:
        log.error("组件不存在: %s / %s", platform, component)
        return None
    entry = items[0]
    manifest = entry.get("manifest") or {}
    url = manifest.get("url")
    if not url:
        return None
    data = _get_bytes(url, str(manifest.get("sha1") or ""), timeout=timeout)
    if not data:
        return None
    try:
        payload = json.loads(data.decode("utf-8"))
    except Exception as exc:
        log.error("组件清单解析失败: %s", exc)
        return None
    payload["_version_name"] = (entry.get("version") or {}).get("name", "")
    payload["_component"] = component
    return payload


# ── 安装 ─────────────────────────────────────────────────────────


def runtime_dir() -> Path:
    from src.core.platform import default_config_directory
    return default_config_directory("SilentXCraftLauncher") / "runtime"


def install_runtime(
    *,
    component: str = "",
    mc_version: str = "",
    target_root: Optional[Path] = None,
    on_progress: Optional[Callable] = None,
    on_status: Optional[Callable[[str], None]] = None,
    cancel=None,
) -> Optional[Path]:
    """下载并安装官方 JRE，返回 java home（含 bin/java[.exe]），失败返回 None。"""
    status = on_status or (lambda text: None)

    status("获取官方 JRE 清单…")
    all_json = fetch_all_json()
    if not all_json:
        status("❌ 无法获取官方 JRE 清单（两条路都不通）")
        return None

    platform = platform_key()
    if not component:
        required = required_java_major(mc_version)
        component = choose_component(all_json, platform, required) or "java-runtime-delta"
        status(f"按 Minecraft {mc_version or '最新版'} 选择组件: {component}")
    else:
        status(f"使用组件: {component}")

    status("下载组件文件清单…")
    manifest = fetch_component_manifest(all_json, platform, component)
    if not manifest:
        status("❌ 组件清单下载或校验失败")
        return None

    version_name = manifest.get("_version_name", "")
    target = Path(target_root) if target_root else (runtime_dir() / f"{component}-{platform}")
    target.mkdir(parents=True, exist_ok=True)

    files = manifest.get("files") or {}
    dirs = [rel for rel, info in files.items() if (info or {}).get("type") == "directory"]
    for rel in dirs:
        try:
            (target / rel).mkdir(parents=True, exist_ok=True)
        except OSError:
            pass

    specs = specs_for_java_runtime(manifest, target)
    if not specs:
        status("❌ 组件清单里没有可下载的文件")
        return None

    total_mb = sum(max(0, s.size) for s in specs) / 1024 / 1024
    status(f"下载 Java {version_name}（{len(specs)} 个文件，{total_mb:.0f} MB）…")

    engine = default_engine()
    results = engine.download(specs, on_progress=on_progress, cancel=cancel)
    failed = [r for r in results if not r.ok]
    if failed:
        status(f"❌ {len(failed)}/{len(results)} 个文件下载或校验失败")
        log.error("JRE 安装失败，前几个: %s", ", ".join(r.name for r in failed[:5]))
        return None

    from src.core.platform import is_windows, java_executable_name

    # 可执行位（Linux/macOS 必需；Windows 没有这个概念）
    if not is_windows():
        for rel, info in files.items():
            if (info or {}).get("executable"):
                path = target / rel
                try:
                    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
                except OSError:
                    pass

    java_bin = target / "bin" / java_executable_name()
    if not java_bin.exists():
        status("❌ 安装完成但找不到 bin/java")
        return None

    try:
        marker = {
            "component": component,
            "platform": platform,
            "version": version_name,
            "java_home": str(target),
        }
        (target / ".sxcl_runtime.json").write_text(
            json.dumps(marker, ensure_ascii=False, indent=2), encoding="utf-8")
    except OSError:
        pass

    status(f"✅ Java {version_name} 安装完成")
    log.info("JRE 安装完成: %s -> %s", component, target)
    return target


def find_installed_runtimes() -> list:
    """扫描已安装的官方 JRE，返回 JavaInstallation 列表（供设置页合并显示）。"""
    from src.services.java.finder import inspect_java

    from src.core.platform import java_executable_name

    found = []
    root = runtime_dir()
    if not root.exists():
        return found
    for child in root.iterdir():
        if not child.is_dir():
            continue
        java_bin = child / "bin" / java_executable_name()
        if java_bin.exists():
            install = inspect_java(java_bin)
            if install:
                found.append(install)
    return found
