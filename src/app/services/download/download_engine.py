# Silent X Craft Launcher (SXCL)
# Copyright (C) SilentStudio / SilentCodeTeams / Silent X Craft Launcher
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
"""下载引擎 - 多线程分片下载"""

import os
import threading
import queue
import time
import shutil
import hashlib
from pathlib import Path
from typing import Optional, Callable, List

import requests

from src.app.common.logger import log


class TaskStatus:
    """任务状态"""
    PENDING = "pending"
    RUNNING = "running"
    COMPLETED = "completed"
    FAILED = "failed"
    CANCELLED = "cancelled"


class DownloadChunk:
    """下载分片"""
    def __init__(self, chunk_id: int, start_byte: int, end_byte: int, temp_path: Path):
        self.chunk_id = chunk_id
        self.start_byte = start_byte
        self.end_byte = end_byte
        self.temp_path = temp_path
        self.downloaded = 0
        self.status = TaskStatus.PENDING
        self.retry_count = 0
        self.task = None  # 反向引用


class DownloadTask:
    """下载任务"""
    def __init__(self, url: str, path: Path, expected_size: int = 0, sha1: str = None):
        self.url = url
        self.path = path
        self.expected_size = expected_size
        self.sha1 = sha1
        self.chunks: List[DownloadChunk] = []
        self.downloaded_bytes = 0
        self.status = TaskStatus.PENDING
        self.progress_callback: Optional[Callable] = None


class DownloadWorker(threading.Thread):
    """下载工作线程 - 处理单个分片"""
    
    def __init__(
        self,
        worker_id: int,
        task_queue: queue.Queue,
        result_queue: queue.Queue,
        max_retries: int = 3,
        timeout: int = 30
    ):
        super().__init__(daemon=True)
        self.worker_id = worker_id
        self.task_queue = task_queue
        self.result_queue = result_queue
        self.max_retries = max_retries
        self.timeout = timeout
        self._running = True
        
    def run(self):
        while self._running:
            try:
                chunk = self.task_queue.get(timeout=1)
                if chunk is None:
                    continue
                
                self._download_chunk(chunk)
                self.result_queue.put(chunk)
                
            except queue.Empty:
                continue
            except Exception as e:
                log.error(f"Worker {self.worker_id} 异常: {e}")
                time.sleep(0.1)
    
    def _download_chunk(self, chunk: DownloadChunk):
        """下载单个分片"""
        task = chunk.task
        url = task.url
        start = chunk.start_byte
        end = chunk.end_byte
        temp_path = chunk.temp_path
        
        headers = {
            'Range': f'bytes={start}-{end}',
            'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36'
        }
        
        for attempt in range(self.max_retries):
            try:
                chunk.status = TaskStatus.RUNNING
                
                response = requests.get(
                    url,
                    headers=headers,
                    stream=True,
                    timeout=self.timeout
                )
                response.raise_for_status()
                
                # 写入临时文件
                temp_path.parent.mkdir(parents=True, exist_ok=True)
                with open(temp_path, 'wb') as f:
                    for data in response.iter_content(chunk_size=8192):
                        if not self._running:
                            chunk.status = TaskStatus.CANCELLED
                            return
                        f.write(data)
                        chunk.downloaded += len(data)
                        
                        # 更新任务进度
                        task.downloaded_bytes += len(data)
                        if task.progress_callback:
                            task.progress_callback(
                                task.downloaded_bytes,
                                task.expected_size,
                                f"下载分片 {chunk.chunk_id + 1}"
                            )
                
                chunk.status = TaskStatus.COMPLETED
                log.debug(f"分片 {chunk.chunk_id} 下载完成")
                return
                
            except Exception as e:
                log.warning(f"分片 {chunk.chunk_id} 下载失败 (尝试 {attempt + 1}/{self.max_retries}): {e}")
                chunk.retry_count += 1
                time.sleep(1)
        
        chunk.status = TaskStatus.FAILED
        log.error(f"分片 {chunk.chunk_id} 最终失败")
    
    def stop(self):
        self._running = False


class DownloadEngine:
    """多线程分片下载引擎"""
    
    # 默认配置
    DEFAULT_WORKERS = 8
    DEFAULT_CHUNK_SIZE = 1024 * 1024  # 1MB
    MIN_FILE_SIZE_FOR_CHUNK = 5 * 1024 * 1024  # 5MB 以上才分片
    
    def __init__(
        self,
        max_workers: int = None,
        chunk_size: int = None,
        timeout: int = 30,
        max_retries: int = 3
    ):
        self.max_workers = max_workers or self.DEFAULT_WORKERS
        self.chunk_size = chunk_size or self.DEFAULT_CHUNK_SIZE
        self.timeout = timeout
        self.max_retries = max_retries
        
        self._task_queue = queue.Queue()
        self._result_queue = queue.Queue()
        self._workers: List[DownloadWorker] = []
        self._running = False
        self._current_task: Optional[DownloadTask] = None
        
        # 进度回调
        self._progress_callback: Optional[Callable] = None
        self._total_tasks = 0
        self._completed_tasks = 0
        
    def set_progress_callback(self, callback: Callable[[int, int, str], None]):
        """设置总体进度回调"""
        self._progress_callback = callback

    def _notify_progress(self, current: int, total: int, status: str, *, is_bytes: bool = True):
        """通知进度（含网速和 ETA）。

        Args:
            is_bytes: ``True`` 表示 current/total 是字节数, ``False`` 表示是文件数。
        """
        if total <= 0 or current <= 0:
            if self._progress_callback:
                self._progress_callback(current, total, status)
            return

        now = time.time()
        if not hasattr(self, "_speed_start"):
            self._speed_start = now
            self._speed_bytes = 0

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
        if bps >= 10 * 1024 * 1024:
            return f"{bps / 1024 / 1024:.1f}MB"
        if bps >= 1024 * 1024:
            return f"{bps / 1024 / 1024:.1f}MB"
        if bps >= 1024:
            return f"{bps / 1024:.0f}KB"
        return f"{bps:.0f}B"
    
    def download(self, url: str, path: Path, expected_size: int = 0, sha1: str = None) -> bool:
        """
        下载单个文件 - 自动判断是否分片
        """
        try:
            # 检查文件是否已存在
            if path.exists():
                if expected_size > 0 and path.stat().st_size == expected_size:
                    if sha1 and self._verify_sha1(path, sha1):
                        log.info(f"文件已存在且校验通过: {path.name}")
                        return True
                    elif not sha1:
                        log.info(f"文件已存在，跳过: {path.name}")
                        return True
            
            # 判断是否分片
            if expected_size >= self.MIN_FILE_SIZE_FOR_CHUNK:
                log.info(f"大文件 ({expected_size / 1024 / 1024:.1f}MB) 使用分片下载: {path.name}")
                return self._download_with_chunks(url, path, expected_size, sha1)
            else:
                return self._download_single(url, path, expected_size, sha1)
                
        except Exception as e:
            log.error(f"下载失败 {path.name}: {e} | {url[:120]}")
            return False
    
    def _download_single(self, url: str, path: Path, expected_size: int = 0, sha1: str = None) -> bool:
        """单线程下载（小文件）"""
        try:
            log.debug(f"单线程下载: {path.name}")
            path.parent.mkdir(parents=True, exist_ok=True)
            
            response = requests.get(url, stream=True, timeout=self.timeout)
            response.raise_for_status()
            
            # 获取实际文件大小（如果未提供）
            content_length = response.headers.get('content-length')
            if expected_size == 0 and content_length:
                expected_size = int(content_length)
            
            downloaded = 0
            with open(path, 'wb') as f:
                for chunk in response.iter_content(chunk_size=8192):
                    if not self._running:
                        return False
                    if chunk:
                        f.write(chunk)
                        downloaded += len(chunk)
                        # 只有 expected_size > 0 时才通知进度
                        if self._progress_callback and expected_size > 0:
                            self._progress_callback(
                                downloaded, expected_size,
                                f"下载 {path.name}"
                            )
            
            # 校验
            if sha1 and not self._verify_sha1(path, sha1):
                log.error(f"SHA1 校验失败: {path.name} | {url[:80]}")
                path.unlink()
                return False
            
            return True
            
        except Exception as e:
            log.error(f"单线程下载失败: {path.name} - {e} | {url[:80]}")
            return False
    
    def _download_with_chunks(self, url: str, path: Path, expected_size: int = 0, sha1: str = None) -> bool:
        """多线程分片下载（大文件）"""
        temp_dir = None
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            temp_dir = path.parent / f".{path.name}.tmp"
            temp_dir.mkdir(parents=True, exist_ok=True)
            
            # 创建任务
            task = DownloadTask(
                url=url,
                path=path,
                expected_size=expected_size,
                sha1=sha1
            )
            task.progress_callback = self._progress_callback
            self._current_task = task
            
            # 计算分片 - 根据文件大小和 worker 数量动态调整
            num_chunks = min(
                self.max_workers * 2,
                max(1, (expected_size + self.chunk_size - 1) // self.chunk_size)
            )
            chunk_size = (expected_size + num_chunks - 1) // num_chunks
            
            log.info(f"分片下载: {path.name} -> {num_chunks} 片, 每片 ~{chunk_size // 1024}KB")
            
            # 创建分片
            chunks = []
            for i in range(num_chunks):
                start = i * chunk_size
                end = min(start + chunk_size - 1, expected_size - 1)
                chunk = DownloadChunk(
                    chunk_id=i,
                    start_byte=start,
                    end_byte=end,
                    temp_path=temp_dir / f"chunk_{i:04d}.tmp"
                )
                chunk.task = task
                chunks.append(chunk)
                self._task_queue.put(chunk)
            
            # 启动工作线程
            self._running = True
            self._total_tasks = num_chunks
            self._completed_tasks = 0
            
            actual_workers = min(self.max_workers, num_chunks)
            for i in range(actual_workers):
                worker = DownloadWorker(
                    i, self._task_queue, self._result_queue,
                    self.max_retries, self.timeout
                )
                worker.start()
                self._workers.append(worker)
            
            # 等待完成
            completed = 0
            failed = 0
            
            while completed + failed < num_chunks:
                try:
                    chunk = self._result_queue.get(timeout=1)
                    if chunk.status == TaskStatus.COMPLETED:
                        completed += 1
                        self._completed_tasks = completed
                        self._notify_progress(completed, num_chunks, f"分片 {completed}/{num_chunks}")
                    elif chunk.status == TaskStatus.FAILED:
                        failed += 1
                    elif chunk.status == TaskStatus.CANCELLED:
                        self._stop_workers()
                        return False
                except queue.Empty:
                    continue
            
            # 停止 workers
            self._stop_workers()
            
            if failed > 0:
                log.error(f"分片下载失败: {failed}/{num_chunks} 片失败")
                if temp_dir and temp_dir.exists():
                    shutil.rmtree(temp_dir, ignore_errors=True)
                return False
            
            # 合并分片
            log.info(f"合并分片: {path.name}")
            with open(path, 'wb') as out_f:
                for chunk in chunks:
                    chunk_path = chunk.temp_path
                    if chunk_path.exists():
                        with open(chunk_path, 'rb') as in_f:
                            shutil.copyfileobj(in_f, out_f)
                        chunk_path.unlink()
            
            # 删除临时目录
            if temp_dir and temp_dir.exists():
                shutil.rmtree(temp_dir, ignore_errors=True)
            
            # 校验
            if sha1 and not self._verify_sha1(path, sha1):
                log.error(f"SHA1 校验失败: {path.name}")
                path.unlink()
                return False
            
            log.info(f"分片下载完成: {path.name}")
            return True
            
        except Exception as e:
            log.error(f"分片下载失败: {path.name} - {e}")
            if temp_dir and temp_dir.exists():
                shutil.rmtree(temp_dir, ignore_errors=True)
            return False
    
    def _stop_workers(self):
        """停止所有工作线程"""
        self._running = False
        for worker in self._workers:
            worker.stop()
        self._workers.clear()
    
    def _verify_sha1(self, path: Path, expected_sha1: str) -> bool:
        """验证 SHA1"""
        try:
            sha1_hash = hashlib.sha1()
            with open(path, 'rb') as f:
                for chunk in iter(lambda: f.read(8192), b''):
                    sha1_hash.update(chunk)
            return sha1_hash.hexdigest().lower() == expected_sha1.lower()
        except Exception as e:
            log.error(f"SHA1 校验异常: {e}")
            return False
    
    def download_batch(
        self,
        items: list[tuple[str, Path, int, str | None]],
        max_concurrent: int = 8,
    ) -> list[bool]:
        """批量下载多个文件，使用共享线程池并行下载。

        Args:
            items: 列表每个元素为 ``(url, path, expected_size, sha1)``
            max_concurrent: 最大并发数

        Returns:
            每个文件对应的成功/失败状态列表。
        """
        results: list[bool] = [False] * len(items)
        lock = threading.Lock()
        completed = [0]
        failed_urls: list[str] = []

        def _download_one(index: int, url: str, path: Path, size: int, sha1: str | None):
            ok = self.download(url, path, size, sha1)
            with lock:
                results[index] = ok
                completed[0] += 1
                if not ok:
                    failed_urls.append(url)
                self._notify_progress(
                    completed[0], len(items),
                    f"批量下载 ({completed[0]}/{len(items)})",
                    is_bytes=False,
                )

        threads: list[threading.Thread] = []
        sem = threading.Semaphore(max_concurrent)

        def _worker(index: int, url: str, path: Path, size: int, sha1: str | None):
            try:
                _download_one(index, url, path, size, sha1)
            finally:
                sem.release()

        for i, (url, path, size, sha1) in enumerate(items):
            sem.acquire()
            t = threading.Thread(
                target=_worker,
                args=(i, url, path, size, sha1),
                daemon=True,
            )
            t.start()
            threads.append(t)

        for t in threads:
            t.join()

        if failed_urls:
            log.warning("批量下载 | %d/%d 个文件失败 (前 5 个): %s",
                        len(failed_urls), len(items),
                        failed_urls[:5])

        self._notify_progress(
            len(items), len(items),
            "批量下载完成",
            is_bytes=False,
        )
        return results

    def cancel(self):
        """取消所有下载"""
        self._running = False
        while not self._task_queue.empty():
            try:
                self._task_queue.get_nowait()
            except:
                break
        self._stop_workers()
        log.info("下载已取消")