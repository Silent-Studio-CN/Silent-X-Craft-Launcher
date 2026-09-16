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
"""文件完整性校验 —— 凡是 Mojang 给了 SHA1 的资源都必须核对。

校验矩阵（全部来自官方元数据，不允许跳过）
------------------------------------------
| 资源              | 哈希来源                                       |
|-------------------|------------------------------------------------|
| 版本清单          | version_manifest_v2.json 里每个版本的 sha1      |
| 版本 JSON         | 同上                                            |
| 客户端/服务端 jar | downloads.client.sha1 / server.sha1             |
| 依赖库            | downloads.artifact.sha1 与 classifiers.*.sha1   |
| 资源索引          | assetIndex.sha1                                 |
| 资源对象          | 文件名即 sha1（objects[*].hash）                |
| 官方 JRE 文件     | files[*].downloads.raw.sha1                     |

为了不每次启动都重算几千个 assets 的哈希，这里带一个磁盘缓存
（cache/hashes.json），键为 路径|大小|mtime。
"""

from __future__ import annotations

import hashlib
import json
import os
import threading
from pathlib import Path
from typing import Iterable, Optional

from src.core.logger import log

HASH_CHUNK = 4 << 20  # 4MB
_CACHE_VERSION = 1

__all__ = [
    "sha1_file", "sha1_bytes", "verify_file", "HashCache", "global_hash_cache",
    "HashMismatch", "HashResult",
]


class HashMismatch(Exception):
    """哈希/大小校验失败。"""


class HashResult:
    """校验结果。"""

    __slots__ = ("ok", "reason", "actual_sha1", "cached")

    def __init__(self, ok: bool, reason: str = "", actual_sha1: str = "", cached: bool = False):
        self.ok = ok
        self.reason = reason
        self.actual_sha1 = actual_sha1
        self.cached = cached

    def __bool__(self) -> bool:
        return self.ok

    def __repr__(self) -> str:  # pragma: no cover
        return f"HashResult(ok={self.ok}, reason={self.reason!r}, cached={self.cached})"


def sha1_file(path: str | Path, chunk: int = HASH_CHUNK) -> str:
    """流式计算文件 SHA1（4MB 分块）。"""
    h = hashlib.sha1()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(chunk), b""):
            h.update(block)
    return h.hexdigest()


def sha1_bytes(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


class HashCache:
    """路径|大小|mtime → sha1 的磁盘缓存（避免重复哈希几千个资源文件）。"""

    def __init__(self, path: Optional[Path] = None) -> None:
        self.path = Path(path) if path else None
        self._lock = threading.Lock()
        self._data: dict[str, str] = {}
        self._dirty = False
        self._load()

    # ── 持久化 ───────────────────────────────────────────────────

    def _load(self) -> None:
        if not self.path or not self.path.exists():
            return
        try:
            payload = json.loads(self.path.read_text(encoding="utf-8"))
            if payload.get("version") == _CACHE_VERSION:
                self._data = dict(payload.get("entries", {}))
        except Exception as exc:  # 缓存坏了不影响功能
            log.debug("哈希缓存读取失败(忽略): %s", exc)

    def save(self) -> None:
        if not self.path:
            return
        with self._lock:
            if not self._dirty:
                return
            try:
                self.path.parent.mkdir(parents=True, exist_ok=True)
                payload = {"version": _CACHE_VERSION, "entries": self._data}
                self.path.write_text(json.dumps(payload), encoding="utf-8")
                self._dirty = False
            except Exception as exc:
                log.debug("哈希缓存写入失败(忽略): %s", exc)

    # ── 读写 ─────────────────────────────────────────────────────

    @staticmethod
    def _key(path: Path, size: int, mtime_ns: int) -> str:
        return f"{path}|{size}|{mtime_ns}"

    def get(self, path: Path, stat: os.stat_result) -> Optional[str]:
        with self._lock:
            return self._data.get(self._key(path, stat.st_size, stat.st_mtime_ns))

    def put(self, path: Path, stat: os.stat_result, digest: str) -> None:
        with self._lock:
            if len(self._data) > 20000:  # 简单上限，防止无限增长
                self._data.clear()
            self._data[self._key(path, stat.st_size, stat.st_mtime_ns)] = digest
            self._dirty = True

    def prune_missing(self, roots: Iterable[Path]) -> None:
        """删除已不存在文件的缓存项（版本目录被清理后调用）。"""
        with self._lock:
            keep = {k: v for k, v in self._data.items() if Path(k.split("|", 1)[0]).exists()}
            if len(keep) != len(self._data):
                self._data = keep
                self._dirty = True


_GLOBAL: Optional[HashCache] = None


def global_hash_cache() -> HashCache:
    """全局哈希缓存（惰性创建，路径在配置目录下）。"""
    global _GLOBAL
    if _GLOBAL is None:
        try:
            from src.core.platform import default_config_directory
            path = default_config_directory("SilentXCraftLauncher") / "cache" / "hashes.json"
        except Exception:
            path = None
        _GLOBAL = HashCache(path)
    return _GLOBAL


def verify_file(
    path: str | Path,
    *,
    sha1: Optional[str] = None,
    size: int = 0,
    use_cache: bool = True,
    cache: Optional[HashCache] = None,
) -> HashResult:
    """校验文件大小 + SHA1。

    * 没有给 sha1 时只校验大小（size 为 0 表示不校验）。
    * sha1 匹配时把结果写入缓存；再次校验同一文件直接命中缓存。
    """
    p = Path(path)
    if not p.is_file():
        return HashResult(False, "文件不存在")
    stat = p.stat()
    if size and stat.st_size != size:
        return HashResult(False, f"大小不符 (期望 {size}, 实际 {stat.st_size})")
    if not sha1:
        return HashResult(True, "仅校验大小")

    cache = cache if cache is not None else (global_hash_cache() if use_cache else None)
    expected = sha1.strip().lower()
    if cache is not None:
        cached = cache.get(p, stat)
        if cached:
            return HashResult(cached == expected, "缓存命中" if cached == expected else "缓存哈希不符",
                              actual_sha1=cached, cached=True)

    actual = sha1_file(p)
    if cache is not None:
        cache.put(p, stat, actual)
    if actual != expected:
        return HashResult(False, f"SHA1 不符 (期望 {expected[:12]}..., 实际 {actual[:12]}...)",
                          actual_sha1=actual)
    return HashResult(True, "SHA1 通过", actual_sha1=actual)
