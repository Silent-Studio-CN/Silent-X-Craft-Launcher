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
"""下载源偏好记忆 —— 记住"上次哪条路更快"，下次直接首选它。

为什么需要：镜像被限流时，我们虽然会切到官方，但每个新进程都要先白等一次
（实测 BMCLAPI 被丢包时单次连接要 20~80 秒）。把实测速度和失败记录下来后，
"智能"模式可以在启动瞬间就选对源；镜像恢复后速度数据会自动把它拉回来。

数据存在 {config}/cache/sources.json，纯统计信息，删掉即回到默认顺序。
"""

from __future__ import annotations

import json
import threading
import time
from pathlib import Path
from typing import Iterable, Optional

from src.core.logger import log

__all__ = ["SourceStats", "stats", "DEFAULT_PRIOR_BPS"]

DEFAULT_PRIOR_BPS = 2 * 1024 * 1024.0   # 未知源的先验速度（2MB/s）
_EMA_ALPHA = 0.35                        # 新样本权重
_FAIL_PENALTY_WINDOW = 600.0             # 最近失败惩罚窗口（秒）
_MIN_BYTES_FOR_SPEED = 1024 * 1024       # 小于 1MB 的传输只算"活着"，不参与速度统计（小文件拼的是延迟）
_SAVE_INTERVAL = 5.0


class SourceStats:
    """各下载源的实测速度 / 失败记录（线程安全，落盘防丢）。"""

    def __init__(self, path: Optional[Path] = None) -> None:
        self.path = Path(path) if path else None
        self._lock = threading.RLock()
        self._data: dict[str, dict] = {}
        self._dirty = False
        self._last_save = 0.0
        self._load()

    # ── 落盘 ─────────────────────────────────────────────────────

    def _load(self) -> None:
        if not self.path or not self.path.exists():
            return
        try:
            payload = json.loads(self.path.read_text(encoding="utf-8"))
            if isinstance(payload, dict):
                self._data = {k: dict(v) for k, v in payload.items() if isinstance(v, dict)}
                log.debug("源偏好已加载: %s", self.snapshot())
        except Exception as exc:  # noqa: BLE001
            log.debug("源偏好读取失败(忽略): %s", exc)

    def save(self, force: bool = False) -> None:
        if not self.path:
            return
        now = time.monotonic()
        with self._lock:
            if not self._dirty:
                return
            if not force and now - self._last_save < _SAVE_INTERVAL:
                return
            self._last_save = now
            snapshot = json.dumps(self._data, ensure_ascii=False, indent=2)
            self._dirty = False
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.path.write_text(snapshot, encoding="utf-8")
        except OSError as exc:
            log.debug("源偏好写入失败(忽略): %s", exc)

    # ── 记录 ─────────────────────────────────────────────────────

    def _entry(self, name: str) -> dict:
        return self._data.setdefault(name, {
            "ema_bps": 0.0, "samples": 0, "last_ok": 0.0, "last_fail": 0.0, "fails": 0,
        })

    def record_success(self, name: str, byte_count: int = 0, seconds: float = 0.0) -> None:
        """记录一次成功（字节数太小则只记"活着"，不参与速度统计）。"""
        if not name:
            return
        with self._lock:
            entry = self._entry(name)
            entry["last_ok"] = time.time()
            entry["fails"] = 0
            if byte_count >= _MIN_BYTES_FOR_SPEED and seconds > 0.05:
                sample = byte_count / seconds
                if entry["samples"] == 0:
                    # 第一个样本与先验混合，避免"一次偶然很快"就长期霸榜
                    entry["ema_bps"] = 0.5 * DEFAULT_PRIOR_BPS + 0.5 * sample
                else:
                    entry["ema_bps"] = (1 - _EMA_ALPHA) * entry["ema_bps"] + _EMA_ALPHA * sample
                entry["samples"] = int(entry["samples"]) + 1
            self._dirty = True
            early = int(entry["samples"]) <= 2
        # 头两次样本立刻落盘，避免刚学到东西就被强杀进程丢掉
        self.save(force=early)

    def record_failure(self, name: str) -> None:
        """记录一次失败（硬失败会被 engine 额外触发熔断）。"""
        if not name:
            return
        with self._lock:
            entry = self._entry(name)
            entry["last_fail"] = time.time()
            entry["fails"] = int(entry.get("fails", 0)) + 1
            self._dirty = True
        # 失败必须立刻落盘：这正是"下次启动别再踩同一个坑"的依据
        self.save(force=True)

    # ── 评分 / 排序 ──────────────────────────────────────────────

    def _score_locked(self, name: str, now: float) -> float:
        """内部评分（调用方必须已持有 _lock）。"""
        entry = self._data.get(name)
        best = max((e.get("ema_bps", 0.0) for e in self._data.values()), default=0.0)
        if not entry or not entry.get("samples"):
            base = best or DEFAULT_PRIOR_BPS
            penalty = 0.1 if self._recent_fail(entry, now) else 0.85
            return base * penalty
        base = float(entry.get("ema_bps") or 0.0) or DEFAULT_PRIOR_BPS
        fails = int(entry.get("fails") or 0)
        if fails:
            base *= 0.5 ** min(fails, 4)
        if self._recent_fail(entry, now):
            base *= 0.25
        return base

    def score(self, name: str) -> float:
        """越大越优先。"""
        now = time.time()
        with self._lock:
            return self._score_locked(name, now)

    @staticmethod
    def _recent_fail(entry: Optional[dict], now: float) -> bool:
        if not entry:
            return False
        last_fail = float(entry.get("last_fail") or 0.0)
        return bool(last_fail) and (now - last_fail) < _FAIL_PENALTY_WINDOW

    def order(self, names: Iterable[str]) -> list[str]:
        """按实测偏好排序候选源名（同分保持原顺序，稳定）。"""
        items = list(names)
        return sorted(items, key=lambda n: -self.score(n))

    def snapshot(self) -> dict:
        now = time.time()
        with self._lock:
            return {
                name: {
                    "speed_mbps": round(float(e.get("ema_bps") or 0) / 1024 / 1024, 2),
                    "samples": e.get("samples", 0),
                    "fails": e.get("fails", 0),
                    "score_mbps": round(self._score_locked(name, now) / 1024 / 1024, 2),
                }
                for name, e in self._data.items()
            }

    def reset(self) -> None:
        with self._lock:
            self._data.clear()
            self._dirty = True
        self.save(force=True)


_STATS: Optional[SourceStats] = None


def stats() -> SourceStats:
    """全局源偏好表（惰性创建）。"""
    global _STATS
    if _STATS is None:
        path = None
        try:
            from src.core.platform import default_config_directory
            path = default_config_directory("SilentXCraftLauncher") / "cache" / "sources.json"
        except Exception:  # noqa: BLE001
            path = None
        _STATS = SourceStats(path)
    return _STATS
