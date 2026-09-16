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
"""全局下载限速器（令牌桶）。

设计要点
--------
* 全局唯一：所有连接共享同一个桶，因此"限速 5MB/s"是整条管道的上限，
  而不是每条连接 5MB/s。
* 可运行时改速：设置页改动即时生效，不需要重启下载。
* 两种消费方式：await limiter.consume(n)（asyncio 引擎用）与
  limiter.consume_sync(n)（同步/兼容路径用）。
* rate_bps <= 0 表示不限速（take() 直接返回 0）。
"""

from __future__ import annotations

import asyncio
import threading
import time

UNLIMITED = 0.0

__all__ = ["RateLimiter", "UNLIMITED", "parse_rate"]


class RateLimiter:
    """字节/秒级令牌桶，线程安全 + 协程安全。"""

    def __init__(self, rate_bps: float = UNLIMITED, burst_seconds: float = 0.25) -> None:
        self._lock = threading.Lock()
        self._rate = max(0.0, float(rate_bps or 0.0))
        self._burst = self._calc_burst(self._rate, burst_seconds)
        self._tokens = self._burst
        self._last = time.monotonic()

    @staticmethod
    def _calc_burst(rate: float, seconds: float = 0.25) -> float:
        """桶容量 = 0.25 秒的额度（64KB ~ 8MB）。

        容量越小限速越"硬"（小文件不会因为突发额度而超速），
        同时必须大于引擎的单次读取块，否则会出现等不满的情况。
        """
        if rate <= 0:
            return 0.0
        return max(64 * 1024.0, min(rate * seconds, 8 * 1024 * 1024.0))

    # ── 配置 ──────────────────────────────────────────────────────

    @property
    def rate(self) -> float:
        """当前速率（字节/秒），0 = 不限速。"""
        return self._rate

    def set_rate(self, rate_bps: float) -> None:
        """运行中改速（上调立刻放行，下调收敛到新桶容量）。"""
        with self._lock:
            self._rate = max(0.0, float(rate_bps or 0.0))
            self._burst = self._calc_burst(self._rate)
            self._tokens = min(self._tokens, self._burst) if self._rate else 0.0
            self._last = time.monotonic()

    # ── 令牌 ──────────────────────────────────────────────────────

    def _refill_locked(self) -> None:
        now = time.monotonic()
        delta = now - self._last
        self._last = now
        if self._rate > 0 and delta > 0:
            self._tokens = min(self._burst, self._tokens + delta * self._rate)

    def take(self, n: int) -> float:
        """尝试取 n 字节令牌；返回 0 表示已取到，否则返回还需等待的秒数。

        关键点：令牌不足时**不能把桶清零**——那会把"已经攒下的零头"丢掉，
        每次都要多等一轮，实测会把 5MB/s 限成 2MB/s。
        """
        if self._rate <= 0:
            return 0.0
        with self._lock:
            self._refill_locked()
            if n > self._burst:
                # 单次请求比桶容量还大：直接按"攒满即放行"处理，避免永远等不满
                self._tokens = 0.0
                return n / self._rate
            if self._tokens >= n:
                self._tokens -= n
                return 0.0
            return (n - self._tokens) / self._rate

    async def consume(self, n: int) -> None:
        """异步消费 n 字节令牌（超限则挂起等待）。"""
        while True:
            wait = self.take(n)
            if wait <= 0:
                return
            await asyncio.sleep(min(wait, 0.2))

    def consume_sync(self, n: int) -> None:
        """同步消费 n 字节令牌。"""
        while True:
            wait = self.take(n)
            if wait <= 0:
                return
            time.sleep(min(wait, 0.2))

    def __repr__(self) -> str:  # pragma: no cover
        rate = "unlimited" if self._rate <= 0 else f"{self._rate / 1024 / 1024:.2f}MB/s"
        return f"RateLimiter({rate})"


# ── 设置页输入解析 ────────────────────────────────────────────────

_UNITS = {
    "": 1, "b": 1, "k": 1024, "kb": 1024, "kib": 1024,
    "m": 1024 ** 2, "mb": 1024 ** 2, "mib": 1024 ** 2,
    "g": 1024 ** 3, "gb": 1024 ** 3, "gib": 1024 ** 3,
}


def parse_rate(text: str) -> float:
    """把 "10 MB/s" / "512kb" / "0" 解析成字节/秒；无法解析时返回不限速。"""
    raw = str(text or "").strip().lower().replace("/s", "").replace("ps", "").strip()
    if not raw or raw in ("0", "unlimited", "none", "off", "不限速"):
        return UNLIMITED
    num = ""
    for ch in raw:
        if ch.isdigit() or ch == ".":
            num += ch
        else:
            break
    unit = raw[len(num):].strip()
    try:
        value = float(num)
    except ValueError:
        return UNLIMITED
    return value * _UNITS.get(unit, 1)
