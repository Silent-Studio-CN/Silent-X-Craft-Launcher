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
"""设置门面 —— 让 core / services 层不必反向 import UI 层。

现在唯一的设置存储是 QFluentWidgets 的 QConfig（src/app/common/launcher_config.py）。
services / core 想读设置时统一走这里，好处是：

* 依赖方向干净：core → services → app 单向，重构目录时可以整层平移；
* 没有 UI（例如命令行工具、脚本）时也能工作：读不到配置就用这里的默认值；
* 将来换成别的配置后端，只改这一个文件。

这是 core / services 里**唯一**允许 import src.app 的地方（其余模块一律走 settings）。
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Optional

__all__ = [
    "settings", "Settings",
]


class Settings:
    """对当前设置的只读/少量可写访问。"""

    # ── 内部：读 QConfig 某一项 ─────────────────────────────────

    @staticmethod
    def _value(name: str, default: Any) -> Any:
        try:
            from src.app.common.launcher_config import cfg
            item = getattr(cfg, name, None)
            if item is None:
                return default
            return getattr(item, "value", default)
        except Exception:      # 无 UI / 配置缺失 / 导入失败
            return default

    # ── 下载 ─────────────────────────────────────────────────────

    def download_source(self) -> str:
        """"auto" / "mojang" / "bmclapi"。"""
        value = self._value("downloadSource", "auto")
        return str(getattr(value, "value", value)).lower()

    def max_connections(self) -> int:
        try:
            return max(1, min(128, int(self._value("maxConnections", 32))))
        except (TypeError, ValueError):
            return 32

    def speed_limit_bps(self) -> float:
        try:
            return max(0.0, float(self._value("speedLimitKbps", 0)) * 1024.0)
        except (TypeError, ValueError):
            return 0.0

    def verify_sha1(self) -> bool:
        return bool(self._value("verifySha1", True))

    # ── 游戏 ─────────────────────────────────────────────────────

    def java_path(self) -> str:
        return str(self._value("javaPath", "") or "")

    def max_memory_mb(self) -> int:
        try:
            return int(self._value("maxMemoryMb", 4096))
        except (TypeError, ValueError):
            return 4096

    def game_directory(self) -> Path:
        raw = self._value("gameDirectory", "")
        if not raw:
            from src.core.platform import default_game_directory
            return default_game_directory()
        return Path(str(raw))

    def version_isolation(self) -> bool:
        return bool(self._value("versionIsolation", False))

    def username(self) -> str:
        return str(self._value("username", "Player") or "Player")

    # ── 可写项（自动检测到 Java 时回填）──────────────────────────

    def set_java_path(self, path: str) -> bool:
        try:
            from qfluentwidgets import qconfig
            from src.app.common.launcher_config import cfg
            qconfig.set(cfg.javaPath, str(path))
            return True
        except Exception:
            return False


settings = Settings()
