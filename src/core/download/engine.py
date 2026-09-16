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
"""自研异步下载引擎 —— asyncio + 标准库，无第三方依赖。

为什么这样设计才能跑满带宽
--------------------------
1. **单线程事件循环 + 几十条并发连接**：完全没有线程切换/GIL 争用，
   并发只受 semaphore 限制（默认 32，可调到 128）。
2. **大文件多连接分片（HTTP Range）**：单个大文件也能吃满带宽，
   而不是"一个文件一条连接"。
3. **预分配 + 直接定位写入**：不再"下载成 N 个分片文件再合并"，
   省掉一整轮磁盘读写（旧引擎 37MB 文件要写 8 个临时文件再拼一遍）。
4. **连接复用（keep-alive 池）**：几千个小资源文件不必每次都握手/TLS。
5. **全局限速**：所有连接共用一个令牌桶，限速值就是管道总上限。
6. **全量 SHA1 校验**：Mojang 给了哈希就必校验，不通过即删档换源重下。

线程模型
--------
对外是同步 API：download() 内部用 asyncio.run() 跑完整个批次（调用方放在
QThread/工作线程里即可）。也提供 download_async() 供已有事件循环的调用方使用。
"""

from __future__ import annotations

import asyncio
import json
import os
import ssl
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Optional, Sequence
from urllib.parse import urljoin, urlsplit

from src.core.health import health, host_of, is_hard_failure
from src.core.source_stats import stats
from src.core.download.limiter import RateLimiter
from src.core.download.spec import FileSpec
from src.core.download.verify import global_hash_cache, sha1_file, verify_file
from src.core.logger import log

__all__ = ["AsyncDownloadEngine", "DownloadResult", "ProgressInfo", "HttpError", "Cancelled",
           "default_engine", "download_specs"]

READ_BLOCK = 1024 * 1024          # 不限速时每次读 1MB
MIN_READ_BLOCK = 64 * 1024        # 限速时最细读取粒度
MIN_CHUNK = 2 * 1024 * 1024       # 分片下载的最小片大小
HEADER_LIMIT = 128 * 1024
USER_AGENT = "SilentXCraftLauncher/1.0 (+https://github.com/Silent-Studio-CN)"

# 换源阈值：不限速时，若某个源持续低于该速度就换下一条候选路（实测镜像有时会
# 掉到 200KB/s 而官方有 10MB/s，这条规则能自动把带宽抢回来）
MIN_SOURCE_SPEED = 512 * 1024      # 字节/秒
SLOW_SOURCE_GRACE = 8.0            # 秒


class Cancelled(Exception):
    """用户取消。"""


class HttpError(Exception):
    """HTTP 层错误（含状态码）。"""

    def __init__(self, status: int, reason: str, url: str):
        super().__init__(f"HTTP {status} {reason} | {url[:120]}")
        self.status = status
        self.reason = reason
        self.url = url


# ── 进度 / 结果 ───────────────────────────────────────────────────


@dataclass
class ProgressInfo:
    """进度快照（由引擎节流后回调给 UI）。"""

    files_total: int = 0
    files_done: int = 0
    files_failed: int = 0
    files_skipped: int = 0
    bytes_total: int = 0
    bytes_done: int = 0
    speed_bps: float = 0.0
    eta_seconds: float = 0.0
    current: str = ""
    sources: dict = field(default_factory=dict)   # source_name -> 文件数

    @property
    def percent(self) -> float:
        if self.bytes_total <= 0:
            return 100.0 * self.files_done / max(1, self.files_total)
        return min(100.0, 100.0 * self.bytes_done / self.bytes_total)


@dataclass
class DownloadResult:
    """单个文件的下载结果。"""

    spec: FileSpec
    ok: bool = False
    skipped: bool = False
    bytes_downloaded: int = 0
    seconds: float = 0.0
    source: str = ""
    attempts: int = 0
    error: str = ""

    @property
    def path(self) -> Path:
        return self.spec.dest

    @property
    def name(self) -> str:
        return self.spec.name

    def __repr__(self) -> str:  # pragma: no cover
        state = "skip" if self.skipped else ("ok" if self.ok else "FAIL")
        return f"DownloadResult({self.name}, {state}, {self.bytes_downloaded}B, {self.source})"


# ── HTTP/1.1 客户端（asyncio + 连接池）────────────────────────────


def _split_url(url: str):
    parts = urlsplit(url)
    scheme = parts.scheme.lower() or "https"
    host = parts.hostname or ""
    port = parts.port or (443 if scheme == "https" else 80)
    path = parts.path or "/"
    if parts.query:
        path += "?" + parts.query
    return scheme, host, port, path


class _ConnectionPool:
    """按 (scheme, host, port) 复用空闲连接。"""

    def __init__(self, idle_limit: int = 16) -> None:
        self._idle: dict[tuple, list] = {}
        self._idle_limit = idle_limit
        self._ssl: Optional[ssl.SSLContext] = None
        self._created = 0
        self._reused = 0

    def ssl_context(self) -> ssl.SSLContext:
        if self._ssl is None:
            ctx = ssl.create_default_context()
            try:
                ctx.set_alpn_protocols(["http/1.1"])
            except NotImplementedError:  # pragma: no cover
                pass
            self._ssl = ctx
        return self._ssl

    async def acquire(self, scheme: str, host: str, port: int):
        key = (scheme, host, port)
        while self._idle.get(key):
            reader, writer = self._idle[key].pop()
            if writer.is_closing():
                continue
            self._reused += 1
            return reader, writer
        ssl_ctx = self.ssl_context() if scheme == "https" else None
        reader, writer = await asyncio.open_connection(
            host, port, ssl=ssl_ctx, server_hostname=host if ssl_ctx else None,
            limit=HEADER_LIMIT,
        )
        self._created += 1
        return reader, writer

    def release(self, scheme: str, host: str, port: int, reader, writer) -> None:
        key = (scheme, host, port)
        bucket = self._idle.setdefault(key, [])
        if len(bucket) >= self._idle_limit or writer.is_closing():
            writer.close()
            return
        bucket.append((reader, writer))

    async def close_all(self) -> None:
        for bucket in self._idle.values():
            for _reader, writer in bucket:
                try:
                    writer.close()
                except Exception:
                    pass
        self._idle.clear()

    @property
    def stats(self) -> tuple[int, int]:
        return self._created, self._reused


class HttpResponse:
    """极简 HTTP/1.1 响应：支持 Content-Length 与 chunked。"""

    __slots__ = ("status", "reason", "headers", "url", "_reader", "_writer",
                 "_pool", "_key", "keep_alive", "_consumed")

    def __init__(self, status, reason, headers, url, reader, writer, pool, key):
        self.status = status
        self.reason = reason
        self.headers = headers
        self.url = url
        self._reader = reader
        self._writer = writer
        self._pool = pool
        self._key = key
        self.keep_alive = headers.get("connection", "").lower() != "close"
        self._consumed = False

    # ── 元信息 ───────────────────────────────────────────────────

    @property
    def content_length(self) -> Optional[int]:
        raw = self.headers.get("content-length")
        if raw is None:
            return None
        try:
            return int(raw)
        except ValueError:
            return None

    @property
    def content_range(self) -> Optional[tuple]:
        """解析 Content-Range: bytes 0-1023/4096 -> (0, 1023, 4096)。"""
        raw = self.headers.get("content-range")
        if not raw or "/" not in raw:
            return None
        try:
            span, total = raw.split("/", 1)
            start_end = span.split(" ", 1)[1]
            start, end = start_end.split("-", 1)
            return int(start), int(end), int(total)
        except Exception:
            return None

    @property
    def is_chunked(self) -> bool:
        return "chunked" in self.headers.get("transfer-encoding", "").lower()

    # ── 读取 ─────────────────────────────────────────────────────

    async def _read_chunked(self, block: int):
        while True:
            line = await self._reader.readline()
            if not line:
                return
            size_text = line.split(b";", 1)[0].strip()
            try:
                size = int(size_text, 16)
            except ValueError:
                return
            if size == 0:
                await self._reader.readline()  # 末尾 CRLF
                return
            remaining = size
            while remaining > 0:
                data = await self._reader.read(min(block, remaining))
                if not data:
                    return
                remaining -= len(data)
                yield data
            await self._reader.readline()  # 块尾 CRLF

    async def iter_body(self, block: int = READ_BLOCK):
        """按块产出响应体。

        任何异常或调用方提前 break（GeneratorExit）都会把连接标记为不可复用，
        避免半读的连接被下一次请求复用而串包。
        """
        self._consumed = True
        try:
            if self.is_chunked:
                async for data in self._read_chunked(block):
                    yield data
                return
            remaining = self.content_length
            if remaining is None:
                while True:
                    data = await self._reader.read(block)
                    if not data:
                        return
                    yield data
            while remaining is not None and remaining > 0:
                data = await self._reader.read(min(block, remaining))
                if not data:
                    raise asyncio.IncompleteReadError(partial=b"", expected=remaining)
                remaining -= len(data)
                yield data
        except BaseException:
            self.keep_alive = False
            raise

    async def drain(self) -> None:
        """读干净响应体，以便连接可以复用。"""
        if self._consumed:
            return
        try:
            async for _ in self.iter_body(READ_BLOCK):
                pass
        except Exception:
            self.keep_alive = False

    def close(self) -> None:
        """归还或关闭连接。"""
        if self._writer is None:
            return
        if self.keep_alive and not self._writer.is_closing():
            self._pool.release(*self._key, self._reader, self._writer)
        else:
            try:
                self._writer.close()
            except Exception:
                pass
        self._writer = None


async def http_get(
    url: str,
    *,
    pool: _ConnectionPool,
    headers: Optional[dict] = None,
    timeout: float = 30.0,
    max_redirects: int = 5,
) -> HttpResponse:
    """发起 GET，自动跟随重定向（最多 max_redirects 次）。"""
    current = url
    for _hop in range(max_redirects + 1):
        scheme, host, port, path = _split_url(current)
        if not host:
            raise HttpError(0, "URL 无效", current)
        reader, writer = await asyncio.wait_for(pool.acquire(scheme, host, port), timeout=timeout)

        req_headers = {
            "Host": host if port in (80, 443) else host + ":" + str(port),
            "User-Agent": USER_AGENT,
            "Accept": "*/*",
            "Accept-Encoding": "identity",
            "Connection": "keep-alive",
        }
        if headers:
            req_headers.update(headers)

        head = "GET " + path + " HTTP/1.1\r\n"
        for key, value in req_headers.items():
            head += key + ": " + str(value) + "\r\n"
        head += "\r\n"
        writer.write(head.encode("latin-1"))
        await asyncio.wait_for(writer.drain(), timeout=timeout)

        status_line = await asyncio.wait_for(reader.readline(), timeout=timeout)
        if not status_line:
            raise HttpError(0, "连接被关闭", current)
        try:
            parts = status_line.decode("latin-1").rstrip("\r\n").split(" ", 2)
            status = int(parts[1])
            reason = parts[2] if len(parts) > 2 else ""
        except Exception:
            raise HttpError(0, "响应非法", current)

        resp_headers = {}
        while True:
            line = await asyncio.wait_for(reader.readline(), timeout=timeout)
            if line in (b"\r\n", b"\n", b""):
                break
            text = line.decode("latin-1").rstrip("\r\n")
            if ":" in text:
                key, value = text.split(":", 1)
                resp_headers[key.strip().lower()] = value.strip()

        resp = HttpResponse(status, reason, resp_headers, current, reader, writer, pool,
                            (scheme, host, port))

        if status in (301, 302, 303, 307, 308) and resp_headers.get("location"):
            location = urljoin(current, resp_headers["location"])
            await resp.drain()
            resp.keep_alive = False   # 换目标了，连接不复用
            resp.close()
            current = location
            continue
        return resp

    raise HttpError(0, "重定向次数过多", url)


# ── 引擎 ─────────────────────────────────────────────────────────


class AsyncDownloadEngine:
    """异步并发下载引擎（对外同步接口）。"""

    def __init__(
        self,
        *,
        max_connections: int = 32,
        max_parts_per_file: int = 16,
        chunk_size: int = 4 * 1024 * 1024,
        limiter: Optional[RateLimiter] = None,
        timeout: float = 30.0,
        retries: int = 2,
        verify: bool = True,
        resume: bool = True,
    ) -> None:
        self.max_connections = max(1, int(max_connections))
        self.max_parts_per_file = max(1, int(max_parts_per_file))
        self.chunk_size = max(256 * 1024, int(chunk_size))
        self.limiter = limiter or RateLimiter(0)
        self.timeout = float(timeout)
        self.retries = max(0, int(retries))
        self.verify = bool(verify)
        self.resume = bool(resume)

        self._pool: Optional[_ConnectionPool] = None
        self._sem: Optional[asyncio.Semaphore] = None
        self._run_lock = threading.RLock()   # 同一个引擎实例串行使用（内部状态非线程安全）
        self._progress = ProgressInfo()
        self._lock = threading.Lock()
        self._speed_samples: list[tuple[float, int]] = []
        self._last_emit = 0.0
        self._on_progress: Optional[Callable[[ProgressInfo], None]] = None
        self._cancel: Optional[threading.Event] = None

    # ── 对外接口 ─────────────────────────────────────────────────

    def download(
        self,
        specs: Sequence[FileSpec],
        *,
        on_progress: Optional[Callable[[ProgressInfo], None]] = None,
        cancel: Optional[threading.Event] = None,
        progress_interval: float = 0.15,
    ) -> list[DownloadResult]:
        """同步下载一批文件（建议在工作线程中调用）。"""
        with self._run_lock:
            return asyncio.run(self.download_async(
                specs, on_progress=on_progress, cancel=cancel,
                progress_interval=progress_interval))

    async def download_async(
        self,
        specs: Sequence[FileSpec],
        *,
        on_progress: Optional[Callable[[ProgressInfo], None]] = None,
        cancel: Optional[threading.Event] = None,
        progress_interval: float = 0.15,
    ) -> list[DownloadResult]:
        specs = [s for s in specs if s is not None]
        self._pool = _ConnectionPool()
        # Windows proactor 在服务器提前断开时会往事件循环里抛 ConnectionResetError，
        # 属于噪音：连接反正要重建，吞掉即可。
        try:
            asyncio.get_running_loop().set_exception_handler(_quiet_exception_handler)
        except Exception:
            pass
        self._sem = asyncio.Semaphore(self.max_connections)
        self._cancel = cancel
        self._on_progress = on_progress
        self._progress = ProgressInfo(files_total=len(specs))
        self._progress.bytes_total = sum(max(0, s.size) for s in specs)
        self._speed_samples = [(time.monotonic(), 0)]
        self._last_emit = 0.0

        watcher = asyncio.create_task(self._progress_loop(progress_interval)) if on_progress else None
        try:
            if cancel is not None and cancel.is_set():
                raise Cancelled()
            results = await asyncio.gather(
                *(self._one_with_sem(spec) for spec in specs), return_exceptions=True)
        finally:
            if watcher:
                watcher.cancel()
                try:
                    await watcher
                except (asyncio.CancelledError, Exception):
                    pass
            if self._on_progress:
                self._emit_progress(force=True)
            await self._pool.close_all()
            cache = global_hash_cache()
            cache.save()
            stats().save(force=True)

        out: list[DownloadResult] = []
        for spec, item in zip(specs, results):
            if isinstance(item, DownloadResult):
                out.append(item)
            elif isinstance(item, BaseException):
                out.append(DownloadResult(spec=spec, ok=False, error=f"{type(item).__name__}: {item}"))
            else:  # pragma: no cover
                out.append(DownloadResult(spec=spec, ok=False, error="未知结果"))
        return out

    # ── 内部：单个文件 ───────────────────────────────────────────

    async def _one_with_sem(self, spec: FileSpec) -> DownloadResult:
        # 并发闸门加在"每条连接"上（见 _fetch / _fetch_stream / _fetch_parts），
        # 这样一个大文件的多分片能与其它文件公平共享带宽，而不会独占名额。
        return await self._download_spec(spec)

    def _check_cancel(self) -> None:
        if self._cancel is not None and self._cancel.is_set():
            raise Cancelled()

    async def _download_spec(self, spec: FileSpec) -> DownloadResult:
        started = time.monotonic()
        result = DownloadResult(spec=spec)

        # 1) 已经下好且校验通过 -> 跳过
        if spec.dest.is_file():
            state = verify_file(spec.dest, sha1=spec.sha1 if self.verify else None,
                                size=spec.size, use_cache=True)
            if state.ok:
                result.ok = True
                result.skipped = True
                result.source = "cache"
                self._mark_done(result)
                return result
            log.warning("下载 | 已存在但校验失败，重新下载: %s (%s)", spec.name, state.reason)
            try:
                spec.dest.unlink()
            except OSError:
                pass

        # 2) 逐个候选源 / 逐个重试
        last_error = ""
        candidates = list(spec.candidates) or [("official", spec.url)]
        # 熔断：某条路刚刚连续失败过就直接跳过（两条都挂了才被迫全试）
        alive = [c for c in candidates if not health().is_down(host_of(c[1]))]
        if alive:
            candidates = alive
        else:
            log.warning("下载 | %s | 两条路都在冷却期，仍尝试全部候选", spec.name)
        # 轮转而不是"死磕首选源"：镜像节点抽风时立刻换官方路，第二个轮次才回头重试
        plan = [(name, url) for _round in range(1 + self.retries) for name, url in candidates]
        for source_name, url in plan:
            self._check_cancel()
            result.attempts += 1
            try:
                level = log.info if (spec.size >= 4 * 1024 * 1024 or spec.kind in
                                     ("client-jar", "asset-index", "loader", "java-runtime")) else log.debug
                level("下载 | %s | 源=%s | 尝试%d | %s", spec.name, source_name,
                      result.attempts, url[:110])
                await self._fetch(spec, url, source_name)
                state = await asyncio.get_running_loop().run_in_executor(
                    None, lambda: verify_file(
                        spec.dest, sha1=spec.sha1 if self.verify else None,
                        size=spec.size, use_cache=self.verify))
                if not state.ok:
                    raise HttpError(0, f"校验失败: {state.reason}", url)
                health().mark_ok(host_of(url))
                result.ok = True
                result.source = source_name
                result.bytes_downloaded = spec.bytes_done
                result.seconds = time.monotonic() - started
                # 记住"这条路实测有多快"，供智能模式下次直接选对源
                stats().record_success(source_name, result.bytes_downloaded, result.seconds)
                self._progress.sources[source_name] = self._progress.sources.get(source_name, 0) + 1
                level = log.info if (spec.size >= 4 * 1024 * 1024 or spec.kind in
                                     ("client-jar", "asset-index", "loader", "java-runtime")) else log.debug
                level("下载 | 完成 %s | %.1f MB | %.1fs | 源=%s", spec.name,
                      spec.dest.stat().st_size / 1024 / 1024, result.seconds, source_name)
                self._mark_done(result)
                return result
            except Cancelled:
                raise
            except Exception as exc:  # noqa: BLE001 —— 网络层什么都可能抛
                last_error = f"{type(exc).__name__}: {exc}"
                health().mark_fail(host_of(url), hard=is_hard_failure(exc))
                stats().record_failure(source_name)
                log.warning("下载 | 失败 %s | 源=%s | %s", spec.name, source_name, last_error[:160])
                # 有 sha1 说明内容唯一，已下的分片换源后依然有效 -> 保留以便续传
                if not spec.sha1:
                    self._cleanup_partial(spec)
                await asyncio.sleep(0.3)
                continue

        result.ok = False
        result.seconds = time.monotonic() - started
        result.error = last_error or "所有下载源均失败"
        self._mark_done(result, failed=True)
        log.error("下载 | 放弃 %s | %s", spec.name, result.error[:200])
        return result

    def _mark_done(self, result: DownloadResult, failed: bool = False) -> None:
        self._progress.files_done += 1
        if failed:
            self._progress.files_failed += 1
        elif result.skipped:
            self._progress.files_skipped += 1
        if result.skipped and result.spec.size:
            self._progress.bytes_done += result.spec.size
        self._progress.current = result.spec.name

    # ── 内部：抓取一个 URL ───────────────────────────────────────

    async def _fetch(self, spec: FileSpec, url: str, source_name: str) -> None:
        part = self._part_path(spec)
        part.parent.mkdir(parents=True, exist_ok=True)

        # 已知大小的小文件：不做 Range 探测，直接下（几千个 assets 的关键优化）
        if 0 < spec.size < MIN_CHUNK:
            await self._fetch_small(spec, url)
            return

        probe = None
        async with self._sem:
            probe = await http_get(url, pool=self._pool, headers={"Range": "bytes=0-0"},
                                   timeout=self.timeout)
            total = spec.size or 0
            ranged = False
            if probe.status == 206:
                info = probe.content_range
                if info:
                    total = total or info[2]
                ranged = True
                # 探测只读了 1 字节，读掉它连接才能安全复用
                await probe.drain()
                probe.close()
            elif probe.status == 200:
                # 服务端忽略 Range：这次响应体就是整个文件，直接用它下载，避免下两遍
                total = total or (probe.content_length or 0)
                ranged = False
            else:
                await probe.drain()
                probe.close()
                raise HttpError(probe.status, probe.reason, url)

        if total and total != spec.size:
            delta = total - (spec.size or 0)
            if delta > 0:
                self._progress.bytes_total += delta
            spec.size = total

        if not ranged and probe is not None:
            await self._consume_response(spec, part, probe, total)
            return

        # 分片：仅当服务器支持 Range 且文件够大
        can_split = ranged and total >= MIN_CHUNK
        if can_split:
            parts = min(self.max_parts_per_file, max(1, (total + self.chunk_size - 1) // self.chunk_size))
            if parts > 1:
                await self._fetch_parts(spec, url, part, total, parts)
                return
        await self._fetch_stream(spec, url, part, total, ranged)

    async def _consume_response(self, spec: FileSpec, part: Path, resp, total: int) -> None:
        """把已经打开的响应体写成文件（用于"服务端不支持 Range"的情况）。"""
        async with self._sem:
            block = self._read_block()
            written = 0
            started_at = time.monotonic()
            try:
                with open(part, "wb") as fh:
                    async for data in resp.iter_body(block):
                        self._check_cancel()
                        fh.write(data)
                        written += len(data)
                        spec.bytes_done += len(data)
                        self._add_bytes(len(data))
                        await self.limiter.consume(len(data))
                        self._check_source_speed(started_at, written, spec)
            finally:
                resp.close()
        if total and written != total:
            raise asyncio.IncompleteReadError(partial=b"", expected=total)
        part.replace(spec.dest)

    async def _fetch_small(self, spec: FileSpec, url: str) -> None:
        """小文件直连下载：跳过 Range 探测，省掉每个文件一次往返。

        assets 动辄几千个文件，探测请求的开销比下载本身还大，所以
        已知大小且小于 MIN_CHUNK 的文件一律走这里（不做续传，重下成本极低）。
        """
        part = self._part_path(spec)
        part.parent.mkdir(parents=True, exist_ok=True)
        async with self._sem:
            resp = await http_get(url, pool=self._pool, timeout=self.timeout)
            if resp.status != 200:
                resp.keep_alive = False
                resp.close()
                raise HttpError(resp.status, resp.reason, url)
            block = self._read_block()
            written = 0
            try:
                with open(part, "wb") as fh:
                    async for data in resp.iter_body(block):
                        self._check_cancel()
                        fh.write(data)
                        written += len(data)
                        spec.bytes_done += len(data)
                        self._add_bytes(len(data))
                        await self.limiter.consume(len(data))
            finally:
                resp.close()
        if spec.size and written != spec.size:
            raise asyncio.IncompleteReadError(partial=b"", expected=spec.size)
        part.replace(spec.dest)

    # ── 单流下载 ─────────────────────────────────────────────────

    async def _fetch_stream(self, spec: FileSpec, url: str, part: Path, total: int,
                            range_supported: bool) -> None:
        start = 0
        if self.resume and part.is_file() and range_supported:
            start = part.stat().st_size
            if total and start > total:
                start = 0
            if start and total and start == total:
                start = 0  # 大小已经对了但校验没过，重来
        async with self._sem:
            headers = {"Range": f"bytes={start}-"} if start else None
            resp = await http_get(url, pool=self._pool, headers=headers, timeout=self.timeout)
            if start and resp.status != 206:
                start = 0  # 服务器不支持续传，从头下
                resp.keep_alive = False
                resp.close()
                resp = await http_get(url, pool=self._pool, timeout=self.timeout)
            if resp.status not in (200, 206):
                resp.keep_alive = False
                resp.close()
                raise HttpError(resp.status, resp.reason, url)

            mode = "ab" if start else "wb"
            block = self._read_block()
            written = 0
            started_at = time.monotonic()
            try:
                with open(part, mode) as fh:
                    async for data in resp.iter_body(block):
                        self._check_cancel()
                        fh.write(data)
                        written += len(data)
                        spec.bytes_done += len(data)
                        self._add_bytes(len(data))
                        await self.limiter.consume(len(data))
                        self._check_source_speed(started_at, written, spec)
            finally:
                resp.close()
        if total and written + start < total:
            raise asyncio.IncompleteReadError(partial=b"", expected=total)
        part.replace(spec.dest)

    # ── 多连接分片下载 ───────────────────────────────────────────

    def _part_path(self, spec: FileSpec) -> Path:
        return spec.dest.with_name(spec.dest.name + ".part")

    def _sidecar_path(self, spec: FileSpec) -> Path:
        return spec.dest.with_name(spec.dest.name + ".part.json")

    def _load_ranges(self, spec: FileSpec, url: str, total: int) -> set:
        side = self._sidecar_path(spec)
        part = self._part_path(spec)
        if not (self.resume and side.is_file() and part.is_file()):
            return set()
        try:
            data = json.loads(side.read_text(encoding="utf-8"))
        except Exception:
            return set()
        # 同 sha1 的文件换源也能续传：只在没有 sha1 时才要求 URL 一致
        if int(data.get("total", -1)) != total:
            return set()
        if not spec.sha1 and data.get("url") != url:
            return set()
        if part.stat().st_size != total:
            return set()
        return {tuple(r) for r in data.get("ranges", [])}

    def _save_ranges(self, spec: FileSpec, url: str, total: int, done: set) -> None:
        try:
            self._sidecar_path(spec).write_text(json.dumps({
                "url": url, "total": total, "sha1": spec.sha1 or "",
                "ranges": sorted(list(done)),
            }), encoding="utf-8")
        except OSError:
            pass

    async def _fetch_parts(self, spec: FileSpec, url: str, part: Path, total: int,
                           parts: int) -> None:
        size_of_part = (total + parts - 1) // parts
        ranges = [(i * size_of_part, min((i + 1) * size_of_part - 1, total - 1)) for i in range(parts)]
        done = self._load_ranges(spec, url, total)
        if not part.is_file() or part.stat().st_size != total:
            with open(part, "wb") as fh:
                fh.truncate(total)
            done = set()

        pending = [r for r in ranges if r not in done]
        if not pending:
            part.replace(spec.dest)
            return

        progress_lock = asyncio.Lock()
        failed: list[str] = []

        async def one(rng):
            start, end = rng
            self._check_cancel()
            async with self._sem:
                headers = {"Range": f"bytes={start}-{end}"}
                resp = await http_get(url, pool=self._pool, headers=headers, timeout=self.timeout)
                if resp.status != 206:
                    resp.keep_alive = False
                    resp.close()
                    raise HttpError(resp.status, resp.reason, url)
                block = self._read_block()
                received = 0
                started_at = time.monotonic()
                try:
                    with open(part, "r+b") as fh:
                        fh.seek(start)
                        async for data in resp.iter_body(block):
                            self._check_cancel()
                            fh.write(data)
                            received += len(data)
                            spec.bytes_done += len(data)
                            self._add_bytes(len(data))
                            await self.limiter.consume(len(data))
                            self._check_source_speed(started_at, received, spec)
                    if received != end - start + 1:
                        raise asyncio.IncompleteReadError(partial=b"", expected=end - start + 1)
                finally:
                    resp.close()
            async with progress_lock:
                done.add(rng)
                self._save_ranges(spec, url, total, done)

        await asyncio.gather(*(one(r) for r in pending), return_exceptions=True)
        missing = [r for r in ranges if r not in done]
        if missing:
            # 只补缺失的分片（镜像节点偶发掐连接时，不必整个文件重下）
            log.warning("分片补齐: %d/%d 片重试 | %s", len(missing), len(ranges), spec.name)
            await asyncio.sleep(0.5)
            await asyncio.gather(*(one(r) for r in missing), return_exceptions=True)
            missing = [r for r in ranges if r not in done]
        if missing:
            raise HttpError(0, f"{len(missing)}/{len(ranges)} 个分片未完成", url)
        spec.dest.parent.mkdir(parents=True, exist_ok=True)
        part.replace(spec.dest)
        try:
            self._sidecar_path(spec).unlink()
        except OSError:
            pass

    def _check_source_speed(self, started: float, bytes_done: int, spec: FileSpec) -> None:
        """某条路太慢就主动放弃，交给 _download_spec 换下一条候选路。

        注意：用户自己设了限速时不判定（否则限速会被误判成"源太慢"）。
        """
        if self.limiter.rate > 0 or len(spec.candidates) < 2:
            return
        elapsed = time.monotonic() - started
        if elapsed < SLOW_SOURCE_GRACE:
            return
        speed = bytes_done / elapsed
        if speed < MIN_SOURCE_SPEED:
            raise HttpError(0, "源速度过慢 (%.0fKB/s)，切换下一个源" % (speed / 1024), spec.url)

    def _cleanup_partial(self, spec: FileSpec) -> None:
        for path in (self._part_path(spec), self._sidecar_path(spec)):
            try:
                if path.is_file():
                    path.unlink()
            except OSError:
                pass

    # ── 读取块 / 进度 ────────────────────────────────────────────

    def _read_block(self) -> int:
        rate = self.limiter.rate
        if rate <= 0:
            return READ_BLOCK
        return int(max(MIN_READ_BLOCK, min(READ_BLOCK, rate / 8)))

    def _add_bytes(self, n: int) -> None:
        self._progress.bytes_done += n
        self._speed_samples.append((time.monotonic(), self._progress.bytes_done))
        if len(self._speed_samples) > 64:
            del self._speed_samples[:32]

    def _emit_progress(self, force: bool = False) -> None:
        now = time.monotonic()
        if not force and now - self._last_emit < 0.1:
            return
        self._last_emit = now
        window = [s for s in self._speed_samples if now - s[0] <= 2.0]
        if len(window) >= 2:
            dt = window[-1][0] - window[0][0]
            db = window[-1][1] - window[0][1]
            speed = db / dt if dt > 0 else 0.0
        else:
            speed = 0.0
        self._progress.speed_bps = speed
        remaining = max(0, self._progress.bytes_total - self._progress.bytes_done)
        self._progress.eta_seconds = remaining / speed if speed > 1 else 0.0
        if self._on_progress:
            try:
                self._on_progress(self._progress)
            except Exception as exc:  # UI 回调炸了不能拖垮下载
                log.debug("进度回调异常(忽略): %s", exc)

    async def _progress_loop(self, interval: float) -> None:
        try:
            while True:
                await asyncio.sleep(max(0.05, interval))
                self._emit_progress()
        except asyncio.CancelledError:
            return

    # ── 统计 ─────────────────────────────────────────────────────

    def stats(self) -> dict:
        created, reused = self._pool.stats if self._pool else (0, 0)
        return {
            "connections_created": created,
            "connections_reused": reused,
            "max_connections": self.max_connections,
            "rate_limit": self.limiter.rate,
        }


def _quiet_exception_handler(loop, context) -> None:
    """忽略连接断开类噪音（Windows proactor 关闭时会抛 ConnectionResetError）。"""
    exc = context.get("exception")
    if isinstance(exc, (ConnectionResetError, ConnectionAbortedError, BrokenPipeError, OSError)):
        log.debug("asyncio 连接噪音: %s", exc)
        return
    log.debug("asyncio 异常: %s", context.get("message"))


# ── 便捷入口 ─────────────────────────────────────────────────────

_DEFAULT: Optional[AsyncDownloadEngine] = None


_SHARED = RateLimiter(0)


def limiter() -> RateLimiter:
    """全局限速器（设置页改速时直接 limiter().set_rate(bps) 即时生效）。"""
    return _SHARED


def _config_values() -> dict:
    """读取设置页当前值（并发 / 限速 / 是否校验）—— 经 core.settings 门面。"""
    from src.core.settings import settings

    max_conn = settings.max_connections()
    verify = settings.verify_sha1()
    limit_bps = settings.speed_limit_bps()
    _SHARED.set_rate(limit_bps)
    return {
        "max_connections": max(1, min(128, max_conn)),
        "verify": verify,
        "limiter": _SHARED,
    }


def default_engine() -> AsyncDownloadEngine:
    """按当前设置构造/复用默认引擎（并发数、限速、校验开关跟随设置页）。"""
    global _DEFAULT
    values = _config_values()
    if _DEFAULT is None:
        _DEFAULT = AsyncDownloadEngine(**values)
    else:
        _DEFAULT.max_connections = values["max_connections"]
        _DEFAULT.verify = values["verify"]
        _DEFAULT.limiter = values["limiter"]
    return _DEFAULT


def download_specs(
    specs: Sequence[FileSpec],
    *,
    on_progress: Optional[Callable[[ProgressInfo], None]] = None,
    cancel: Optional[threading.Event] = None,
) -> list[DownloadResult]:
    """用默认引擎下载一批文件（同步）。"""
    engine = default_engine()
    engine.limiter = limiter()
    return engine.download(specs, on_progress=on_progress, cancel=cancel)
