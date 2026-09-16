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
"""Forge / NeoForge 安装器分析器（共用实现，参数化 URL）"""

from __future__ import annotations

import json
from pathlib import Path
from typing import List, Dict, Optional

from src.core.logger import log

from .base import LoaderAnalyzer


def _mirror_url(url: str) -> str:
    from src.core.mirror import maybe_mirror_url
    return maybe_mirror_url(url)


class _ForgeLikeAnalyzer(LoaderAnalyzer):
    """Forge / NeoForge 共用分析基类。

    子类只需覆写 :meth:`get_loader_type` 和 :meth:`_maven_url`。
    """

    MAVEN_URL = "https://maven.minecraftforge.net/"  # 子类可覆写

    def __init__(self, installer_path: Path):
        super().__init__(installer_path)
        self._profile = None
        self._version_info = None
        self._load_metadata()

    # ── 子类可自定义 ──────────────────────────────────────────

    def get_loader_type(self) -> str:
        return "forge"

    def _maven_url(self) -> str:
        return self.MAVEN_URL

    # ── 共享实现 ──────────────────────────────────────────────

    def _load_metadata(self):
        content = self._read_file_from_jar("install_profile.json")
        if content:
            self._profile = json.loads(content)

        # 老格式（Forge <= 1.12）：版本 JSON 直接嵌在 install_profile.json 的 versionInfo 里
        version_info = (self._profile or {}).get("versionInfo") or {}

        # 新格式（Forge 1.13+ / NeoForge）：真正的版本 JSON 是 jar 根目录的 version.json，
        # 里面有 inheritsFrom / mainClass / libraries，与官方版本 JSON 同构
        if not version_info:
            raw = self._read_file_from_jar("version.json")
            if raw:
                try:
                    version_info = json.loads(raw)
                except Exception:
                    version_info = {}

        self._version_info = version_info

    def _section(self, key: str, default):
        # 老格式把 libraries/processors/minecraft 放在 install 下面，新格式放在顶层。
        if not self._profile:
            return default
        install = self._profile.get("install") or {}
        if key in install:
            return install[key]
        return self._profile.get(key, default)

    def get_libraries(self) -> List[Dict[str, str]]:
        libraries = []
        if not self._profile:
            return libraries

        # 老格式（1.12 及更早）：通用 jar 的真实路径写在 install.path 里，
        # 而 install.libraries 里那条 net.minecraftforge:forge:<版本> **没有分类器**，
        # 拼出来是 forge-<版本>.jar —— maven 上并不存在（实测 404）。PCL 也是直接用 install.path。
        install = (self._profile or {}).get("install") or {}
        legacy_path = str(install.get("path") or "").strip().replace(chr(92), "/")

        for lib in self._section("libraries", []) or []:
            if not isinstance(lib, dict):
                continue
            name = lib.get("name")
            if not name:
                continue
            if legacy_path and name.count(":") == 2 and name.startswith(
                    ("net.minecraftforge:forge:", "net.minecraftforge:fmlloader:")):
                log.info("[ForgeAnalyzer] 跳过无分类器的加载器坐标（改用 install.path）: %s", name)
                continue
            # 没有显式 url 的坐标要走加载器自己的 maven：这些是安装器用的库
            # （lzma-java / gson…），libraries.minecraft.net 上是 404，
            # 实测 maven.minecraftforge.net 和它的 BMCLAPI 镜像都有。
            url = lib.get("url") or self._maven_url()
            libraries.append({"name": name, "url": _mirror_url(url), "path": self._coord_to_path(name)})

        for processor in self._section("processors", []) or []:
            for cp_entry in processor.get("classpath", []):
                libraries.append({
                    "name": cp_entry,
                    "url": _mirror_url(self._maven_url()),
                    "path": self._coord_to_path(cp_entry),
                })

        if legacy_path:
            # 老格式的通用 jar：路径就是 install.path，直接照用（别自己拼）
            libraries.append({
                "name": legacy_path.rsplit("/", 1)[-1],
                "url": _mirror_url(self._maven_url()),
                "path": legacy_path,
            })

        seen = set()
        unique = []
        for lib in libraries:
            if lib["name"] not in seen:
                seen.add(lib["name"])
                unique.append(lib)
        return unique

    def get_main_class(self) -> Optional[str]:
        return self._version_info.get("mainClass") if self._version_info else None

    def get_version_info(self) -> Dict:
        return self._version_info or {}

    def get_processors(self) -> List[Dict]:
        return self._section("processors", [])

    def get_minecraft_version(self) -> Optional[str]:
        return self._section("minecraft", None)


class ForgeAnalyzer(_ForgeLikeAnalyzer):
    MAVEN_URL = "https://maven.minecraftforge.net/"

    def get_loader_type(self) -> str:
        return "forge"


class NeoForgeAnalyzer(_ForgeLikeAnalyzer):
    MAVEN_URL = "https://maven.neoforged.net/releases/"

    def get_loader_type(self) -> str:
        return "neoforge"
