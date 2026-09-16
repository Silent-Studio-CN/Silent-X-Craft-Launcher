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
"""PCL 兼容层：读/写 PCL 放在版本目录与游戏目录里的那几个文件。

为什么必须在**改完版本之后**动这些文件（这是 PCL 的缓存机制决定的）：

    PCL 命中缓存后**不再解析版本 JSON**（ModMinecraft.vb:1379-1395）——
    只要 versions/<版本>/PCL/Setup.ini 里有 VersionVanillaName 且不是 Unknown、
    State 不是 Error，它就直接用缓存里的 VersionForge / VersionFabric… 构造版本信息。
    所以如果我们改了版本（装了加载器、改名、修好 JSON）却不动 Setup.ini，
    PCL 会一直显示旧信息 —— 这就是"不改 Setup 就会过久（显示旧的）"的由来。

    反过来，我们写 Setup.ini 之后**必须清 <游戏目录>/PCL.ini 里的 InstanceCache / CardKey* /
    CardValue***（PCL 靠它判断要不要重新扫版本列表，ModMinecraft.vb:1286、:1339），
    否则版本列表还是旧卡片。这个模块把这两步绑在一起做，避免谁忘了其中一步。

注意：
  * Setup.ini 不是标准 INI —— 每行 Key:Value，没有 section、没有方括号（ModBase.vb:554-559）。
  * 写入时**合并**，绝不整份重写：里面还有 PCL 自己的 26 项 Sources.Instance 设置，弄丢就是我们的错。
  * 行尾用 CRLF：PCL 写的是 vbCrLf。
"""

from __future__ import annotations

import json
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Optional

from src.core.logger import log

__all__ = [
    "read_setup_ini", "write_setup_ini", "state_for", "update_version_state", "sync_version",
    "invalidate_pcl_cache", "read_instance_java", "write_instance_java",
    "set_custom_logo", "clear_custom_logo", "merge_launcher_profile", "pcl_files_present",
]

SETUP_REL = ("PCL", "Setup.ini")
CONFIG_REL = ("PCL", "config.json")
LOGO_REL = ("PCL", "Logo.png")


# ── Setup.ini 读写 ────────────────────────────────────────────

def read_setup_ini(version_dir: Path) -> Dict[str, str]:
    """读 PCL 的版本级缓存（Key:Value，无 section）。读不出来就返回空 dict。"""
    path = Path(version_dir).joinpath(*SETUP_REL)
    data: Dict[str, str] = {}
    if not path.is_file():
        return data
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if ":" not in line:
                continue
            key, _, value = line.partition(":")
            data[key.strip()] = value.strip()
    except Exception as exc:
        log.debug("读 Setup.ini 失败: %s", exc)
    return data


def write_setup_ini(version_dir: Path, values: Dict[str, object]) -> bool:
    """把若干键合并进 Setup.ini（保留文件里其它键，CRLF 行尾）。"""
    version_dir = Path(version_dir)
    path = version_dir.joinpath(*SETUP_REL)
    try:
        merged = read_setup_ini(version_dir)
        for key, value in values.items():
            if value is None:
                merged.pop(key, None)
            else:
                merged[key] = str(value)
        path.parent.mkdir(parents=True, exist_ok=True)
        body = "".join(f"{key}:{value}\r\n" for key, value in merged.items())
        path.write_text(body, encoding="utf-8", newline="")
        log.info("[PCL] 已更新 %s（%d 个键）", path, len(values))
        return True
    except Exception as exc:
        log.warning("[PCL] 写 Setup.ini 失败: %s", exc)
        return False


def state_for(loaders: Iterable, version_type: str = "", broken: bool = False) -> str:
    """按 PCL 的优先级算出 State（ModMinecraft.vb:756-780 是"后置覆盖前置"）。

    PCL 的顺序：先看 OptiFine、再看 LiteLoader，最后 Fabric/Quilt 或 Forge/NeoForge 覆盖 State。
    我们按同一顺序算，保证 PCL 读到的 State 和它的算法一致。
    """
    if broken:
        return "Error"

    def kind_of(item) -> str:
        if isinstance(item, (tuple, list)):
            return str(item[0]) if item else ""
        return str(getattr(item, "kind", item))

    kinds = {kind_of(item) for item in loaders}
    state = ""
    if "optifine" in kinds:
        state = "OptiFine"
    if "liteloader" in kinds:
        state = "LiteLoader"
    if "fabric" in kinds or "quilt" in kinds:
        state = "Fabric"                     # PCL 把 Quilt 也当 Fabric
    elif "forge" in kinds:
        state = "Forge"
    elif "neoforge" in kinds:
        state = "NeoForge"
    if state:
        return state
    kind = (version_type or "").lower()
    if kind == "snapshot":
        return "Snapshot"
    if kind in ("old_alpha", "old_beta", "old"):
        return "Old"
    return "Original"


def update_version_state(game_dir: Path, version_id: str, *, loaders: Iterable = (),
                         base_version: str = "", version_type: str = "",
                         release_time: str = "", broken: bool = False,
                         invalidate_cache: bool = True) -> bool:
    """我们改过某个版本后，把 PCL 的缓存同步成事实（并清版本列表缓存）。

    写进 Setup.ini 的键与 PCL 自己写的一致（ModMinecraft.vb:829-841）：
        State / ReleaseTime / VersionVanillaName / VersionFabric / VersionForge /
        VersionNeoForge / VersionOptiFine / VersionLiteLoader
    """
    version_dir = Path(game_dir) / "versions" / version_id
    if not version_dir.is_dir():
        return False

    def version_of(kind: str) -> str:
        for item in loaders:
            current = (str(item[0]) if isinstance(item, (tuple, list)) and item
                       else str(getattr(item, "kind", item)))
            if current == kind:
                value = (str(item[1]) if isinstance(item, (tuple, list)) and len(item) > 1
                         else str(getattr(item, "version", "")))
                return value or "未知版本"
        return ""

    values: Dict[str, object] = {
        "State": state_for(loaders, version_type, broken),
        "VersionVanillaName": base_version or version_id,
    }
    if release_time:
        try:
            stamp = datetime.fromisoformat(release_time.replace("Z", "+00:00"))
            values["ReleaseTime"] = stamp.strftime("%Y-%m-%d %H:%M")
        except Exception:
            pass
    for kind, key in (("fabric", "VersionFabric"), ("forge", "VersionForge"),
                      ("neoforge", "VersionNeoForge"), ("optifine", "VersionOptiFine"),
                      ("liteloader", "VersionLiteLoader")):
        values[key] = version_of(kind) or None

    ok = write_setup_ini(version_dir, values)
    if ok and invalidate_cache:
        invalidate_pcl_cache(game_dir)
    return ok


def sync_version(game_dir: Path, version_id: str, *, invalidate_cache: bool = True) -> bool:
    """把我们刚改过的那个版本同步给 PCL（按真实扫描结果写，不靠调用方传参）。

    安装加载器、改名、修好缺失的 JSON 之后都该调它一次 —— 否则 PCL 会一直用缓存里的
    旧信息（它命中缓存就不再解析 JSON，ModMinecraft.vb:1379-1395）。
    """
    try:
        from src.services.minecraft.loaders import scan_installed
        for item in scan_installed(game_dir):
            if item.id != version_id:
                continue
            return update_version_state(
                game_dir, version_id,
                loaders=[(loader.kind, loader.version) for loader in item.loaders],
                base_version=item.base_version,
                version_type=item.version_type,
                broken=item.broken or bool(item.missing_parent),
                invalidate_cache=invalidate_cache,
            )
        log.debug("[PCL] sync_version 没找到版本: %s", version_id)
        return False
    except Exception as exc:
        log.warning("[PCL] 同步版本状态失败（%s）: %s", version_id, exc)
        return False


def invalidate_pcl_cache(game_dir: Path) -> bool:
    """清掉 <游戏目录>/PCL.ini 里的版本列表缓存，让 PCL 下次启动重新扫描。

    要清的键（PCL：ModMinecraft.vb:1286、:1304、:1339、:1605-1613）：
        InstanceCache（文件夹列表指纹）、CardCount / CardKey{i} / CardValue{i}（卡片缓存）
    只删这几个键，其余键（Version 当前选中版本、DisplayType 等）原样保留。
    """
    path = Path(game_dir) / "PCL.ini"
    if not path.is_file():
        return False
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        kept: List[str] = []
        removed = 0
        for line in lines:
            key = line.partition(":")[0].strip() if ":" in line else ""
            if key in ("InstanceCache", "CardCount") or key.startswith("CardKey") or key.startswith("CardValue"):
                removed += 1
                continue
            kept.append(line)
        if not removed:
            return False
        path.write_text("".join(line + "\r\n" for line in kept), encoding="utf-8", newline="")
        log.info("[PCL] 已清版本列表缓存 %d 项（%s）", removed, path)
        return True
    except Exception as exc:
        log.warning("[PCL] 清缓存失败: %s", exc)
        return False


# ── 实例级配置（PCL/config.json） ────────────────────────────

def _read_instance_config(version_dir: Path) -> Dict[str, object]:
    path = Path(version_dir).joinpath(*CONFIG_REL)
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}


def read_instance_java(version_dir: Path) -> str:
    """读 PCL 给这个版本单独指定的 Java（InstanceForcedJava）。

    用途：同一个版本目录被两个启动器共用时，行为保持一致 —— 用户在 PCL 里钉过 Java，
    我们启动时也该用它，否则会出现"PCL 能启动、SXCL 报 Java 不对"。
    """
    data = _read_instance_config(version_dir)
    value = str(data.get("InstanceForcedJava") or "").strip()
    return value if value and Path(value).exists() else ""


def write_instance_java(version_dir: Path, java_path: str) -> bool:
    """把"这个版本用哪个 Java"写进 PCL 的实例配置（空串 = 清除）。"""
    version_dir = Path(version_dir)
    path = version_dir.joinpath(*CONFIG_REL)
    try:
        data = _read_instance_config(version_dir)
        if java_path:
            data["InstanceForcedJava"] = str(java_path)
        else:
            data.pop("InstanceForcedJava", None)
        data.setdefault("InstanceMigratedJava", True)     # 告诉 PCL 这份配置已经是新格式
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        log.info("[PCL] 已写入实例 Java 设置: %s -> %s", version_dir.name, java_path or "（清除）")
        return True
    except Exception as exc:
        log.warning("[PCL] 写实例配置失败: %s", exc)
        return False


# ── 自定义图标（Logo.png + Logo + LogoCustom） ────────────────

def set_custom_logo(version_dir: Path, image_path: Optional[Path] = None) -> bool:
    """设置版本自定义图标。

    PCL 只在 LogoCustom 为真时才用 Logo（ModMinecraft.vb:785-786），所以两个键必须成对写。
    image_path 为空时只标记 Logo（图标文件已存在的情况）。
    """
    version_dir = Path(version_dir)
    target = version_dir.joinpath(*LOGO_REL)
    try:
        if image_path is not None:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(Path(image_path).read_bytes())
        if not target.is_file():
            return False
        return write_setup_ini(version_dir, {
            "Logo": str(target.relative_to(version_dir)).replace("/", "\\"),
            "LogoCustom": "True",
        })
    except Exception as exc:
        log.warning("[PCL] 设置自定义图标失败: %s", exc)
        return False


def clear_custom_logo(version_dir: Path) -> bool:
    """取消自定义图标（PCL 会退回按 State 自动选图标）。"""
    return write_setup_ini(version_dir, {"Logo": None, "LogoCustom": None})


# ── launcher_profiles.json（给官方安装器看的，不是版本索引） ──

def merge_launcher_profile(game_dir: Path, name: str = "Silent X Craft Launcher",
                           key: str = "SXCL") -> bool:
    """把我们的 profile 并进 launcher_profiles.json，**保留别人的条目**。

    PCL 也在用这个文件（它的用途是喂 Forge/OptiFine 官方安装器，ModDownloadLib.vb:1248、:551），
    而且登录后会往里补 authenticationDatabase —— 所以只能合并，绝不能整份覆盖。
    """
    path = Path(game_dir) / "launcher_profiles.json"
    try:
        data: Dict[str, object] = {}
        if path.is_file():
            try:
                data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
            except Exception:
                data = {}
        profiles = data.get("profiles")
        if not isinstance(profiles, dict):
            profiles = {}
            data["profiles"] = profiles
        if key in profiles:
            return True                        # 已经有了就不动它
        profiles[key] = {
            "icon": "Grass",
            "name": name,
            "lastVersionId": "latest-release",
            "type": "latest-release",
            "lastUsed": datetime.now().strftime("%Y-%m-%dT%H:%M:%S.0000Z"),
        }
        data.setdefault("selectedProfile", key)
        data.setdefault("clientToken", "23323323323323323323323323323333")
        Path(game_dir).mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, ensure_ascii=False, indent=4), encoding="utf-8")
        log.info("[PCL] 已合并 launcher_profiles.json（保留原有 %d 个 profile）", len(profiles))
        return True
    except Exception as exc:
        log.warning("[PCL] 合并 launcher_profiles.json 失败: %s", exc)
        return False


def pcl_files_present(version_dir: Path) -> bool:
    """这个版本目录里有没有 PCL 的痕迹（用来决定我们要不要同步它的缓存）。"""
    version_dir = Path(version_dir)
    return version_dir.joinpath(*SETUP_REL).is_file() or (version_dir / "PCL").is_dir()
