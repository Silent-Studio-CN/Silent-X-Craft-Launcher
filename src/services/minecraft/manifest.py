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
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional

import requests

from src.core.constants import DownloadSource, VersionType
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


_DEBUG_LOG = Path(__file__).resolve().parents[4] / "debug-958f80.log"


def _agent_log(location: str, message: str, data: dict, hypothesis_id: str) -> None:
    payload = {
        "sessionId": "958f80",
        "runId": "version-manifest",
        "hypothesisId": hypothesis_id,
        "location": location,
        "message": message,
        "data": data,
        "timestamp": int(datetime.now().timestamp() * 1000),
    }
    try:
        with _DEBUG_LOG.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(payload, ensure_ascii=False) + "\n")
    except OSError:
        pass


@dataclass(frozen=True)
class GameVersion:
    id: str
    version_type: str
    url: str
    release_time: str

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
            return dt.strftime("%Y-%m-%d %H:%M")
        except ValueError:
            return self.release_time


def _do_fetch(
    source: DownloadSource,
    timeout: float = 20.0,
) -> tuple[str, list[GameVersion]]:
    """Perform the actual HTTP fetch (uncached)."""
    url = source.manifest_url

    _agent_log(
        "manifest.py:fetch_version_manifest",
        "fetching manifest",
        {"url": url, "source": source.value},
        "H2",
    )

    response = requests.get(url, timeout=timeout)
    if response.status_code in (403, 429):
        log.warning("版本清单 | HTTP %d 获取失败 (URL: %s), 稍后重试可恢复",
                    response.status_code, url[:80])
    response.raise_for_status()
    payload = response.json()

    latest = payload.get("latest", {})
    versions: list[GameVersion] = []
    for item in payload.get("versions", []):
        versions.append(
            GameVersion(
                id=item.get("id", ""),
                version_type=item.get("type", "old"),
                url=item.get("url", ""),
                release_time=item.get("releaseTime", ""),
            )
        )

    _agent_log(
        "manifest.py:fetch_version_manifest",
        "manifest fetched",
        {
            "count": len(versions),
            "latest": latest,
            "first": versions[0].id if versions else None,
        },
        "H2",
    )

    return json.dumps(latest, ensure_ascii=False), versions


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
