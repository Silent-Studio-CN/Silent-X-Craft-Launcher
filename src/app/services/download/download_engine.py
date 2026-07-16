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
"""Download engine — multi-threaded chunk download with cross-platform safety.

Architecture
────────────
- Single shared ``requests.Session`` with connection pooling for all chunks.
- ``ThreadPoolExecutor`` for worker management (no raw thread/queue).
- Platform-aware concurrency: macOS ARM64 = 3, others = 8.
- Retry adapter with exponential backoff.
"""

from __future__ import annotations

import hashlib
import platform
import shutil
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from threading import Lock
from typing import Callable, Optional

import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

from src.app.common.logger import log

# ── Platform-aware concurrency ───────────────────────────────────
# macOS ARM64 Python 3.14 has threading + SSL issues with too many
# concurrent connections.  Reduce worker count but KEEP chunking.
_PLATFORM = platform.system()
_ARCH = platform.machine()
_IS_MACOS_ARM64 = _PLATFORM == "Darwin" and _ARCH == "arm64"

DEFAULT_MAX_WORKERS = 3 if _IS_MACOS_ARM64 else 8
CHUNK_SIZE = 512 * 1024       # 512 KB per chunk
MIN_FILE_SIZE_FOR_CHUNK = 5 * 1024 * 1024  # 5 MB


# ── Shared session factory ───────────────────────────────────────

def _make_session() -> requests.Session:
    """Create a session with connection pooling and retry support."""
    retry = Retry(
        total=2,
        backoff_factor=0.5,
        status_forcelist={500, 502, 503, 504},
        allowed_methods={"GET"},
    )
    adapter = HTTPAdapter(
        pool_connections=8,
        pool_maxsize=16,
        max_retries=retry,
    )
    session = requests.Session()
    session.mount("https://", adapter)
    session.mount("http://", adapter)
    session.headers.update({
        "User-Agent": "SilentXCraftLauncher/1.0",
    })
    return session


# ── Download Engine ──────────────────────────────────────────────


class DownloadEngine:
    """Multi-threaded chunk download engine.

    Uses a single shared ``requests.Session`` so SSL connections are
    pooled across all chunk threads — this avoids the macOS ARM64
    threading + SSL segfault without disabling chunking.
    """

    def __init__(
        self,
        max_workers: int | None = None,
        timeout: int = 30,
    ):
        self.max_workers = max_workers or DEFAULT_MAX_WORKERS
        self.timeout = timeout
        self._session = _make_session()
        self._progress_callback: Callable[[int, int, str], None] | None = None
        self._running = True

        # Speed tracking
        self._speed_lock = Lock()
        self._speed_start = 0.0
        self._speed_bytes = 0

    def set_progress_callback(self, callback: Callable[[int, int, str], None]):
        self._progress_callback = callback

    def cancel(self):
        self._running = False

    # ── Progress helpers ────────────────────────────────────────

    def _notify_progress(self, current: int, total: int, status: str, *, is_bytes: bool = True):
        if total <= 0 or current <= 0:
            if self._progress_callback:
                self._progress_callback(current, total, status)
            return

        now = time.time()
        with self._speed_lock:
            if not self._speed_start:
                self._speed_start = now
            elapsed = now - self._speed_start
            self._speed_bytes = current

        if elapsed >= 1.0:
            rate = current / elapsed if elapsed > 0 else 0
            remaining = total - current
            eta_sec = remaining / rate if rate > 0 else 0
            pct = current * 100 // total

            if is_bytes:
                unit = self._format_speed(rate)
                eta_str = f"{eta_sec / 60:.1f}分钟" if eta_sec >= 60 else f"{eta_sec:.0f}秒"
                log.info("下载 | %s | %d%% | %s/s | 剩余 %s", status, pct, unit, eta_str)
            else:
                eta_str = f"{eta_sec / 60:.1f}分钟" if eta_sec >= 60 else f"{eta_sec:.0f}秒"
                log.info("下载 | %s | %d%% (%d/%d) | %d 文件/秒 | 剩余 %s",
                         status, pct, current, total, int(rate), eta_str)

        if self._progress_callback:
            self._progress_callback(current, total, status)

    @staticmethod
    def _format_speed(bps: float) -> str:
        if bps >= 1024 * 1024:
            return f"{bps / 1024 / 1024:.1f}MB"
        if bps >= 1024:
            return f"{bps / 1024:.0f}KB"
        return f"{bps:.0f}B"

    # ── SHA1 verification ───────────────────────────────────────

    @staticmethod
    def _verify_sha1(path: Path, expected: str) -> bool:
        try:
            h = hashlib.sha1()
            with open(path, "rb") as f:
                for chunk in iter(lambda: f.read(8192), b""):
                    h.update(chunk)
            return h.hexdigest().lower() == expected.lower()
        except Exception:
            return False

    # ── Single-threaded download (small files) ──────────────────

    def _download_single(self, url: str, path: Path, expected_size: int = 0, sha1: str | None = None) -> bool:
        # Try up to 2 times (CDN 5xx → fallback to original)
        for attempt in range(2):
            try:
                path.parent.mkdir(parents=True, exist_ok=True)
                resp = self._session.get(url, stream=True, timeout=self.timeout)
                resp.raise_for_status()

                # Log CDN redirect info (helps debug 525 errors)
                if "bmclapi" in url.lower() and resp.history:
                    final_url = str(resp.url)[:100]
                    log.info("CDN 重定向: %s → %s", url[:60], final_url)

                cl = resp.headers.get("content-length")
                total = int(cl) if cl else expected_size

                downloaded = 0
                with open(path, "wb") as f:
                    for chunk in resp.iter_content(chunk_size=8192):
                        if not self._running:
                            return False
                        if chunk:
                            f.write(chunk)
                            downloaded += len(chunk)
                            if total > 0:
                                self._notify_progress(downloaded, total, f"下载 {path.name}")

                if sha1 and not self._verify_sha1(path, sha1):
                    log.error("SHA1 校验失败: %s | %s", path.name, url[:80])
                    path.unlink()
                    return False
                return True

            except requests.exceptions.HTTPError as e:
                status = e.response.status_code if e.response is not None else 0
                if attempt == 0 and status in (500, 502, 503, 504, 525, 526):
                    log.warning("下载失败 (HTTP %d), 重试一次: %s | %s", status, path.name, url[:80])
                    continue
                log.error("下载失败: %s - HTTP %d | %s", path.name, status, url[:80])
                return False
            except Exception as e:
                log.error("下载失败: %s - %s | %s", path.name, e, url[:80])
                return False
        return False

    # ── Chunked download (large files, multi-threaded) ──────────

    def _download_chunk(self, session: requests.Session, url: str, start: int, end: int,
                        temp_path: Path) -> bool:
        """Download a single byte-range chunk; used by _download_with_chunks."""
        try:
            headers = {"Range": f"bytes={start}-{end}"}
            resp = session.get(url, headers=headers, stream=True, timeout=self.timeout)
            resp.raise_for_status()
            temp_path.parent.mkdir(parents=True, exist_ok=True)
            with open(temp_path, "wb") as f:
                for chunk in resp.iter_content(chunk_size=8192):
                    if not self._running:
                        return False
                    if chunk:
                        f.write(chunk)
            return True
        except Exception as e:
            log.warning("分片下载失败 %s-%s: %s", start, end, e)
            return False

    def _download_with_chunks(self, url: str, path: Path, expected_size: int = 0, sha1: str | None = None) -> bool:
        temp_dir = path.parent / f".{path.name}.tmp"
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            temp_dir.mkdir(parents=True, exist_ok=True)

            # Determine file size
            file_size = expected_size
            if file_size <= 0:
                head = self._session.head(url, timeout=self.timeout)
                head.raise_for_status()
                cl = head.headers.get("content-length")
                file_size = int(cl) if cl else 0
            if file_size <= 0:
                return self._download_single(url, path, 0, sha1)

            # Calculate chunks
            num_chunks = min(self.max_workers * 2, max(1, file_size // CHUNK_SIZE + 1))
            actual_chunk = (file_size + num_chunks - 1) // num_chunks
            log.info("分片下载: %s -> %d 片, 每片 ~%dKB", path.name, num_chunks, actual_chunk // 1024)

            chunks: list[tuple[int, int, Path]] = []
            for i in range(num_chunks):
                start = i * actual_chunk
                end = min(start + actual_chunk - 1, file_size - 1)
                t = temp_dir / f"chunk_{i:04d}.tmp"
                chunks.append((start, end, t))

            # Download chunks via thread pool (shared session)
            with ThreadPoolExecutor(max_workers=self.max_workers) as pool:
                futures = {}
                for i, (start, end, t) in enumerate(chunks):
                    fut = pool.submit(self._download_chunk, self._session, url, start, end, t)
                    futures[fut] = i

                completed = 0
                failed = 0
                for fut in as_completed(futures):
                    if fut.result():
                        completed += 1
                    else:
                        failed += 1
                    self._notify_progress(completed, num_chunks, f"分片 {completed}/{num_chunks}",
                                          is_bytes=False)
                    if not self._running:
                        pool.shutdown(wait=False, cancel_futures=True)
                        return False

            if failed > 0:
                log.error("分片下载失败: %d/%d 片失败 | %s", failed, num_chunks, path.name)
                shutil.rmtree(temp_dir, ignore_errors=True)
                return False

            # Merge chunks
            log.info("合并分片: %s", path.name)
            with open(path, "wb") as out:
                for _, _, t in chunks:
                    if t.exists():
                        with open(t, "rb") as f:
                            shutil.copyfileobj(f, out)
                        t.unlink()
            shutil.rmtree(temp_dir, ignore_errors=True)

            if sha1 and not self._verify_sha1(path, sha1):
                log.error("SHA1 校验失败: %s | %s", path.name, url[:80])
                path.unlink()
                return False

            log.info("分片下载完成: %s", path.name)
            return True

        except Exception as e:
            log.error("分片下载失败: %s - %s", path.name, e)
            if temp_dir.exists():
                shutil.rmtree(temp_dir, ignore_errors=True)
            return False

    # ── Public API ──────────────────────────────────────────────

    def download(self, url: str, path: Path, expected_size: int = 0, sha1: str | None = None) -> bool:
        """Download a single file — auto-selects chunked or single-thread."""
        try:
            if path.exists():
                if expected_size > 0 and path.stat().st_size == expected_size:
                    if sha1 and self._verify_sha1(path, sha1):
                        log.info("文件已存在且校验通过: %s", path.name)
                        return True
                    if not sha1:
                        log.info("文件已存在，跳过: %s", path.name)
                        return True

            if expected_size >= MIN_FILE_SIZE_FOR_CHUNK:
                log.info("大文件 (%dMB) 使用分片下载: %s", expected_size // 1024 // 1024, path.name)
                return self._download_with_chunks(url, path, expected_size, sha1)
            else:
                return self._download_single(url, path, expected_size, sha1)

        except Exception as e:
            log.error("下载失败 %s: %s | %s", path.name, e, url[:120])
            return False

    # ── Batch downloads ─────────────────────────────────────────

    def download_batch(
        self,
        items: list[tuple[str, Path, int, str | None]],
        max_concurrent: int | None = None,
    ) -> list[bool]:
        """Download multiple files concurrently via thread pool."""
        max_workers = max_concurrent or DEFAULT_MAX_WORKERS
        results: list[bool] = [False] * len(items)
        failed_urls: list[str] = []
        lock = Lock()
        done = [0]

        def _work(index: int, url: str, path: Path, size: int, sha1: str | None):
            ok = self.download(url, path, size, sha1)
            with lock:
                results[index] = ok
                done[0] += 1
                if not ok:
                    failed_urls.append(url)
                self._notify_progress(done[0], len(items), f"批量下载 ({done[0]}/{len(items)})",
                                      is_bytes=False)
            return ok

        with ThreadPoolExecutor(max_workers=max_workers) as pool:
            futs = []
            for i, (url, path, size, sha1) in enumerate(items):
                futs.append(pool.submit(_work, i, url, path, size, sha1))
            for _ in as_completed(futs):
                pass  # handled inside _work via lock

        if failed_urls:
            log.warning("批量下载 | %d/%d 个文件失败 (前 5 个): %s",
                        len(failed_urls), len(items), failed_urls[:5])

        self._notify_progress(len(items), len(items), "批量下载完成", is_bytes=False)
        return results

    def __del__(self):
        try:
            self._session.close()
        except Exception:
            pass
