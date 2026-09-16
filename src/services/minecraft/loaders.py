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
"""已安装版本的扫描 + 模组加载器识别（兼容 PCL / HMCL 装出来的版本）。

三条硬性事实决定了这里怎么写：

1. **Forge 1.13+ / Fabric / Quilt 装的版本没有自己的 jar**（靠 inheritsFrom 继承原版）。
   以前按"jar + json 都在"判断，这些版本在界面里根本不显示。现在只要求 JSON。
2. **PCL 装出来的版本是"拍平"的单层 JSON**：没有 inheritsFrom、没有 jar，id == 文件夹名，
   另外多一个 clientVersion 字段（= Mojang 原始版本号）—— 这是它的身份标记
   （PCL：ModDownloadLib.vb:57、ModMinecraft.vb:339）。所以取原版版本号要优先看 clientVersion。
3. **识别加载器宁可多认几条线索**：JSON 里的 libraries 坐标、mainClass、以及整段 JSON 文本里的
   关键字符串（PCL 就是这么判的：ModMinecraft.vb:756-780），再加上目录名兜底。
   另外 PCL 会把状态缓存在 versions/<版本>/PCL/Setup.ini（Key:Value，无 section），
   我们只读不写，用来补全信息与显示它设过的自定义图标。
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

from src.core.logger import log, log_exception
from src.core.settings import settings

__all__ = [
    "LoaderKind", "LoaderInfo", "InstalledVersion",
    "detect_loaders", "scan_installed", "group_by_base", "installed_for_base",
    "loader_label", "default_version_name", "get_installed_versions", "get_version_info",
]


class LoaderKind:
    """加载器种类（字符串常量，方便直接写进 JSON/日志）。"""

    VANILLA = "vanilla"
    FORGE = "forge"
    NEOFORGE = "neoforge"
    FABRIC = "fabric"
    QUILT = "quilt"
    OPTIFINE = "optifine"
    LITELOADER = "liteloader"


LOADER_NAMES = {
    LoaderKind.VANILLA: "原版",
    LoaderKind.FORGE: "Forge",
    LoaderKind.NEOFORGE: "NeoForge",
    LoaderKind.FABRIC: "Fabric",
    LoaderKind.QUILT: "Quilt",
    LoaderKind.OPTIFINE: "OptiFine",
    LoaderKind.LITELOADER: "LiteLoader",
}

# 目录名顺序固定成 PCL 的拼接顺序（Fabric -> Forge -> NeoForge -> LiteLoader -> OptiFine）
LOADER_ORDER = (LoaderKind.FABRIC, LoaderKind.FORGE, LoaderKind.NEOFORGE,
                LoaderKind.LITELOADER, LoaderKind.OPTIFINE)

# maven 坐标前缀 -> 种类（neoforge 必须排在 forge 前面）
LIBRARY_RULES = (
    ("net.neoforged:neoforge", LoaderKind.NEOFORGE),
    ("net.neoforged:forge", LoaderKind.NEOFORGE),
    ("net.neoforge", LoaderKind.NEOFORGE),
    ("net.minecraftforge:forge", LoaderKind.FORGE),
    ("net.minecraftforge:fmlloader", LoaderKind.FORGE),
    ("net.minecraftforge:minecraftforge", LoaderKind.FORGE),
    ("net.fabricmc:fabric-loader", LoaderKind.FABRIC),
    ("net.fabricmc:intermediary", LoaderKind.FABRIC),
    ("org.quiltmc:quilt-loader", LoaderKind.QUILT),
    ("org.quiltmc:quilted-fabric-loader", LoaderKind.QUILT),
    ("optifine:OptiFine", LoaderKind.OPTIFINE),
    ("optifine:launchwrapper-of", LoaderKind.OPTIFINE),
    ("com.mumfrey:liteloader", LoaderKind.LITELOADER),
)

# mainClass 线索
MAIN_CLASS_RULES = (
    ("net.neoforged", LoaderKind.NEOFORGE),
    ("net.minecraftforge.bootstrap", LoaderKind.FORGE),
    ("cpw.mods.modlauncher", LoaderKind.FORGE),
    ("net.minecraftforge", LoaderKind.FORGE),
    ("net.fabricmc.loader", LoaderKind.FABRIC),
    ("org.quiltmc.loader", LoaderKind.QUILT),
    ("net.optifine", LoaderKind.OPTIFINE),
    ("com.mumfrey.liteloader", LoaderKind.LITELOADER),
)

# 整段 JSON 文本里的关键字符串 -> 种类（PCL 的判据，ModMinecraft.vb:756-780）
TEXT_RULES = (
    ("net.neoforge", LoaderKind.NEOFORGE),
    ("minecraftforge", LoaderKind.FORGE),
    ("net.fabricmc:fabric-loader", LoaderKind.FABRIC),
    ("org.quiltmc:quilt-loader", LoaderKind.QUILT),
    ("optifine", LoaderKind.OPTIFINE),
    ("liteloader", LoaderKind.LITELOADER),
)

# 目录名线索（兼容 PCL：1.20.1-Forge_47.2.0 / 1.20.1-Fabric 0.15.11 / 1.20.1-OptiFine_HD_U_I6）
NAME_RULES = (
    (re.compile(r"neoforge[_-]?(?P<ver>[0-9][0-9A-Za-z.]*)", re.I), LoaderKind.NEOFORGE),
    (re.compile(r"(?<!neo)forge[_-]?(?P<ver>[0-9][0-9A-Za-z.]*)", re.I), LoaderKind.FORGE),
    (re.compile(r"fabric[_-]?(?:loader[_-]?)?(?P<ver>[0-9][0-9A-Za-z.]*)", re.I), LoaderKind.FABRIC),
    (re.compile(r"quilt[_-]?(?:loader[_-]?)?(?P<ver>[0-9][0-9A-Za-z.]*)", re.I), LoaderKind.QUILT),
    (re.compile(r"optifine[_-]?(?P<ver>[0-9A-Za-z_.]+)", re.I), LoaderKind.OPTIFINE),
    (re.compile(r"liteloader[_-]?(?P<ver>[0-9A-Za-z_.]+)", re.I), LoaderKind.LITELOADER),
)

# 版本号：优先挑像 1.x / a1.x / b1.x 的那一段（fabric-loader-0.15.11-1.20.1 -> 1.20.1）
MC_VERSION_RE = re.compile(r"(?<![\w.\-])((?:1\.\d+|[ab]\d+\.\d+)(?:\.\d+)?)(?![\w.])")
ANY_VERSION_RE = re.compile(r"(?<![\w.\-])(\d+\.\d+(?:\.\d+)?)(?![\w.])")

# PCL 会为"看起来可能不是版本文件夹"的目录名跳过（ModMinecraft.vb:1443-1447）
SKIP_NAMES = ("cache", "BLClient", "PCL")


@dataclass(frozen=True)
class LoaderInfo:
    """识别出来的一个加载器。"""

    kind: str
    version: str = ""
    from_json: bool = False          # True=从 JSON 认出来，False=只能靠名字/缓存猜

    @property
    def name(self) -> str:
        return LOADER_NAMES.get(self.kind, self.kind)

    @property
    def display(self) -> str:
        return f"{self.name} {self.version}".strip()


@dataclass
class InstalledVersion:
    """一个已安装的版本目录。"""

    id: str                          # 目录名（也是启动时用的版本 id）
    base_version: str = ""           # 原版版本号
    loaders: List[LoaderInfo] = field(default_factory=list)
    has_jar: bool = False
    has_json: bool = False
    version_type: str = ""           # release / snapshot / old_alpha / old_beta
    broken: bool = False             # JSON 读不出来 / 缺 mainClass
    missing_parent: str = ""         # 缺哪个前置版本（PCL：需要安装 XXX 作为前置版本）
    launcher: str = ""               # pcl / hmcl / "" —— 兼容标记，识别出来只是想让用户少困惑
    custom_logo: Optional[Path] = None   # PCL 里设过的自定义图标（PCL\Logo.png + LogoCustom:True）
    pcl_state: str = ""              # PCL 缓存的 State（仅参考）

    @property
    def is_vanilla(self) -> bool:
        return not self.loaders

    @property
    def loader_kinds(self) -> List[str]:
        return [item.kind for item in self.loaders]

    @property
    def ready(self) -> bool:
        """能不能启动：有 JSON、没坏、前置版本在。"""
        return self.has_json and not self.broken and not self.missing_parent

    def has_loader(self, kind: str) -> bool:
        return any(item.kind == kind for item in self.loaders)

    def summary(self) -> str:
        """给界面用的一句话：原版 / Forge 61.0.11 / Forge 61 + OptiFine I6"""
        if not self.loaders:
            return "原版"
        return " + ".join(item.display for item in self.loaders)

    def problem(self) -> str:
        """不能启动时的一句话原因（空串 = 没问题）。"""
        if self.broken:
            return "版本 JSON 损坏或缺少 mainClass"
        if self.missing_parent:
            return f"需要安装 {self.missing_parent} 作为前置版本"
        if not self.has_json:
            return "缺少版本 JSON"
        return ""


# ── 解析工具 ──────────────────────────────────────────────────

def _coord_of(lib: dict) -> str:
    if not isinstance(lib, dict):
        return ""
    return str(lib.get("name") or (lib.get("downloads", {}).get("artifact", {}) or {}).get("path") or "")


def _version_from_coord(coord: str) -> str:
    parts = coord.split(":")
    return parts[2] if len(parts) >= 3 else ""


def _read_pcl_setup_ini(version_dir: Path) -> Dict[str, str]:
    """读 PCL 的状态缓存：versions/<版本>/PCL/Setup.ini。

    格式是每行 Key:Value（冒号分隔、没有 section、不是标准 INI —— PCL 自己写的），
    这里只读不写：改了它会让 PCL 显示旧信息（PCL 命中缓存后不再解析 JSON）。
    """
    ini = version_dir / "PCL" / "Setup.ini"
    data: Dict[str, str] = {}
    if not ini.is_file():
        return data
    try:
        for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
            if ":" not in line:
                continue
            key, _, value = line.partition(":")
            data[key.strip()] = value.strip()
    except Exception as exc:
        log.debug("读 PCL/Setup.ini 失败: %s", exc)
    return data


def _base_from_json(data: dict, version_id: str) -> Tuple[str, bool]:
    """按 PCL 的顺序推断原版版本号（返回值第二项是"是否可靠"）。"""
    # 1) PCL 的标记：拍平后的 JSON 靠它认祖归宗
    client_version = str(data.get("clientVersion") or "").strip()
    if client_version:
        return client_version, True
    # 2) HMCL：patches 里 id=game 的 version
    patches = data.get("patches")
    if isinstance(patches, list) and "time" not in data:
        for patch in patches:
            if isinstance(patch, dict) and str(patch.get("id")) == "game":
                value = str(patch.get("version") or "").strip()
                if value:
                    return value, True
    # 3) 标准继承
    inherit = str(data.get("inheritsFrom") or "").strip()
    if inherit:
        return inherit, True
    # 4) Forge 新版会在参数里写 --fml.mcVersion
    flat = json.dumps(data, ensure_ascii=False)
    match = re.search(r'--fml\\.mcVersion\x22\s*,\s*\x22([^\x22]+)\x22', flat)
    if match:
        return match.group(1), True
    # 5) jar 字段（LiteLoader 常靠它）
    jar = str(data.get("jar") or "").strip()
    if jar:
        return jar, True
    # 6) 目录名 / JSON id 兜底（不可靠）
    match = MC_VERSION_RE.search(version_id)
    if match:
        return match.group(1), False
    fallback = ANY_VERSION_RE.search(version_id)
    return (fallback.group(1) if fallback else ""), False


def detect_loaders(version_id: str, data: Optional[dict],
                   setup_ini: Optional[Dict[str, str]] = None) -> List[LoaderInfo]:
    """识别一个版本带了哪些加载器。

    线索优先级：libraries 坐标 > mainClass > 整段 JSON 文本（PCL 的判据）> PCL 的 Setup.ini > 目录名。
    同一加载器只保留一条（越靠前的线索版本号越准）。
    """
    found: Dict[str, LoaderInfo] = {}
    text = ""

    if isinstance(data, dict):
        for lib in data.get("libraries") or []:
            coord = _coord_of(lib)
            for prefix, kind in LIBRARY_RULES:
                if prefix in coord and kind not in found:
                    found[kind] = LoaderInfo(kind, _version_from_coord(coord), True)
                    break
        main_class = str(data.get("mainClass") or "")
        for prefix, kind in MAIN_CLASS_RULES:
            if prefix in main_class and kind not in found:
                found[kind] = LoaderInfo(kind, "", True)
        try:
            text = json.dumps(data, ensure_ascii=False)
        except Exception:
            text = ""

    if text:
        low = text.lower()
        for needle, kind in TEXT_RULES:
            if kind in found or needle.lower() not in low:
                continue
            if kind == LoaderKind.FORGE and "net.neoforge" in low:
                continue                       # PCL 的互斥判据：NeoForge 里也含 minecraftforge
            version = ""
            if kind == LoaderKind.OPTIFINE:
                match = re.search(r'(?<=HD_U_)[^\x22:/]+', text)
            elif kind == LoaderKind.NEOFORGE:
                match = re.search(r'neoForgeVersion\x22\s*,\s*\x22([^\x22]+)', text)
                if not match:
                    match = re.search(r'forgeVersion\x22\s*,\s*\x22([^\x22]+)', text)
            else:
                match = re.search(r"(?<=fabric-loader:)[0-9.]+|(?<=quilt-loader:)[0-9.]+", text)
            if match:
                version = str(match.group(0) if match.lastindex is None else match.group(1))
            found[kind] = LoaderInfo(kind, version.replace("+build", ""), True)

    for key, kind in (("VersionFabric", LoaderKind.FABRIC), ("VersionForge", LoaderKind.FORGE),
                      ("VersionNeoForge", LoaderKind.NEOFORGE), ("VersionOptiFine", LoaderKind.OPTIFINE),
                      ("VersionLiteLoader", LoaderKind.LITELOADER)):
        value = (setup_ini or {}).get(key, "").strip()
        if value and value.lower() != "unknown" and kind not in found:
            found[kind] = LoaderInfo(kind, value, False)

    for pattern, kind in NAME_RULES:
        if kind in found:
            continue
        match = pattern.search(version_id)
        if match:
            found[kind] = LoaderInfo(kind, (match.groupdict().get("ver") or "").strip("_-"), False)

    return [found[kind] for kind in LOADER_ORDER if kind in found]


def default_version_name(base_version: str, loaders: Dict[str, str]) -> str:
    """按 PCL 的规则拼默认版本名（GetSelectName，PageDownloadInstall.xaml.vb:412-420）。

    逐字对齐它的分隔符，这样从 PCL 过来的用户看到的名字是熟悉的：
        1.20.1-Fabric 0.15.0        （Fabric 后面是空格，没有下划线）
        1.20.1-Forge_47.2.0
        1.20.1-NeoForge_20.6.119     （取 + 之前的部分）
        1.20.1-LiteLoader            （不带版本号）
        1.20.1-OptiFine_HD_U_I6
    拼接顺序固定 Fabric -> Forge -> NeoForge -> LiteLoader -> OptiFine。
    """
    name = base_version or ""
    fabric = (loaders.get(LoaderKind.FABRIC) or "").split("+")[0].strip()
    if fabric:
        name += f"-Fabric {fabric}"
    forge = (loaders.get(LoaderKind.FORGE) or "").strip()
    if forge:
        name += f"-Forge_{forge}"
    neoforge = (loaders.get(LoaderKind.NEOFORGE) or "").split("+")[0].strip()
    if neoforge:
        name += f"-NeoForge_{neoforge}"
    if loaders.get(LoaderKind.LITELOADER):
        name += "-LiteLoader"
    optifine = (loaders.get(LoaderKind.OPTIFINE) or "").strip()
    if optifine:
        optifine = optifine.replace(f"{base_version} ", "").replace(" ", "_")
        name += f"-OptiFine_{optifine}"
    return name


def _pick_version_json(version_dir: Path, version_id: str) -> Optional[Path]:
    """同名 JSON 优先；没有就按 PCL 的规则找任意一个含 mainClass+type+id 的 JSON。"""
    same = version_dir / f"{version_id}.json"
    if same.is_file():
        return same
    for candidate in sorted(version_dir.glob("*.json")):
        try:
            data = json.loads(candidate.read_text(encoding="utf-8"))
        except Exception:
            continue
        if all(key in data for key in ("mainClass", "type", "id")):
            log.info("[Loaders] 没找到同名 JSON，改用 %s", candidate.name)
            return candidate
    return None


def scan_installed(game_dir: Optional[Path] = None) -> List[InstalledVersion]:
    """扫描 versions/ 下所有版本，识别加载器与健康状态。"""
    base = Path(game_dir) if game_dir else settings.game_directory()
    versions_dir = base / "versions"
    result: List[InstalledVersion] = []
    if not versions_dir.is_dir():
        return result

    for version_dir in sorted(versions_dir.iterdir()):
        if not version_dir.is_dir():
            continue
        version_id = version_dir.name
        files = [item for item in version_dir.iterdir() if item.is_file()]
        if not files:
            continue                                       # 空文件夹：PCL 也跳过
        has_jar = (version_dir / f"{version_id}.jar").is_file()
        json_path = _pick_version_json(version_dir, version_id)
        if json_path is None and not has_jar:
            continue                                       # 连 JSON 和 jar 都没有，不算版本
        if json_path is None and version_id in SKIP_NAMES:
            continue                                       # PCL 会跳过这几个目录

        data: Optional[dict] = None
        broken = False
        if json_path is not None:
            try:
                data = json.loads(json_path.read_text(encoding="utf-8"))
            except Exception:
                broken = True
                log.warning(f"[Loaders] 版本 JSON 读不出来: {json_path}")
        if isinstance(data, dict) and not data.get("mainClass"):
            broken = True                                  # PCL 的硬性门槛：没有 mainClass 不算版本
            log.warning(f"[Loaders] {version_id} 的 JSON 缺少 mainClass，按损坏处理")

        setup_ini = _read_pcl_setup_ini(version_dir)
        base_version, _reliable = _base_from_json(data or {}, version_id)
        loaders = detect_loaders(version_id, data, setup_ini)
        if base_version:
            loaders = [replace(item, version=item.version[len(base_version) + 1:])
                       if item.version.startswith(base_version + "-") else item
                       for item in loaders]

        # 前置版本在不在（PCL：需要安装 XXX 作为前置版本）
        missing_parent = ""
        if isinstance(data, dict):
            parent = str(data.get("inheritsFrom") or "").strip()
            if parent and not (versions_dir / parent / f"{parent}.json").is_file():
                missing_parent = parent

        launcher = ""
        if isinstance(data, dict) and isinstance(data.get("patches"), list) and "time" not in data:
            launcher = "hmcl"
        elif setup_ini or (version_dir / "PCL").is_dir():
            launcher = "pcl"

        custom_logo = None
        if setup_ini.get("LogoCustom", "").lower() in ("true", "1"):
            candidate = version_dir / "PCL" / "Logo.png"
            if candidate.is_file():
                custom_logo = candidate

        result.append(InstalledVersion(
            id=version_id,
            base_version=base_version,
            loaders=loaders,
            has_jar=has_jar,
            has_json=json_path is not None,
            version_type=str((data or {}).get("type") or ""),
            broken=broken,
            missing_parent=missing_parent,
            launcher=launcher,
            custom_logo=custom_logo,
            pcl_state=setup_ini.get("State", ""),
        ))
    return result


def group_by_base(versions: Iterable[InstalledVersion]) -> Dict[str, List[InstalledVersion]]:
    """按原版版本分组 —— 用户经常在同一个原版下装好几个加载器。"""
    groups: Dict[str, List[InstalledVersion]] = {}
    for item in versions:
        groups.setdefault(item.base_version or item.id, []).append(item)
    for items in groups.values():
        items.sort(key=lambda v: (v.is_vanilla, v.id))
    return groups


def installed_for_base(base_version: str, game_dir: Optional[Path] = None) -> List[InstalledVersion]:
    """某个原版版本下已经装了哪些（原版 / Forge / OptiFine…）。"""
    return group_by_base(scan_installed(game_dir)).get(base_version, [])


def loader_label(kind: str) -> str:
    return LOADER_NAMES.get(kind, kind)


# ── 兼容旧的调用点 ────────────────────────────────────────────

def get_installed_versions(game_dir: Optional[Path] = None) -> List[str]:
    """已安装版本 id 列表（倒序）。**不要求 jar**：Forge 1.13+/Fabric 就没有自己的 jar。"""
    try:
        return sorted((v.id for v in scan_installed(game_dir)), reverse=True)
    except Exception:
        log_exception(log, "get_installed_versions 失败")
        return []


def get_version_info(version_id: str, game_dir: Optional[Path] = None) -> Optional[dict]:
    """读取某个版本的 JSON（不存在/坏 JSON 返回 None）。"""
    try:
        base = Path(game_dir) if game_dir else settings.game_directory()
        version_dir = base / "versions" / version_id
        path = _pick_version_json(version_dir, version_id)
        if path is None:
            return None
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        log_exception(log, f"get_version_info 失败: {version_id}")
        return None
