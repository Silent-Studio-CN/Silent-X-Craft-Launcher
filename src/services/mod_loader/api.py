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
"""Mod loader version API clients (Forge, Fabric, NeoForge).

Provides version listing and installer URL generation with BMCLAPI mirror support.
"""

from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from typing import List, Dict, Optional
from enum import Enum

import requests

from src.core.constants import DownloadSource
from src.core.logger import log, log_exception
from src.app.common.launcher_config import cfg


class LoaderSource(Enum):
    AUTO = "auto"
    BMCLAPI = "bmclapi"
    OFFICIAL = "official"


class BaseLoaderAPI:
    """Base class for all mod loader API clients."""

    LOADER_NAME = "base"

    @classmethod
    def _get_source(cls) -> DownloadSource:
        return cfg.downloadSource.value

    @classmethod
    def _should_use_bmclapi(cls) -> bool:
        return cls._get_source() == DownloadSource.BMCLAPI

    @classmethod
    def _safe_request(cls, url: str, timeout: int = 15) -> Optional[requests.Response]:
        try:
            log.debug(f"[{cls.LOADER_NAME}] 请求: {url}")
            resp = requests.get(url, timeout=timeout)
            resp.raise_for_status()
            return resp
        except requests.exceptions.Timeout:
            log.warning(f"[{cls.LOADER_NAME}] 请求超时: {url}")
            return None
        except requests.exceptions.RequestException as e:
            log.warning(f"[{cls.LOADER_NAME}] 请求失败: {e}")
            return None
        except Exception as e:
            log_exception(log, f"[{cls.LOADER_NAME}] 未知错误")
            return None


class ForgeAPI(BaseLoaderAPI):
    """Forge version API — BMCLAPI mirror + official fallback."""

    LOADER_NAME = "Forge"

    BMCLAPI_LIST_URL = "https://bmclapi2.bangbang93.com/forge/minecraft/{mc_version}"
    BMCLAPI_DOWNLOAD_URL = "https://bmclapi2.bangbang93.com/maven/net/minecraftforge/forge/{forge_id}/forge-{forge_id}-installer.jar"
    OFFICIAL_DOWNLOAD_URL = "https://files.minecraftforge.net/maven/net/minecraftforge/forge/{forge_id}/forge-{forge_id}-installer.jar"

    @classmethod
    def fetch_versions(cls, mc_version: str) -> List[Dict]:
        url = cls.BMCLAPI_LIST_URL.format(mc_version=mc_version)
        resp = cls._safe_request(url, timeout=10)
        if not resp:
            return []
        try:
            data = resp.json()
            return data if isinstance(data, list) else []
        except Exception:
            log_exception(log, f"[{cls.LOADER_NAME}] 解析失败")
            return []

    @classmethod
    def get_installer_url(cls, forge_id: str) -> str:
        if cls._should_use_bmclapi():
            return cls.BMCLAPI_DOWNLOAD_URL.format(forge_id=forge_id)
        return cls.OFFICIAL_DOWNLOAD_URL.format(forge_id=forge_id)


class FabricAPI(BaseLoaderAPI):
    """Fabric version API — BMCLAPI mirror + official fallback."""

    LOADER_NAME = "Fabric"

    BMCLAPI_LIST_URL = "https://bmclapi2.bangbang93.com/fabric-meta/v2/versions/loader/{mc_version}"
    BMCLAPI_DOWNLOAD_URL = "https://bmclapi2.bangbang93.com/maven/net/fabricmc/fabric-installer/{version}/fabric-installer-{version}.jar"
    OFFICIAL_LIST_URL = "https://meta.fabricmc.net/v2/versions/loader/{mc_version}"
    OFFICIAL_DOWNLOAD_URL = "https://maven.fabricmc.net/net/fabricmc/fabric-installer/{version}/fabric-installer-{version}.jar"

    @classmethod
    def fetch_versions(cls, mc_version: str, only_stable: bool = False) -> List[Dict]:
        urls = [
            (cls.BMCLAPI_LIST_URL, "BMCLAPI"),
            (cls.OFFICIAL_LIST_URL, "官方"),
        ]
        for url_template, source_name in urls:
            url = url_template.format(mc_version=mc_version)
            resp = cls._safe_request(url, timeout=15)
            if not resp:
                continue
            try:
                data = resp.json()
                if not isinstance(data, list):
                    continue
                if only_stable:
                    stable = [item for item in data if item.get("loader", {}).get("stable", False)]
                    return stable
                return data
            except Exception:
                log_exception(log, f"[{cls.LOADER_NAME}] {source_name} 解析失败")
                continue
        return []

    @classmethod
    def get_installer_url(cls, version: str = "1.0.1") -> str:
        if cls._should_use_bmclapi():
            return cls.BMCLAPI_DOWNLOAD_URL.format(version=version)
        return cls.OFFICIAL_DOWNLOAD_URL.format(version=version)


class NeoForgeAPI(BaseLoaderAPI):
    """NeoForge version API — BMCLAPI mirror + official Maven metadata."""

    LOADER_NAME = "NeoForge"

    BMCLAPI_LIST_URL = "https://bmclapi2.bangbang93.com/neoforge/list/{mc_version}"
    BMCLAPI_DOWNLOAD_URL = "https://bmclapi2.bangbang93.com/maven/net/neoforged/neoforge/{version}/neoforge-{version}-installer.jar"
    OFFICIAL_LIST_URL = "https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml"
    OFFICIAL_DOWNLOAD_URL = "https://maven.neoforged.net/releases/net/neoforged/neoforge/{version}/neoforge-{version}-installer.jar"

    @classmethod
    def _parse_version(cls, version_str: str) -> Optional[Dict]:
        match = re.match(r"^(\d+)\.(\d+)\.", version_str)
        if not match:
            return None
        major = int(match.group(1))
        minor = int(match.group(2))
        if major >= 21:
            mc_version = f"{major}.{minor}"
        else:
            mc_version = f"1.{major}.{minor}"
        is_beta = "-beta" in version_str
        sort_key = version_str.replace("-beta", "")
        return {
            "version": version_str,
            "mc_version": mc_version,
            "is_beta": is_beta,
            "sort_key": sort_key,
            "display": f"{version_str} {'(Beta)' if is_beta else ''}",
        }

    @classmethod
    def fetch_versions(cls, mc_version: Optional[str] = None) -> List[Dict]:
        if mc_version and cls._should_use_bmclapi():
            url = cls.BMCLAPI_LIST_URL.format(mc_version=mc_version)
            resp = cls._safe_request(url, timeout=15)
            if resp:
                try:
                    data = resp.json()
                    versions = []
                    for item in data:
                        version_str = item.get("version", "")
                        if not version_str:
                            continue
                        parsed = cls._parse_version(version_str)
                        if parsed:
                            versions.append(parsed)
                    return sorted(versions, key=lambda x: x.get("sort_key", ""), reverse=True)
                except Exception:
                    log_exception(log, f"[{cls.LOADER_NAME}] BMCLAPI 解析失败")

        resp = cls._safe_request(cls.OFFICIAL_LIST_URL, timeout=15)
        if not resp:
            return []
        try:
            root = ET.fromstring(resp.text)
            versions = []
            for version_elem in root.findall(".//version"):
                version_str = version_elem.text
                if not version_str:
                    continue
                parsed = cls._parse_version(version_str)
                if parsed:
                    versions.append(parsed)
            return sorted(versions, key=lambda x: x.get("sort_key", ""), reverse=True)
        except Exception:
            log_exception(log, f"[{cls.LOADER_NAME}] 官方源解析失败")
            return []

    @classmethod
    def filter_by_mc_version(cls, mc_version: str) -> List[Dict]:
        all_versions = cls.fetch_versions(mc_version)
        if all_versions and all_versions[0].get("mc_version") == mc_version:
            all_match = all(v.get("mc_version") == mc_version for v in all_versions)
            if all_match:
                return all_versions
        parts = mc_version.split(".")
        if parts[0] == "1" and len(parts) >= 3:
            target = f"{parts[0]}.{parts[1]}.{parts[2]}"
        elif len(parts) >= 2:
            target = f"{parts[0]}.{parts[1]}"
        else:
            target = mc_version
        filtered = []
        for v in all_versions:
            mc_ver = v.get("mc_version", "")
            if mc_ver == target or mc_ver.startswith(target) or target.startswith(mc_ver):
                filtered.append(v)
        return filtered

    @classmethod
    def get_installer_url(cls, version: str) -> str:
        if cls._should_use_bmclapi():
            return cls.BMCLAPI_DOWNLOAD_URL.format(version=version)
        return cls.OFFICIAL_DOWNLOAD_URL.format(version=version)


# ── OptiFine (placeholder) ────────────────────────────────────────


def check_optifine(mc_version: str) -> bool:
    return False


# ── Backward-compatible entry points ──────────────────────────────


def fetch_forge_versions(mc_version: str) -> List[Dict]:
    return ForgeAPI.fetch_versions(mc_version)


def fetch_fabric_versions(mc_version: str, only_stable: bool = False) -> List[Dict]:
    return FabricAPI.fetch_versions(mc_version, only_stable)


def fetch_neoforge_versions() -> List[Dict]:
    return NeoForgeAPI.fetch_versions()


def filter_neoforge_by_mc_version(mc_version: str) -> List[Dict]:
    return NeoForgeAPI.filter_by_mc_version(mc_version)
