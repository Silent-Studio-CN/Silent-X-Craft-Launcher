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
"""下载源健康度（熔断器）—— 某条路连续失败就先别再浪费用户时间。

背景：BMCLAPI 有时会对某个 IP 直接丢包（实测 TCP 连接 84 秒才失败）。
如果每个文件都先试一遍镜像，1000 个资源就是几小时。所以：

* 同一主机连续失败 threshold 次 -> 进入 cooldown 秒的冷却期；
* 冷却期内所有候选都会被跳过（除非两条路都在冷却，那就只能硬着头皮试）；
* 冷却结束后自动恢复尝试。
"""

from __future__ import annotations

import threading
import time
from typing import Optional

__all__ = ["HostHealth", "health", "host_of", "is_hard_failure"]

DEFAULT_THRESHOLD = 2
DEFAULT_COOLDOWN = 90.0


def host_of(url: str) -> str:
    try:
        return url.split("//", 1)[1].split("/", 1)[0].lower()
    except Exception:
        return url.lower()


class HostHealth:
    """线程安全的主机熔断表。"""

    def __init__(self, threshold: int = DEFAULT_THRESHOLD, cooldown: float = DEFAULT_COOLDOWN) -> None:
        self.threshold = threshold
        self.cooldown = cooldown
        self._lock = threading.Lock()
        self._fails: dict[str, int] = {}
        self._down_until: dict[str, float] = {}

    def is_down(self, host: str) -> bool:
        with self._lock:
            until = self._down_until.get(host)
            if until is None:
                return False
            if time.monotonic() >= until:
                self._down_until.pop(host, None)
                self._fails.pop(host, None)
                return False
            return True

    def mark_ok(self, host: str) -> None:
        with self._lock:
            self._fails.pop(host, None)
            self._down_until.pop(host, None)

    def mark_fail(self, host: str, *, hard: bool = False) -> None:
        """记录一次失败。

        hard=True 用于"连不上/超时"这类硬失败 —— 立刻熔断，避免每个文件都
        先白等一次（实测某个源被限流时单次 TCP 连接要 20~80 秒）。
        """
        with self._lock:
            fails = self._fails.get(host, 0) + 1
            self._fails[host] = fails
            if hard:
                fails = max(fails, self.threshold)
                self._fails[host] = fails
            if fails >= self.threshold:
                self._down_until[host] = time.monotonic() + self.cooldown

    def remaining(self, host: str) -> float:
        with self._lock:
            until = self._down_until.get(host)
            if until is None:
                return 0.0
            return max(0.0, until - time.monotonic())

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "fails": dict(self._fails),
                "cooldown_remaining": {h: round(self.remaining(h), 1) for h in self._down_until},
            }

    def reset(self) -> None:
        with self._lock:
            self._fails.clear()
            self._down_until.clear()


_HEALTH = HostHealth()


def health() -> HostHealth:
    """全局熔断表。"""
    return _HEALTH


def is_hard_failure(exc: BaseException) -> bool:
    """连接层失败（连不上 / 超时 / 被重置）算硬失败，可以立即熔断该主机。

    实测：某个源被限流时单次 TCP 连接要 20~80 秒才失败，如果不立刻熔断，
    每个文件都要先白等一次。
    """
    import asyncio

    if isinstance(exc, (TimeoutError, ConnectionError, OSError, asyncio.IncompleteReadError)):
        return True
    name = type(exc).__name__
    return name in ("Timeout", "ReadTimeout", "ConnectTimeout", "RemoteDisconnected",
                    "ProtocolError", "SSLError", "ConnectionResetError")
