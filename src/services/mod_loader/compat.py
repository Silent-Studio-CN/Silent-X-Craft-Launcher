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
"""安装前的兼容性判定 —— 不兼容的组合不允许进入下一步。

规则都来自实测或官方支持矩阵，不做"猜"：

    error（禁止继续）
      * 这个 MC 版本根本没有该加载器（列表为空）—— 例如 1.12.1 没有 NeoForge；
      * 加载器版本列表没取到（网络/接口失败）—— 状态未知就不该让人装；
      * 该加载器我们还没有安装实现（LiteLoader / Quilt）。

    warn（可以继续，但要告诉用户）
      * OptiFine 的预览版会写明它配套的 Forge 版本（BMCLAPI 列表里的 forge 字段，
        例如 1.21.11 的 pre12 对应 Forge 61.0.6）—— 实测 OptiFine 独立也能装上，
        所以这不是硬门槛，但要提醒用户"想在 Forge 上用 OptiFine 就得先装那个 Forge"。
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional

from src.services.minecraft.loaders import LOADER_NAMES, LoaderKind

__all__ = ["Issue", "check_selection", "IMPLEMENTED_LOADERS", "parse_forge_requirement"]

# 我们已经实现静默安装的加载器（其余的选了也不能装，必须挡住）
IMPLEMENTED_LOADERS = (LoaderKind.FORGE, LoaderKind.NEOFORGE, LoaderKind.FABRIC, LoaderKind.OPTIFINE)

_FORGE_REQ = re.compile(r"forge\s*([0-9][0-9A-Za-z.\-]*)", re.I)


@dataclass(frozen=True)
class Issue:
    """一条兼容性结论。level=error 时界面应禁止继续。"""

    level: str                  # error / warn
    message: str
    fix: str = ""

    @property
    def is_error(self) -> bool:
        return self.level == "error"


def parse_forge_requirement(text: str) -> str:
    """从 OptiFine 列表的 forge 字段里取出 Forge 版本号（"Forge 61.0.6" -> "61.0.6"）。"""
    if not text:
        return ""
    match = _FORGE_REQ.search(str(text))
    return match.group(1) if match else ""


def check_selection(
    *,
    base_version: str,
    loader_type: Optional[str],
    loader_version: Optional[str],
    loader_versions: Dict[str, list],
    loader_failed: Iterable[str] = (),
    optifine_meta: Optional[dict] = None,
    installed_for_base: Optional[list] = None,
) -> List[Issue]:
    """给出这次选择的全部兼容性结论。空列表 = 没问题，可以继续。"""
    issues: List[Issue] = []
    failed = {name for name in loader_failed}

    # 没选加载器 = 只装原版：加载器列表取没取到都不影响，别拦着用户装原版
    if not loader_type:
        return issues

    # 1) 选中的那个加载器，列表没取到 -> 状态未知，不许装
    if loader_type in failed:
        issues.append(Issue(
            "error",
            f"{LOADER_NAMES.get(loader_type, loader_type)} 的版本列表没取到，无法确认能不能装",
            "检查网络后点右上角的刷新重试",
        ))

    # 2) 没实现的加载器
    if loader_type not in IMPLEMENTED_LOADERS:
        issues.append(Issue(
            "error",
            f"还不支持安装 {LOADER_NAMES.get(loader_type, loader_type)}",
            "先选 Forge / NeoForge / Fabric / OptiFine",
        ))

    # 3) 这个原版版本下该加载器一个版本都没有
    versions = loader_versions.get(loader_type) or []
    if not versions:
        issues.append(Issue(
            "error",
            f"{base_version} 没有可用的 {LOADER_NAMES.get(loader_type, loader_type)} 版本",
            "换一个游戏版本，或换别的加载器",
        ))

    # 4) 选了版本但不在列表里（列表刷新过 / 手改过）
    #    用序列化后的整体文本做包含判断：Fabric 的条目是 loader.version 嵌套结构，
    #    以前只取 item 的 version 字段，于是 0.19.5 明明在列表里也被判成不在可用列表。
    if loader_version and versions:
        import json as _json
        blob = _json.dumps(versions, ensure_ascii=False)
        if str(loader_version) not in blob:
            issues.append(Issue(
                "error",
                f"选中的 {LOADER_NAMES.get(loader_type, loader_type)} 版本（{loader_version}）不在可用列表里",
                "重新选一个版本",
            ))

    # 5) OptiFine 的配套 Forge：提示而不是拦截（实测 OptiFine 可以独立安装）
    if loader_type == LoaderKind.OPTIFINE and optifine_meta:
        needed = parse_forge_requirement(str(optifine_meta.get("forge") or ""))
        if needed:
            have = [
                item for item in (installed_for_base or [])
                if getattr(item, "has_loader", lambda _k: False)(LoaderKind.FORGE)
            ]
            matched = any(
                any(loader.version.startswith(needed) for loader in getattr(item, "loaders", []) if loader.kind == LoaderKind.FORGE)
                for item in have
            )
            if matched:
                issues.append(Issue("warn", f"已装有配套的 Forge {needed}，OptiFine 会跟它一起工作"))
            else:
                issues.append(Issue(
                    "warn",
                    f"这个 OptiFine 是配合 Forge {needed} 的（预览版信息）",
                    f"想搭配 Forge 用，先装 Forge {needed}；只装 OptiFine 也可以，它会独立运行",
                ))

    return issues
