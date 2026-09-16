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
"""Minecraft version manifest fetching, parsing, and filtering.

Includes a TTL-based cache to avoid re-fetching on every page load.
"""

from __future__ import annotations

import json
import time
from datetime import datetime
from dataclasses import dataclass
from typing import Optional

import requests

from src.core.net import fetch_bytes

from src.core.constants import MOJANG_VERSION_MANIFEST_URL, DownloadSource, VersionType
from src.core.exceptions import NetworkError
from src.core.logger import log

# ── Manifest cache ────────────────────────────────────────────────

_CACHE_TTL_SECONDS = 300  # 5 minutes
_cache: dict[str, tuple[float, str, list["GameVersion"]]] = {}


def _cached_fetch(source: DownloadSource, timeout: float = 20.0) -> tuple[str, list["GameVersion"]]:
    """TTL-cached version manifest fetch."""
    key = source.value
    now = time.time()
    cached = _cache.get(key)
    if cached and (now - cached[0]) < _CACHE_TTL_SECONDS:
        return cached[1], list(cached[2])  # return a shallow copy
    latest_str, versions = _do_fetch(source, timeout)
    _cache[key] = (now, latest_str, versions)
    return latest_str, list(versions)


def invalidate_cache(source: Optional[DownloadSource] = None) -> None:
    """Clear the manifest cache for *source* (or all sources)."""
    if source:
        _cache.pop(source.value, None)
    else:
        _cache.clear()


@dataclass(frozen=True)
class GameVersion:
    id: str
    version_type: str
    url: str
    release_time: str
    # v2 清单里每个版本 JSON 都有 sha1，安装时要拿它校验下载到的 JSON
    sha1: str = ""
    size: int = 0

    @property
    def category(self) -> VersionType:
        if self.version_type == "release":
            return VersionType.RELEASE
        if self.version_type == "snapshot":
            return VersionType.SNAPSHOT
        return VersionType.OLD

    @property
    def release_label(self) -> str:
        try:
            dt = datetime.fromisoformat(self.release_time.replace("Z", "+00:00"))
            return dt.strftime("%Y-%m-%d")
        except ValueError:
            return self.release_time


def _do_fetch(
    source: DownloadSource,
    timeout: float = 20.0,
) -> tuple[str, list[GameVersion]]:
    """抓取版本清单：官方 / BMCLAPI 两条路轮转，带重试。

    以前只请求"当前选择的那一条路"，镜像抽风时整条安装流程直接失败；
    现在任一源可用即可，且会把实际生效的源记进日志。
    """
    last_error: Exception | None = None
    for round_no in range(2):
        data = fetch_bytes(MOJANG_VERSION_MANIFEST_URL)
        if not data:
            last_error = NetworkError("所有下载源均失败")
            time.sleep(0.4 * (round_no + 1))
            continue
        try:
            payload = json.loads(data.decode("utf-8"))
            latest = payload.get("latest", {})
            versions: list[GameVersion] = []
            for item in payload.get("versions", []):
                versions.append(
                    GameVersion(
                        id=item.get("id", ""),
                        version_type=item.get("type", "old"),
                        url=item.get("url", ""),
                        release_time=item.get("releaseTime", ""),
                        sha1=str(item.get("sha1") or ""),
                        size=int(item.get("size") or 0),
                    )
                )
            log.info("版本清单 | %d 个版本 | 第 %d 轮", len(versions), round_no + 1)
            return json.dumps(latest, ensure_ascii=False), versions
        except Exception as exc:      # noqa: BLE001
            last_error = exc
            log.warning("版本清单解析失败: %s", exc)
            time.sleep(0.4)

    raise NetworkError(f"版本清单所有源均失败: {last_error}")


def fetch_version_manifest(
    source: DownloadSource,
    timeout: float = 20.0,
) -> tuple[str, list[GameVersion]]:
    """Fetch the version manifest (with TTL cache)."""
    return _cached_fetch(source, timeout)


def filter_versions(
    versions: list[GameVersion],
    *,
    query: str = "",
    category: VersionType | str = VersionType.ALL,
) -> list[GameVersion]:
    """Filter *versions* by text *query* and *category*.

    Accepts both :class:`VersionType` enum values and plain strings
    (``"release"``, ``"snapshot"``, ``"old_beta"``, ``"all"``) for
    backward compatibility.
    """
    query = query.strip().lower()
    # Normalise *category* to a VersionType enum value
    if isinstance(category, str):
        # Map legacy string values to the current enum
        _LEGACY_CATEGORY_MAP = {
            "all": VersionType.ALL,
            "release": VersionType.RELEASE,
            "snapshot": VersionType.SNAPSHOT,
            "old": VersionType.OLD,
            "old_beta": VersionType.OLD,
        }
        category = _LEGACY_CATEGORY_MAP.get(category, VersionType.ALL)
    filtered: list[GameVersion] = []
    for version in versions:
        if category != VersionType.ALL and version.category != category:
            continue
        if query and query not in version.id.lower():
            continue
        filtered.append(version)
    return filtered
