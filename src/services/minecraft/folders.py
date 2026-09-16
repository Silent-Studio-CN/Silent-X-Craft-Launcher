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
"""游戏目录检测 —— 别让用户自己填路径。

扫描顺序照 PCL 的思路（它的报告写得很清楚：便携目录、APPDATA 下的 .minecraft、
用户目录、以及"任意带 versions/ 的文件夹"）：

    1. 启动器所在目录（便携版习惯：SXCL.exe 旁边的 .minecraft）
    2. APPDATA/.minecraft（官方启动器；HMCL 也爱往这儿塞）
    3. 用户主目录下的 .minecraft
    4. 桌面上的 .minecraft（真的有用户这么放）
    5. 当前配置里的目录（哪怕是空的也列出来，让用户知道现在用的是哪个）

每个候选都会**实际数一下 versions/ 里有多少版本**（含只有 JSON 没有 jar 的加载器版本），
这样"装了 12 个版本的那个目录"会排在"刚建的空目录"前面。
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from src.core.logger import log
from src.core.settings import settings

__all__ = ["GameFolder", "detect_game_folders", "best_game_folder", "describe"]


@dataclass(frozen=True)
class GameFolder:
    """一个候选游戏目录。"""

    path: Path
    label: str = ""            # 这是谁留下的目录（官方 / 便携 / 桌面…）
    versions: int = 0          # versions/ 下的版本数
    has_assets: bool = False   # 有 assets/ 说明真的玩过
    has_launcher_profiles: bool = False

    @property
    def exists(self) -> bool:
        return self.path.is_dir()

    @property
    def score(self) -> int:
        """排序用：有版本 > 有资源 > 有 launcher_profiles。"""
        if not self.exists:
            return -1
        value = self.versions * 10
        if self.has_assets:
            value += 5
        if self.has_launcher_profiles:
            value += 2
        return value


def _count_versions(folder: Path) -> int:
    versions_dir = folder / "versions"
    if not versions_dir.is_dir():
        return 0
    count = 0
    try:
        for item in versions_dir.iterdir():
            if not item.is_dir():
                continue
            if (item / f"{item.name}.json").is_file() or (item / f"{item.name}.jar").is_file():
                count += 1
            elif any(p.suffix == ".json" for p in item.glob("*.json")):
                count += 1           # 加载器版本常常只有 JSON（jar 靠继承）
    except Exception:
        return 0
    return count


def _inspect(path: Path, label: str) -> GameFolder:
    return GameFolder(
        path=path,
        label=label,
        versions=_count_versions(path),
        has_assets=(path / "assets").is_dir(),
        has_launcher_profiles=(path / "launcher_profiles.json").is_file(),
    )


def _program_dir() -> Optional[Path]:
    """启动器自己所在的目录（便携版就把 .minecraft 放它旁边）。"""
    import sys
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parents[3]


def detect_game_folders(include_configured: bool = True) -> List[GameFolder]:
    """列出所有候选游戏目录，按"像不像真的在用的那个"排序。"""
    candidates: List[tuple] = []

    program = _program_dir()
    if program is not None:
        for name in (".minecraft", "minecraft", "MC"):
            candidates.append((program / name, "启动器目录（便携）"))

    appdata = os.environ.get("APPDATA")
    if appdata:
        candidates.append((Path(appdata) / ".minecraft", "官方启动器（APPDATA）"))
    home = Path.home()
    candidates.append((home / ".minecraft", "用户目录"))
    for desktop in ("Desktop", "桌面"):
        candidates.append((home / desktop / ".minecraft", "桌面"))

    if include_configured:
        try:
            configured = settings.game_directory()
            if configured:
                candidates.append((Path(configured), "当前配置"))
        except Exception:
            pass

    seen: dict = {}
    for path, label in candidates:
        try:
            key = str(Path(path).resolve()).lower()
        except Exception:
            key = str(path).lower()
        if key in seen:
            continue
        seen[key] = _inspect(Path(path), label)

    folders = [item for item in seen.values() if item.exists]
    folders.sort(key=lambda item: item.score, reverse=True)
    log.info("[目录检测] 找到 %d 个候选: %s", len(folders),
             "、".join(f"{item.path}({item.versions} 版本)" for item in folders) or "无")
    return folders


def best_game_folder() -> Optional[GameFolder]:
    """挑一个最像"用户平时在玩的"目录（有版本数的优先）。"""
    folders = detect_game_folders()
    if not folders:
        return None
    with_versions = [item for item in folders if item.versions > 0]
    return (with_versions or folders)[0]


def describe(folder: GameFolder) -> str:
    return f"{folder.path}（{folder.label}，{folder.versions} 个版本）"
