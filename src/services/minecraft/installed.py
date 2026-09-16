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
"""已安装版本的扫描与读取（从旧的 download_service.py 搬过来）。

只做"读磁盘"的事，不涉及任何下载逻辑 —— 这样下载层重构/删除都不会影响它。
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import List, Optional

from src.core.logger import log, log_exception
from src.core.settings import settings

__all__ = ["get_installed_versions", "get_version_info"]


def get_installed_versions(game_dir: Optional[Path] = None) -> List[str]:
    """列出 game_dir/versions 下"jar + json 都齐全"的版本（倒序）。"""
    try:
        base = Path(game_dir) if game_dir else settings.game_directory()
        versions_dir = base / "versions"
        if not versions_dir.exists():
            return []

        versions: List[str] = []
        for version_dir in versions_dir.iterdir():
            if not version_dir.is_dir():
                continue
            jar_path = version_dir / f"{version_dir.name}.jar"
            json_path = version_dir / f"{version_dir.name}.json"
            if jar_path.exists() and json_path.exists():
                versions.append(version_dir.name)
        return sorted(versions, reverse=True)
    except Exception:
        log_exception(log, "get_installed_versions 失败")
        return []


def get_version_info(version_id: str, game_dir: Optional[Path] = None) -> Optional[dict]:
    """读取某个版本的 JSON（不存在/坏 JSON 返回 None）。"""
    try:
        base = Path(game_dir) if game_dir else settings.game_directory()
        json_path = base / "versions" / version_id / f"{version_id}.json"
        if not json_path.exists():
            return None
        with open(json_path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        log_exception(log, f"get_version_info 失败: {version_id}")
        return None
