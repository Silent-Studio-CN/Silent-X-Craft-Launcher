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
"""图标：全部用 QPainter 现画，不引入任何第三方图片资源。

为什么不用 PCL 那批 PNG（查过它的 LICENCE 了，结论明确）：
  1. PCL 用的是自定义的《PCL 分发有限许可》，不是 GPL/Apache/MIT。搬图属于它定义的
     "极小部分内容 = 轻度使用"，硬性义务是署名 + 不得暗示与 PCL 有关；
  2. 更要紧的是：那批图**本来就不是 PCL 原创** —— 草方块/铁砧/鸡蛋/圆石是 Mojang 的
     Minecraft 素材，布料与狐狸是 FabricMC / NeoForged 的 logo。PCL 无权再授权给我们，
     而且 Mojang 的素材条款与各项目商标政策另外管着。所以能照搬的只有"方块 -> 状态"
     这个对应关系（设计语言），图形一律自己画。
  3. 自己画还能跟主题走、任意缩放不失真、打包零体积。

对应关系借自 PCL（ModMinecraft.vb:785-809）：
    原版=草方块 / 快照=命令方块 / 旧版=圆石 / Forge=铁砧 / Fabric=布料
    NeoForge=狐狸 / OptiFine=土径 / LiteLoader=鸡蛋 / 出错=红石块
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QBrush, QColor, QIcon, QPainter, QPen, QPixmap
from PySide6.QtWidgets import QApplication

__all__ = [
    "block_pixmap", "block_icon", "grass_block_pixmap", "grass_block_icon",
    "loader_pixmap", "loader_color", "loader_chip_text", "state_icon_kind", "BLOCK_STYLES",
]

# 每种状态/加载器一套配色：(亮面, 暗面, 侧面, 侧面暗部)
BLOCK_STYLES = {
    "vanilla": ("#5fa03a", "#4a8230", "#8b5a2b", "#6f4720"),      # 草方块
    "snapshot": ("#b06a4a", "#8d5238", "#7a4630", "#5f3624"),     # 命令方块
    "old": ("#8f8f8f", "#6f6f6f", "#5c5c5c", "#454545"),          # 圆石
    "forge": ("#8a8a90", "#6a6a72", "#4a4a52", "#33333a"),        # 铁砧
    "neoforge": ("#e08a3c", "#b96b28", "#f2f2f2", "#c9c9c9"),     # 狐狸
    "fabric": ("#d8c9a3", "#b8a67f", "#8a7a55", "#6d6042"),       # 布料
    "quilt": ("#c05ad0", "#9a3aa8", "#d8c9a3", "#b8a67f"),        # Quilt（PCL 里复用布料）
    "optifine": ("#c9a06a", "#a37c4b", "#8b5a2b", "#6f4720"),     # 土径
    "liteloader": ("#f6ecd4", "#dccdaa", "#c2ae86", "#9d8a67"),   # 鸡蛋
    "error": ("#c0392b", "#8e2a1f", "#6f2118", "#4d1710"),        # 红石（出错）
}

# 版本行里小标签的底色
LOADER_COLORS = {
    "forge": "#c9a227",
    "neoforge": "#e08a3c",
    "fabric": "#b5752f",
    "quilt": "#b04ac0",
    "optifine": "#3f8fd0",
    "liteloader": "#9a8f5f",
    "vanilla": "#5fa03a",
}


def state_icon_kind(version_type: str = "", loaders=None, broken: bool = False) -> str:
    """按版本状态挑图标种类（照 PCL 的对应关系：原版/快照/旧版/各加载器）。"""
    if broken:
        return "error"
    loaders = loaders or []
    def _kind_of(item) -> str:
        # 支持三种形态：LoaderInfo 对象 / ("forge", "61.0.11") 元组 / 裸字符串
        if isinstance(item, (tuple, list)):
            return str(item[0]) if item else ""
        return str(getattr(item, "kind", item))

    for kind in ("forge", "neoforge", "fabric", "quilt", "optifine", "liteloader"):
        if any(_kind_of(item) == kind for item in loaders):
            return kind
    kind = (version_type or "").lower()
    if kind == "snapshot":
        return "snapshot"
    if kind in ("old_alpha", "old_beta", "old"):
        return "old"
    return "vanilla"


# PCL 的方块图标文件名（assets/icons/blocks 下，64x64 或 48x48 原图）
BLOCK_FILES = {
    "vanilla": "Grass.png",
    "snapshot": "CommandBlock.png",
    "old": "CobbleStone.png",
    "forge": "Anvil.png",
    "neoforge": "NeoForge.png",
    "fabric": "Fabric.png",
    "quilt": "Fabric.png",          # PCL 也没有独立的 Quilt 图标
    "optifine": "GrassPath.png",
    "liteloader": "Egg.png",
    "error": "RedstoneBlock.png",
    "gold": "GoldBlock.png",
    "optifabric": "OptiFabric.png",
    "lamp_on": "RedstoneLampOn.png",
    "lamp_off": "RedstoneLampOff.png",
}

_asset_cache: dict = {}
_scaled_cache: dict = {}


def screen_ratio() -> float:
    """当前屏幕缩放比（高分屏 1.25 / 1.5 / 2.0 很常见）。"""
    app = QApplication.instance()
    if app is None:
        return 1.0
    screen = app.primaryScreen()
    return float(screen.devicePixelRatio()) if screen is not None else 1.0


def _asset_dir() -> Optional[Path]:
    """找 assets/icons/blocks：开发时在仓库里，打包后在程序旁边或资源目录里。""",
    roots = [Path(__file__).resolve().parents[2]]
    frozen = getattr(sys, "_MEIPASS", None)          # PyInstaller
    if frozen:
        roots.append(Path(frozen))
    roots.append(Path(sys.executable).resolve().parent)   # Nuitka / 便携版
    roots.append(Path.cwd())
    for root in roots:
        candidate = Path(root) / "assets" / "icons" / "blocks"
        if candidate.is_dir():
            return candidate
    return None


def asset_pixmap(kind: str) -> Optional[QPixmap]:
    """取原图（不缩放）。找不到资源返回 None，由调用方退回手绘版本。""",
    name = BLOCK_FILES.get(kind)
    if not name:
        return None
    if name in _asset_cache:
        return _asset_cache[name]
    folder = _asset_dir()
    pixmap: Optional[QPixmap] = None
    if folder is not None:
        path = folder / name
        if path.is_file():
            loaded = QPixmap(str(path))
            pixmap = loaded if not loaded.isNull() else None
    _asset_cache[name] = pixmap
    return pixmap


def _scaled_asset(kind: str, size: int) -> Optional[QPixmap]:
    """原图缩放到目标像素尺寸：整倍用最近邻（锐利），非整倍用平滑。"""
    key = (kind, size)
    if key in _scaled_cache:
        return _scaled_cache[key]
    source = asset_pixmap(kind)
    if source is None:
        return None
    if source.width() == size:
        result = source
    else:
        exact = bool(size) and source.width() % size == 0
        mode = Qt.FastTransformation if exact else Qt.SmoothTransformation
        result = source.scaled(size, size, Qt.KeepAspectRatio, mode)
    _scaled_cache[key] = result
    return result

def _speckle(kind: str, x: int, y: int) -> bool:
    """确定性的颗粒（不用随机数，避免每次重绘都变）。"""
    seed = sum(ord(ch) for ch in kind) + x * 7 + y * 13
    return seed % 5 == 0


def block_pixmap(kind: str, size: int = 24) -> QPixmap:
    """方块的图，逻辑尺寸 size（物理像素按屏幕缩放比自动放大）。

    这就是"图标分辨率极低"的根因修复：以前直接给 22px 的图，在 125%/150% 缩放的屏幕上
    Qt 会放大绘制 -> 糊。现在按 DPR 生成并把 devicePixelRatio 标回去，Qt 就 1:1 画。
    """
    ratio = screen_ratio()
    physical = max(1, int(round(size * ratio)))
    pixmap = _scaled_asset(kind, physical)
    if pixmap is None:
        pixmap = _drawn_block(kind, physical)
    if ratio != 1.0:
        pixmap = QPixmap(pixmap)          # 别动缓存里那份
        pixmap.setDevicePixelRatio(ratio)
    return pixmap

def block_icon(kind: str, size: int = 24) -> QIcon:
    """给导航/列表用的 QIcon（多尺寸，缩放干净）。"""
    icon = QIcon()
    for side in sorted({size, max(16, size // 2), size * 2}):
        icon.addPixmap(block_pixmap(kind, side))
    return icon


def grass_block_pixmap(size: int = 24) -> QPixmap:
    """草方块（老调用点沿用的名字）。"""
    return block_pixmap("vanilla", size)


def grass_block_icon(size: int = 24) -> QIcon:
    """版本导航项用的草方块图标。"""
    return block_icon("vanilla", size)


def loader_pixmap(kind: str, size: int = 16) -> QPixmap:
    """加载器小图标（版本行标签里用）。"""
    return block_pixmap(kind if kind in BLOCK_STYLES else "old", size)


def loader_color(kind: str) -> str:
    return LOADER_COLORS.get(kind, "#8a8a8a")


def loader_chip_text(kind: str, version: str = "") -> str:
    """版本行里那个小标签的文字（太长就只留名字）。"""
    from src.services.minecraft.loaders import LOADER_NAMES

    name = LOADER_NAMES.get(kind, kind)
    version = (version or "").strip()
    if not version:
        return name
    text = f"{name} {version}"
    return text if len(text) <= 18 else name
