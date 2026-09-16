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
"""下载任务的资源描述 —— 把 Mojang 元数据翻译成"可校验的下载清单"。

核心原则：**Mojang 提供 SHA1 的资源一律带 sha1 字段**，下载完成后由引擎强制校验，
校验不通过则删除重下（并自动回退到另一条路）。

覆盖矩阵见 verify.py 顶部注释。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Optional, Sequence

from src.core.mirror import candidates, library_url, asset_object_url

__all__ = [
    "FileSpec", "spec_for", "specs_for_version_json", "specs_for_libraries",
    "specs_for_asset_objects", "specs_for_java_runtime", "native_classifier_key",
    "platform_os_name", "platform_arch_name", "rules_allow", "is_native_library",
]


@dataclass
class FileSpec:
    """一个待下载文件：目标路径 + 候选 URL（两条路）+ 校验信息。"""

    dest: Path
    candidates: Sequence[tuple[str, str]]
    sha1: Optional[str] = None
    size: int = 0
    kind: str = ""
    label: str = ""
    required: bool = True
    # 运行期统计（由引擎填写）
    bytes_done: int = 0
    source_used: str = ""

    @property
    def url(self) -> str:
        return self.candidates[0][1] if self.candidates else ""

    @property
    def name(self) -> str:
        return self.label or self.dest.name

    def __repr__(self) -> str:  # pragma: no cover
        return f"FileSpec({self.name}, {self.size}B, sha1={'yes' if self.sha1 else 'no'})"


def _size_of(entry: dict) -> int:
    try:
        return int(entry.get("size") or 0)
    except (TypeError, ValueError):
        return 0


def _sha1_of(entry: dict) -> Optional[str]:
    value = entry.get("sha1")
    return str(value).lower() if value else None


def spec_for(
    dest: Path,
    url: str,
    *,
    sha1: Optional[str] = None,
    size: int = 0,
    kind: str = "",
    label: str = "",
    source=None,
    fallback: bool = True,
) -> FileSpec:
    """构造单个 FileSpec（自动生成两条路候选）。"""
    return FileSpec(
        dest=Path(dest),
        candidates=tuple(candidates(url, kind=kind, source=source, fallback=fallback)),
        sha1=sha1.lower() if sha1 else None,
        size=size or 0,
        kind=kind,
        label=label or Path(dest).name,
    )


def specs_for_version_json(
    version_json: dict,
    game_dir: Path,
    version_name: str,
    *,
    source=None,
) -> list[FileSpec]:
    """从版本 JSON 生成：客户端 jar + 资源索引（两者都带 Mojang sha1）。"""
    game_dir = Path(game_dir)
    version_dir = game_dir / "versions" / version_name
    specs: list[FileSpec] = []

    client = (version_json.get("downloads") or {}).get("client") or {}
    if client.get("url"):
        specs.append(spec_for(
            version_dir / f"{version_name}.jar",
            client["url"],
            sha1=_sha1_of(client),
            size=_size_of(client),
            kind="client-jar",
            label=f"{version_name}.jar",
            source=source,
        ))

    index = version_json.get("assetIndex") or {}
    if index.get("url"):
        index_id = index.get("id") or "assets"
        specs.append(spec_for(
            game_dir / "assets" / "indexes" / f"{index_id}.json",
            index["url"],
            sha1=_sha1_of(index),
            size=_size_of(index),
            kind="asset-index",
            label=f"{index_id}.json",
            source=source,
        ))
    return specs


def rules_allow(lib: dict, *, os_name: Optional[str] = None, arch: Optional[str] = None) -> bool:
    """按 Mojang 的 rules 判断这个库在当前平台是否需要。

    规则语义与原版启动器一致：有 rules 时默认 False，命中的规则决定 allow/disallow；
    没有 rules 则一律允许。带 features 的规则（demo/quickPlay 等）一律不匹配。
    """
    rules = lib.get("rules")
    if not rules:
        return True
    os_name = os_name or platform_os_name()
    arch = arch or platform_arch_name()
    allowed = False
    for rule in rules:
        if not isinstance(rule, dict):
            continue
        if rule.get("features"):
            continue
        os_info = rule.get("os") or {}
        if os_info:
            name = os_info.get("name")
            if name and str(name).lower() != os_name:
                continue
            rule_arch = os_info.get("arch")
            if rule_arch:
                want = str(rule_arch).lower()
                if want in ("x86", "i386", "32") and arch != "x86":
                    continue
                if want in ("x86_64", "amd64") and arch != "x86_64":
                    continue
                if want in ("arm64", "aarch64") and arch != "arm64":
                    continue
        allowed = str(rule.get("action", "allow")).lower() == "allow"
    return allowed


def native_rank(label: str, arch: str) -> int:
    """natives 与当前架构的匹配度：2=精确匹配，1=可接受的通用版，0=不要。

    这个排序很关键：Apple Silicon / Windows ARM64 上同时存在
    natives-macos(其实是 x86_64 dylib) 与 natives-macos-arm64，
    两者解压到同一个目录会互相覆盖（同名 dylib），
    结果就是"架构不对"的崩溃。所以必须只留最匹配的那一个。
    """
    low = (label or "").lower()
    if "-arm64" in low or "aarch64" in low:
        return 2 if arch == "arm64" else 0
    if "x86_64" in low or "-amd64" in low:
        return 2 if arch == "x86_64" else 0
    if "-x86" in low or "-i386" in low or "-32" in low:
        return 2 if arch == "x86" else 0
    # 没有架构后缀：对 64 位平台就是"通用版"（LWJGL 里 natives-macos / natives-windows
    # 指的就是各自平台的默认位数）
    return 2 if arch == "x86_64" else 1


def native_matches_arch(label: str, arch: str) -> bool:
    """兼容旧接口：只要不是明确不匹配就算可用。"""
    return native_rank(label, arch) > 0


def native_group(name: str, path: str = "") -> str:
    """同一个库的不同架构变体归到一组（去掉 natives-<os>-<arch> 后缀）。

    注意 -patch 变体（如 natives-macos-patch）是**叠加补丁**，不是架构变体，
    必须单独成组，否则会被"只要最优架构"的规则吃掉。
    """
    text = str(name or path or "")
    low = text.lower()
    if "-patch" in low:
        return text
    idx = low.find(":natives-")
    if idx > 0:
        return text[:idx]
    for suffix in ("-natives-", "_natives_"):
        idx = low.find(suffix)
        if idx > 0:
            return text[:idx]
    return text


def is_native_library(lib: dict, path: str = "", *, os_name: Optional[str] = None,
                      arch: Optional[str] = None) -> bool:
    """判断这个库是不是 natives（新式：独立库条目 org.lwjgl:lwjgl:3.3.3:natives-windows）。"""
    name = str(lib.get("name") or "")
    target = f"{name} {path}".lower()
    if "natives" not in target:
        return False
    arch = arch or platform_arch_name()
    os_name = os_name or platform_os_name()
    if not native_matches_arch(target, arch):
        return False
    # 形如 natives-windows / natives-linux / natives-macos-arm64
    for alias in ("windows", "linux", "macos", "osx"):
        if f"natives-{alias}" in target:
            return alias in ("macos", "osx") and os_name == "osx" or alias == os_name
    return False


def specs_for_libraries(
    version_json: dict,
    game_dir: Path,
    *,
    source=None,
    os_name: Optional[str] = None,
    arch: Optional[str] = None,
) -> list[FileSpec]:
    """所有依赖库，含两种形态的 natives。

    * 旧式：downloads.classifiers 里的 natives-<os>（1.18 及更早）
    * 新式：独立库条目 org.lwjgl:xxx:3.3.3:natives-windows（1.19+，Mojang 现在用这种）

    同时按 rules 过滤掉不属于当前平台的库（以前会把 macOS/Linux 的 natives
    以及 32 位版本全部下载下来，既浪费带宽又污染 libraries 目录）。
    """
    game_dir = Path(game_dir)
    os_name = os_name or platform_os_name()
    arch = arch or platform_arch_name()
    specs: list[FileSpec] = []
    seen: set[str] = set()
    # natives 每个库只保留"架构最匹配"的那一个变体
    best_native: dict[str, tuple[int, FileSpec]] = {}

    for lib in version_json.get("libraries", []):
        if not rules_allow(lib, os_name=os_name, arch=arch):
            continue
        downloads = lib.get("downloads") or {}
        artifact = downloads.get("artifact") or {}
        path = artifact.get("path")
        if path and path not in seen:
            native = is_native_library(lib, path, os_name=os_name, arch=arch)
            spec = spec_for(
                game_dir / "libraries" / path,
                artifact.get("url") or library_url(path),
                sha1=_sha1_of(artifact),
                size=_size_of(artifact),
                kind="native" if native else "library",
                label=path,
                source=source,
            )
            if native:
                rank = native_rank(str(lib.get("name") or "") + " " + path, arch)
                if rank <= 0:
                    continue                      # 架构不对的 natives 直接不要
                group = native_group(str(lib.get("name") or ""), path)
                current = best_native.get(group)
                if current and current[0] >= rank:
                    continue                      # 已经有更匹配的变体
                if current:
                    try:
                        specs.remove(current[1])  # 摘掉先前那个不够匹配的
                    except ValueError:
                        pass
                best_native[group] = (rank, spec)
            seen.add(path)
            specs.append(spec)

        classifiers = downloads.get("classifiers") or {}
        key = native_classifier_key(classifiers, os_name=os_name, arch=arch)
        if key and key in classifiers:
            entry = classifiers[key] or {}
            cpath = entry.get("path")
            if cpath and cpath not in seen and native_matches_arch(key + " " + cpath, arch):
                seen.add(cpath)
                specs.append(spec_for(
                    game_dir / "libraries" / cpath,
                    entry.get("url") or library_url(cpath),
                    sha1=_sha1_of(entry),
                    size=_size_of(entry),
                    kind="native",
                    label=cpath,
                    source=source,
                ))
    return specs


def specs_for_asset_objects(
    asset_index: dict,
    game_dir: Path,
    *,
    source=None,
    limit: int = 0,
) -> list[FileSpec]:
    """资源对象（文件名即 SHA1，因此天然全量校验）。"""
    game_dir = Path(game_dir)
    specs: list[FileSpec] = []
    for name, info in (asset_index.get("objects") or {}).items():
        obj_hash = (info or {}).get("hash")
        if not obj_hash:
            continue
        specs.append(spec_for(
            game_dir / "assets" / "objects" / obj_hash[:2] / obj_hash,
            asset_object_url(obj_hash),
            sha1=obj_hash,
            size=_size_of(info or {}),
            kind="asset",
            label=name,
            source=source,
        ))
        if limit and len(specs) >= limit:
            break
    return specs


def specs_for_java_runtime(
    manifest: dict,
    target_dir: Path,
    *,
    source=None,
) -> list[FileSpec]:
    """官方 JRE 清单（all.json -> 组件 manifest）里的全部文件。

    这些文件同样带 Mojang sha1（downloads.raw.sha1），必须逐个校验。
    """
    target_dir = Path(target_dir)
    specs: list[FileSpec] = []
    for rel, entry in (manifest.get("files") or {}).items():
        entry = entry or {}
        if entry.get("type") == "directory":
            continue
        raw = (entry.get("downloads") or {}).get("raw") or {}
        if not raw.get("url"):
            continue
        if entry.get("type") == "link":
            continue
        specs.append(spec_for(
            target_dir / rel,
            raw["url"],
            sha1=_sha1_of(raw),
            size=_size_of(raw),
            kind="java-runtime",
            label=rel,
            source=source,
        ))
    return specs


# ── 平台判定（natives 分类器）─────────────────────────────────────


def platform_os_name() -> str:
    from src.core.platform import PlatformType, current_platform
    plat = current_platform()
    if plat is PlatformType.WINDOWS:
        return "windows"
    if plat is PlatformType.MACOS:
        return "osx"
    return "linux"


def platform_arch_name() -> str:
    from src.core.platform import ArchType, current_arch
    arch = current_arch()
    if arch is ArchType.ARM64:
        return "arm64"
    if arch in (ArchType.X86, ArchType.X86_64):
        return "x86_64" if arch is ArchType.X86_64 else "x86"
    return "x86_64"


def native_classifier_key(
    classifiers: dict,
    *,
    os_name: Optional[str] = None,
    arch: Optional[str] = None,
) -> Optional[str]:
    """从 downloads.classifiers 里挑出当前平台该用的 natives。

    兼容老式 natives-<os>-<arch> 与新式 <arch>-<os>（如 arm64-osx）。
    """
    if not classifiers:
        return None
    os_name = os_name or platform_os_name()
    arch = arch or platform_arch_name()
    os_aliases = {
        "windows": ("windows", "win"),
        "osx": ("osx", "macos", "mac"),
        "linux": ("linux",),
    }.get(os_name, (os_name,))
    arch_aliases = {
        "x86_64": ("x86_64", "amd64", "64"),
        "x86": ("x86", "32", "i386"),
        "arm64": ("arm64", "aarch64"),
    }.get(arch, (arch,))

    # 1) 精确匹配（含架构）
    for key in classifiers:
        low = key.lower()
        if any(o in low for o in os_aliases) and any(a in low for a in arch_aliases):
            return key
    # 2) 只匹配系统
    for key in classifiers:
        low = key.lower()
        if any(o in low for o in os_aliases):
            return key
    return None
