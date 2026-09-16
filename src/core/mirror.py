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
"""下载源定位器 —— 官方（第一条路）与 BMCLAPI 镜像（第二条路）。

本文件是**唯一**允许构造下载 URL 的地方。任何位置需要下载 Minecraft
相关资源时，都应调用这里的 candidates() 拿到"两条路"的候选列表，
再交给下载引擎按序回退。

实测结论（2026-09 用真实请求逐条验证，改动前请重跑 scripts/probe_sources.py）
--------------------------------------------------------------------------
1. piston-meta / piston-data / launchermeta 支持**路径透传**：
   https://piston-meta.mojang.com/v1/packages/<sha>/<id>.json
   -> https://bmclapi2.bangbang93.com/v1/packages/<sha>/<id>.json
   （官方 JRE 清单 v1/products/java-runtime/... 同理）
2. GET /version/<id>/json 返回的 JSON **内部 URL 没有被改写**（仍是官方域名），
   所以必须用 rewrite_tree() 自己改。
3. /version/<id>/<category> 只对 client / server / json 有效，
   其他 category 会**静默返回客户端 jar**（HTTP 200 + 30MB），绝不能用于资源索引。
4. maven.fabricmc.net 在 /maven/ 下**只有部分 artifact**：
   fabric-loader / intermediary 有，fabric-installer 三个版本全部 404。
5. Quilt 完全没有镜像（/quilt-meta 报 COMMON_NO_SUCH_OBJECT，/maven/ 也 404）。
6. /java/list 文档里有，实际 403，不要依赖。

因此候选列表不是"盲目换域名"，而是：
  候选 = [首选源, 另一条路]（命中 MIRROR_UNSAFE 的资源只给官方）
"""

from __future__ import annotations

from enum import Enum
from typing import Iterable, Optional

from src.core.constants import DownloadSource
from src.core.source_stats import stats

__all__ = [
    "Source", "MIRROR_UNSAFE_KINDS", "MIRROR_UNSAFE_HOSTS", "MIRROR_UNSAFE_PATTERNS",
    "MIRROR_ONLY_PATTERNS",
    "AUTO", "candidates", "rewrite_url", "official_url", "maybe_mirror_url", "rewrite_tree",
    "current_source", "is_bmclapi", "is_mirrored",
    "manifest_url", "library_url", "asset_object_url",
]

MIRROR_HOST = "bmclapi2.bangbang93.com"
MIRROR_BASE = f"https://{MIRROR_HOST}"

# 官方主机 → 镜像主机/前缀（**不含协议**：这里只做"主机名替换"，
# 因为原 URL 已经带了 https://，写上协议会拼出 https://https:// 这种坏地址）
# 顺序有意义：更具体的规则在前。
_HOST_RULES: list[tuple[str, str]] = [
    ("piston-meta.mojang.com", MIRROR_HOST),              # 版本 JSON / 资源索引 / JRE 清单
    ("piston-data.mojang.com", MIRROR_HOST),              # 客户端 jar / JRE 文件
    ("launchermeta.mojang.com", MIRROR_HOST),             # 版本清单 / java-runtime 清单
    ("launcher.mojang.com", MIRROR_HOST),                 # 早期版本资源（同路径透传）
    ("resources.download.minecraft.net", MIRROR_HOST + "/assets"),
    ("libraries.minecraft.net", MIRROR_HOST + "/maven"),
    ("maven.minecraftforge.net", MIRROR_HOST + "/maven"),
    ("files.minecraftforge.net/maven", MIRROR_HOST + "/maven"),
    ("meta.fabricmc.net", MIRROR_HOST + "/fabric-meta"),
    ("maven.fabricmc.net", MIRROR_HOST + "/maven"),
    ("maven.neoforged.net/releases", MIRROR_HOST + "/maven"),
    ("maven.neoforged.net", MIRROR_HOST + "/maven"),
]

# 实测不可镜像的主机：改写后返回 404 / COMMON_NO_SUCH_OBJECT
MIRROR_UNSAFE_HOSTS: tuple[str, ...] = (
    "maven.quiltmc.org",
    "meta.quiltmc.org",
)

# 镜像独有接口（官方没有等价物）：这些地址不要反查官方、也不要造第二条候选，
# 否则会拼出 https://piston-meta.mojang.com/forge/minecraft/1.21.1 这种必然 404 的地址。
MIRROR_ONLY_PATTERNS: tuple[str, ...] = (
    "/forge/minecraft",
    "/forge/list",
    "/forge/last",
    "/forge/promos",
    "/neoforge/list",
    "/neoforge/version",
    "/optifine/",
    "/fabric-meta/",
    "/liteloader/",
    "/java/list",
)

# 实测不可镜像的路径片段：即使调用方忘了传 kind，也不会拿到 404 的镜像地址
MIRROR_UNSAFE_PATTERNS: tuple[str, ...] = (
    "/fabric-installer/",      # 实测 /maven/net/fabricmc/fabric-installer/** -> 404
    "/quilt-installer/",
    "/org/quiltmc/",
)

# 实测不可镜像的资源种类（由调用方通过 kind= 传入）
MIRROR_UNSAFE_KINDS: frozenset[str] = frozenset({
    "fabric-installer",   # /maven/net/fabricmc/fabric-installer/** -> 404
    "quilt-maven",        # /maven/org/quiltmc/** -> 404
    "quilt-meta",         # /quilt-meta/** -> COMMON_NO_SUCH_OBJECT
    "java-list",          # /java/list -> 403
})


class Source(str, Enum):
    """下载源。"""

    OFFICIAL = "official"
    BMCLAPI = "bmclapi"

    @property
    def label(self) -> str:
        return "Mojang 官方源" if self is Source.OFFICIAL else "BMCLAPI 镜像源"


# ── 源选择 ────────────────────────────────────────────────────────


#: 取值表示"智能模式"：由实测数据决定先走哪条路
AUTO = "auto"


def current_source():
    """用户当前选择的下载源；智能模式返回字符串 "auto"。

    导入配置失败时退回镜像优先（国内默认体验），行为与旧版一致。
    """
    from src.core.settings import settings

    text = settings.download_source()
    if text == "auto":
        return AUTO
    return Source.BMCLAPI if text == "bmclapi" else Source.OFFICIAL


def _normalize(source):
    """None = 未指定（用配置）；AUTO = 智能模式；其余返回 Source。"""
    if source is None:
        return None
    if isinstance(source, Source):
        return source
    if isinstance(source, DownloadSource):
        source = source.value
    text = str(source).lower()
    if text in ("auto", "smart"):
        return AUTO
    if text in ("bmclapi", "mirror"):
        return Source.BMCLAPI
    if text in ("official", "mojang"):
        return Source.OFFICIAL
    return None


def is_mirrored(url: str) -> bool:
    return MIRROR_HOST in url


def _host_of(url: str) -> str:
    try:
        return url.split("//", 1)[1].split("/", 1)[0].lower()
    except Exception:
        return ""


# ── URL 改写 ──────────────────────────────────────────────────────


# 反查表（镜像路径 → 官方主机）。全部按"路径尾巴"拼接，避免出现
# https://https:// 或丢路径段这类错误。
_MIRROR_PATH_ROUTES: tuple[tuple[str, str], ...] = (
    # 镜像路径前缀            官方主机 + 路径前缀（镜像前缀之后的内容原样保留）
    ("/maven/net/minecraftforge/", "maven.minecraftforge.net/"),
    ("/maven/net/neoforged/", "maven.neoforged.net/releases/"),
    ("/maven/net/fabricmc/", "maven.fabricmc.net/"),
    ("/maven/", "libraries.minecraft.net/"),
    ("/assets/", "resources.download.minecraft.net/"),
    ("/fabric-meta/", "meta.fabricmc.net/"),
    ("/", "piston-meta.mojang.com/"),        # 兜底：piston-meta/data、launchermeta 同路径透传
)


def official_url(url: str) -> str:
    """把镜像 URL 反查回官方 URL（传入的已经是镜像地址时，也能给出真正的官方路）。"""
    if not is_mirrored(url):
        return url
    scheme, sep, rest = url.partition("://")
    prefix = (scheme + sep) if sep else ""
    if not rest.startswith(MIRROR_HOST):
        return url
    tail = rest[len(MIRROR_HOST):] or "/"
    for mirror_path, upstream in _MIRROR_PATH_ROUTES:
        if tail.startswith(mirror_path):
            # 去掉镜像特有的 /maven、/assets 前缀，保留 artifact 路径
            if mirror_path.startswith("/maven/"):
                keep = tail[len("/maven/"):]
            elif mirror_path == "/assets/":
                keep = tail[len("/assets/"):]
            elif mirror_path == "/fabric-meta/":
                keep = tail[len("/fabric-meta/"):]
            else:
                keep = tail.lstrip("/")
            return f"https://{upstream}{keep}"
    return url


def rewrite_url(url: str) -> Optional[str]:
    """把官方 URL 改写成镜像 URL；不可镜像时返回 None。"""
    if not url:
        return None
    if is_mirrored(url):
        return url
    if any(host in url for host in MIRROR_UNSAFE_HOSTS):
        return None
    if any(pattern in url for pattern in MIRROR_UNSAFE_PATTERNS):
        return None
    for official, mirror in _HOST_RULES:
        if official in url:
            return url.replace(official, mirror, 1)
    return None


def maybe_mirror_url(url: str, source: DownloadSource | str | None = None) -> str:
    """兼容旧接口：按当前源返回（可能被改写的）单个 URL。

    新代码请使用 candidates()，它才会带"两条路 + 回退"。
    """
    src = _normalize(source) or current_source()
    if src not in (Source.BMCLAPI, AUTO):
        return url
    return rewrite_url(url) or url


def candidates(
    url: str,
    *,
    kind: str = "",
    source: DownloadSource | str | None = None,
    fallback: bool = True,
) -> list[tuple[str, str]]:
    """返回 [(源名, URL), ...]，首选源在前；不可镜像时只返回官方。

    kind 用于标记那些"实测不能镜像"的资源种类（见 MIRROR_UNSAFE_KINDS）。
    """
    src = _normalize(source)
    if src is None:
        src = current_source()

    # 镜像独有接口：官方没有等价物，直接只给这一条（调用方各自做官方兜底）
    if is_mirrored(url) and any(p in url for p in MIRROR_ONLY_PATTERNS):
        return [("bmclapi", url)]

    official = official_url(url)          # 传进来是镜像地址也能还原出官方地址
    mirrored = None if kind in MIRROR_UNSAFE_KINDS else rewrite_url(official)

    if not mirrored:
        return [("official", official)] if official != url else [("official", url)]

    pairs = {"bmclapi": mirrored, "official": official}
    if src == AUTO:
        # 智能模式：用实测速度/失败记录决定顺序（没数据时仍是镜像优先）
        names = stats().order(["bmclapi", "official"])
        order = [(name, pairs[name]) for name in names]
    elif src is Source.BMCLAPI:
        order = [("bmclapi", mirrored), ("official", official)]
    else:
        order = [("official", official), ("bmclapi", mirrored)]
    if not fallback:
        order = order[:1]
    return order


def rewrite_tree(node, source: DownloadSource | str | None = None, *, kind: str = ""):
    """递归改写 Mojang JSON 里的 url 字段（版本 JSON / 资源索引 / JRE 清单）。

    /version/<id>/json 返回的内容里 URL 指向官方域名，必须整体改写后再落盘，
    否则后续所有下载都会绕过镜像。
    """
    src = _normalize(source) or current_source()
    if src not in (Source.BMCLAPI, AUTO):
        return node
    return _rewrite_node(node, kind)


def _rewrite_node(node, kind: str):
    if isinstance(node, dict):
        out = {}
        for key, value in node.items():
            if key == "url" and isinstance(value, str):
                out[key] = maybe_mirror_url(value, DownloadSource.BMCLAPI) if kind not in MIRROR_UNSAFE_KINDS else value
            else:
                out[key] = _rewrite_node(value, kind)
        return out
    if isinstance(node, list):
        return [_rewrite_node(item, kind) for item in node]
    return node


# ── 常用资源的官方 URL 构造 ───────────────────────────────────────


def manifest_url(source: DownloadSource | str | None = None) -> str:
    """版本清单 URL（v2，含 sha1，用于校验）。"""
    src = _normalize(source) or current_source()
    if src is Source.BMCLAPI:
        return MIRROR_BASE + "/mc/game/version_manifest_v2.json"
    return "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"


def library_url(maven_path: str) -> str:
    """依赖库官方 URL（Maven 路径）。"""
    return "https://libraries.minecraft.net/" + maven_path.lstrip("/")


def asset_object_url(object_hash: str) -> str:
    """资源对象官方 URL。"""
    return f"https://resources.download.minecraft.net/{object_hash[:2]}/{object_hash}"
