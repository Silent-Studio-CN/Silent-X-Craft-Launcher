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
"""元数据（小 JSON / 文本）下载的统一入口。

为什么要单独包一层：

1. **硬性截止时间**。requests 的 timeout 是"每个 socket 操作的超时"，
   当某个域名对多个 IP 逐个超时时，一次请求可以拖到 80 秒以上（实测 BMCLAPI
   被限流时就是这样）。这里把请求丢进 worker 线程并 join(deadline)，
   到点就放弃，保证启动流程不会被某条挂掉的源拖死。
2. **两条路 + 熔断**。候选来自 src.core.mirror.candidates，失败会记进
   全局熔断表，短时间内不再重复踩坑。
3. **可选 sha1 校验**（Mojang 给了哈希的清单必须校验）。
"""

from __future__ import annotations

import hashlib
import threading
import time
from typing import Optional

import requests

from src.core.health import health, host_of, is_hard_failure
from src.core.source_stats import stats
from src.core.logger import log
from src.core.mirror import candidates

__all__ = ["fetch_bytes", "fetch_json", "FetchError"]


_DEFAULT_TIMEOUT = (5, 20)     # (连接, 读取)
_DEFAULT_DEADLINE = 25.0       # 整体硬截止


class FetchError(Exception):
    """所有候选都失败。"""


def _request(url: str, timeout, deadline: float, headers: Optional[dict]):
    """在 worker 线程里跑请求，超时直接放弃（线程是 daemon，不会拖住退出）。"""
    box: dict = {}

    def work():
        try:
            box["resp"] = requests.get(url, timeout=timeout, headers=headers)
        except Exception as exc:      # noqa: BLE001
            box["error"] = exc

    thread = threading.Thread(target=work, daemon=True)
    thread.start()
    thread.join(deadline)
    if thread.is_alive():
        raise TimeoutError(f"超过 {deadline:.0f}s 未返回")
    if "error" in box:
        raise box["error"]
    return box.get("resp")


def fetch_bytes(
    url: str,
    *,
    sha1: str = "",
    timeout=_DEFAULT_TIMEOUT,
    deadline: float = _DEFAULT_DEADLINE,
    headers: Optional[dict] = None,
    source=None,
    kind: str = "",
) -> Optional[bytes]:
    """按两条路抓取资源，返回字节；全部失败返回 None。"""
    last_error = ""
    for source_name, candidate in candidates(url, kind=kind, source=source):
        host = host_of(candidate)
        if health().is_down(host):
            log.debug("元数据 | 跳过冷却中的源: %s", host)
            continue
        started = time.monotonic()
        try:
            resp = _request(candidate, timeout, deadline, headers)
            if resp.status_code in (403, 429):
                log.warning("元数据 | HTTP %d | %s", resp.status_code, candidate[:80])
            resp.raise_for_status()
            data = resp.content
            if sha1:
                actual = hashlib.sha1(data).hexdigest()
                if actual.lower() != sha1.lower():
                    raise ValueError(f"sha1 不符 (期望 {sha1[:12]}, 实际 {actual[:12]})")
            health().mark_ok(host)
            stats().record_success(source_name, len(data), time.monotonic() - started)
            log.info("元数据 | 源=%s | %d 字节 | %s", source_name, len(data), candidate[:90])
            return data
        except Exception as exc:      # noqa: BLE001
            last_error = f"{type(exc).__name__}: {exc}"
            health().mark_fail(host, hard=is_hard_failure(exc))
            stats().record_failure(source_name)
            log.warning("元数据失败 | 源=%s | %s | %s", source_name, candidate[:80], last_error[:120])
    log.error("元数据全部失败 | %s | %s", url[:90], last_error)
    return None


def fetch_json(url: str, **kwargs) -> Optional[dict]:
    """抓 JSON（内部走 fetch_bytes）。"""
    import json

    data = fetch_bytes(url, **kwargs)
    if data is None:
        return None
    try:
        return json.loads(data.decode("utf-8"))
    except Exception as exc:          # noqa: BLE001
        log.error("元数据 JSON 解析失败 | %s | %s", url[:80], exc)
        return None
