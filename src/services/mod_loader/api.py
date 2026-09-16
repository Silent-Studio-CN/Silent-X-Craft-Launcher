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
"""模组加载器 API 客户端（Forge / Fabric / NeoForge / OptiFine）。

约定（也是目录重构时的迁移要点）
--------------------------------
1. 这里只写**官方地址**；镜像由 src/core/mirror.candidates() 自动补齐，
   所以本文件不再出现第二套 BMCLAPI 常量（以前每个接口都写两份 URL）。
2. 请求统一走 src/core/net.py —— 硬截止 + 熔断 + 源偏好，某个源被限流时
   不会再把下载配置页卡住几十秒。
3. 对外函数签名保持不变，调用方（下载配置页 / 安装器）无需改动。

实测记录（2026-09）
------------------
* BMCLAPI 的 /forge/minecraft/<mc>、/neoforge/list/<mc>、/optifine/<mc> 是**镜像独有**
  接口，官方没有等价物；前两者用 maven-metadata.xml 兜底，OptiFine 只能靠镜像。
* /maven/ 不提供 fabric-installer（实测 404），Fabric 安装器固定走官方 maven；
  镜像保护见 src/core/mirror.MIRROR_UNSAFE_PATTERNS。
"""

from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from enum import Enum
from typing import Dict, List, Optional

from src.core.logger import log, log_exception
from src.core.mirror import candidates
from src.core.net import fetch_bytes, fetch_json

__all__ = [
    "LoaderSource", "BaseLoaderAPI", "ForgeAPI", "FabricAPI", "NeoForgeAPI",
    "fetch_forge_versions", "fetch_fabric_versions", "fetch_neoforge_versions",
    "filter_neoforge_by_mc_version", "fetch_optifine_versions", "check_optifine",
]

BMCLAPI_BASE = "https://bmclapi2.bangbang93.com"


class LoaderSource(Enum):
    """保留的兼容枚举（新代码请用设置里的下载源 + 智能选源）。"""

    AUTO = "auto"
    BMCLAPI = "bmclapi"
    OFFICIAL = "official"


# ── 工具 ─────────────────────────────────────────────────────────


def _version_key(text: str) -> tuple:
    """把 52.0.16 / 0.19.5-beta.1 之类变成可比较的数字元组。"""
    return tuple(int(part) for part in re.findall(r"\d+", str(text))) or (0,)


def _sort_desc(items: List[Dict], key: str = "version") -> List[Dict]:
    """按版本号倒序：最新的排在最前面，UI 默认就选中最新版。"""
    return sorted(items, key=lambda item: _version_key(item.get(key, "")), reverse=True)


def _mc_key(mc_version: str) -> str:
    """把 1.21 / 1.21.1 归一成 NeoForge 使用的 21.1 形式。"""
    parts = [p for p in str(mc_version).split(".") if p.isdigit()]
    if not parts:
        return str(mc_version)
    if parts[0] == "1" and len(parts) >= 2:
        return f"{parts[1]}.{parts[2]}" if len(parts) >= 3 else parts[1]
    return ".".join(parts[:2])


class BaseLoaderAPI:
    """所有加载器 API 的基类：只负责取数据，URL 与回退交给 core 层。"""

    LOADER_NAME = "base"
    TIMEOUT = 15.0

    @classmethod
    def _json(cls, url: str, timeout: Optional[float] = None):
        data = fetch_json(url, deadline=(timeout or cls.TIMEOUT) + 10)
        if data is None:
            log.debug("[%s] 取不到 JSON: %s", cls.LOADER_NAME, url[:90])
        return data

    @classmethod
    def _text(cls, url: str, timeout: Optional[float] = None) -> Optional[str]:
        data = fetch_bytes(url, deadline=(timeout or cls.TIMEOUT) + 10)
        return data.decode("utf-8", "replace") if data else None

    @classmethod
    def _xml_versions(cls, url: str, timeout: Optional[float] = None) -> List[str]:
        """读 maven-metadata.xml 的 version 列表。"""
        text = cls._text(url, timeout)
        if not text:
            return []
        try:
            root = ET.fromstring(text)
            return [elem.text.strip() for elem in root.findall(".//version") if elem.text]
        except Exception:
            log_exception(log, f"[{cls.LOADER_NAME}] maven-metadata 解析失败")
            return []


# ── Forge ────────────────────────────────────────────────────────


class ForgeAPI(BaseLoaderAPI):
    """Forge 版本列表 + 安装器地址。

    列表：镜像独有的 /forge/minecraft/<mc> 优先，官方 maven-metadata.xml 兜底。
    安装器：官方 maven（/maven/ 镜像可用，由 candidates() 自动补上）。
    """

    LOADER_NAME = "Forge"
    LIST_URL = BMCLAPI_BASE + "/forge/minecraft/{mc}"
    MAVEN_METADATA = "https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml"
    INSTALLER_URL = ("https://maven.minecraftforge.net/net/minecraftforge/forge/"
                     "{forge_id}/forge-{forge_id}-installer.jar")

    @classmethod
    def fetch_versions(cls, mc_version: str) -> List[Dict]:
        data = cls._json(cls.LIST_URL.format(mc=mc_version))
        if isinstance(data, list) and data:
            return _sort_desc([item for item in data if item.get("version")], "version")

        # 官方兜底：把 1.21.1-52.1.16 这样的版本号切成 Forge 版本
        prefix = f"{mc_version}-"
        official = [
            {"version": ver[len(prefix):], "mcversion": mc_version, "official": True}
            for ver in cls._xml_versions(cls.MAVEN_METADATA)
            if ver.startswith(prefix)
        ]
        if official:
            log.info("[Forge] 官方源列出了 %d 个 %s 版本", len(official), mc_version)
        return _sort_desc(official, "version")

    @classmethod
    def get_installer_url(cls, forge_id: str) -> str:
        return cls.INSTALLER_URL.format(forge_id=forge_id)

    @classmethod
    def get_installer_candidates(cls, forge_id: str) -> List[tuple]:
        return candidates(cls.get_installer_url(forge_id))


# ── Fabric ───────────────────────────────────────────────────────


class FabricAPI(BaseLoaderAPI):
    """Fabric 版本列表 + 安装器地址（安装器固定走官方 maven）。"""

    LOADER_NAME = "Fabric"
    LIST_URL = "https://meta.fabricmc.net/v2/versions/loader/{mc}"
    INSTALLER_METADATA = "https://maven.fabricmc.net/net/fabricmc/fabric-installer/maven-metadata.xml"
    INSTALLER_URL = ("https://maven.fabricmc.net/net/fabricmc/fabric-installer/"
                     "{version}/fabric-installer-{version}.jar")

    _installer_version: Optional[str] = None

    @classmethod
    def fetch_versions(cls, mc_version: str, only_stable: bool = False) -> List[Dict]:
        data = cls._json(cls.LIST_URL.format(mc=mc_version))
        if not isinstance(data, list):
            return []
        if only_stable:
            data = [item for item in data if (item.get("loader") or {}).get("stable", False)]
        return data

    @classmethod
    def latest_installer_version(cls) -> str:
        """安装器最新版（官方 maven-metadata，进程内缓存）。"""
        if cls._installer_version:
            return cls._installer_version
        text = cls._text(cls.INSTALLER_METADATA, timeout=10)
        latest = ""
        if text:
            match = re.search(r"<latest>([^<]+)</latest>", text)
            if match:
                latest = match.group(1).strip()
            else:
                all_versions = re.findall(r"<version>([^<]+)</version>", text)
                latest = all_versions[-1].strip() if all_versions else ""
        cls._installer_version = latest or "1.1.2"     # 兜底：实测可用版本
        log.info("[Fabric] 安装器版本: %s", cls._installer_version)
        return cls._installer_version

    @classmethod
    def get_installer_url(cls, version: Optional[str] = None) -> str:
        return cls.INSTALLER_URL.format(version=version or cls.latest_installer_version())

    @classmethod
    def get_installer_candidates(cls, version: Optional[str] = None) -> List[tuple]:
        # fabric-installer 在镜像上不存在，candidates 会按 UNSAFE 规则只给官方
        return candidates(cls.get_installer_url(version), kind="fabric-installer")


# ── NeoForge ─────────────────────────────────────────────────────


class NeoForgeAPI(BaseLoaderAPI):
    """NeoForge 版本列表 + 安装器地址。"""

    LOADER_NAME = "NeoForge"
    LIST_URL = BMCLAPI_BASE + "/neoforge/list/{mc}"
    MAVEN_METADATA = ("https://maven.neoforged.net/releases/net/neoforged/neoforge/"
                      "maven-metadata.xml")
    INSTALLER_URL = ("https://maven.neoforged.net/releases/net/neoforged/neoforge/"
                     "{version}/neoforge-{version}-installer.jar")

    @classmethod
    def _parse_version(cls, version_str: str) -> Optional[Dict]:
        match = re.match(r"^(\d+)\.(\d+)\.", version_str)
        if not match:
            return None
        major, minor = int(match.group(1)), int(match.group(2))
        mc_version = f"{major}.{minor}" if major >= 21 else f"1.{major}.{minor}"
        is_beta = "-beta" in version_str
        return {
            "version": version_str,
            "mc_version": mc_version,
            "is_beta": is_beta,
            "sort_key": version_str.replace("-beta", ""),
            "display": f"{version_str} {'(Beta)' if is_beta else ''}",
        }

    @classmethod
    def fetch_versions(cls, mc_version: Optional[str] = None) -> List[Dict]:
        raw: List[str] = []
        if mc_version:
            data = cls._json(cls.LIST_URL.format(mc=mc_version))
            if isinstance(data, list):
                raw = [item.get("version", "") for item in data if item.get("version")]
        if not raw:      # 官方兜底：整包 maven-metadata，再自行建索引
            raw = cls._xml_versions(cls.MAVEN_METADATA)
        parsed = [item for item in (cls._parse_version(v) for v in raw if v) if item]
        return _sort_desc(parsed, "version")

    @classmethod
    def filter_by_mc_version(cls, mc_version: str) -> List[Dict]:
        """只保留该 Minecraft 版本可用的 NeoForge。"""
        versions = cls.fetch_versions(mc_version)
        wanted = _mc_key(mc_version)
        matched = [item for item in versions if item.get("mc_version") == wanted]
        if matched:
            return matched
        return [item for item in versions
                if str(item.get("mc_version", "")).startswith(wanted)
                or wanted.startswith(str(item.get("mc_version", "")))]

    @classmethod
    def get_installer_url(cls, version: str) -> str:
        return cls.INSTALLER_URL.format(version=version)

    @classmethod
    def get_installer_candidates(cls, version: str) -> List[tuple]:
        return candidates(cls.get_installer_url(version))


# ── OptiFine（镜像独有接口，官方无 API）──────────────────────────


def fetch_optifine_versions(mc_version: str) -> List[Dict]:
    """OptiFine 版本列表：官方没有接口，只有 BMCLAPI 有。"""
    data = ForgeAPI._json(BMCLAPI_BASE + f"/optifine/{mc_version}", timeout=8)
    if isinstance(data, list):
        return _sort_desc([item for item in data if item.get("patch")], "patch")
    return []


def check_optifine(mc_version: str) -> bool:
    """该版本是否支持 OptiFine（以前恒为 False，UI 永远显示"—"）。"""
    return bool(fetch_optifine_versions(mc_version))


# ── 兼容入口（旧调用方在用）──────────────────────────────────────


def fetch_forge_versions(mc_version: str) -> List[Dict]:
    return ForgeAPI.fetch_versions(mc_version)


def fetch_fabric_versions(mc_version: str, only_stable: bool = False) -> List[Dict]:
    return FabricAPI.fetch_versions(mc_version, only_stable)


def fetch_neoforge_versions() -> List[Dict]:
    return NeoForgeAPI.fetch_versions()


def filter_neoforge_by_mc_version(mc_version: str) -> List[Dict]:
    return NeoForgeAPI.filter_by_mc_version(mc_version)
