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
"""按键布局的存取 —— 存在配置目录里，桌面端与安卓端共用一份文件。

目录结构::

    {配置目录}/keymaps/
        preset-minimal.json     内置预设（首次运行写入，可覆盖）
        my-pvp.json             用户自己存的
    {配置目录}/keymaps/active.json   ->  {"active": "my-pvp"}

安卓端只要拿到这个目录（或导出的单个 JSON）就能直接用。
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import List, Optional

from src.core.keymap.model import KeymapLayout
from src.core.keymap.presets import PRESETS, PRESET_LABELS, build_preset
from src.core.logger import log

__all__ = ["keymap_dir", "list_layouts", "load", "save", "delete",
           "active_name", "set_active", "ensure_presets"]

_ACTIVE_FILE = "active.json"


def keymap_dir() -> Path:
    from src.core.platform import default_config_directory
    path = default_config_directory("SilentXCraftLauncher") / "keymaps"
    path.mkdir(parents=True, exist_ok=True)
    return path


def ensure_presets(overwrite: bool = False) -> List[Path]:
    """首次运行时把内置预设落盘（这样手机上也能直接选）。"""
    written: List[Path] = []
    for name in PRESETS:
        target = keymap_dir() / f"preset-{name}.json"
        if target.exists() and not overwrite:
            continue
        layout = build_preset(name)
        target.write_text(layout.to_json(), encoding="utf-8")
        written.append(target)
    if written:
        log.info("已写入内置按键预设: %s", ", ".join(p.name for p in written))
    return written


def list_layouts() -> List[dict]:
    """列出所有布局（含内置预设标记）。"""
    items: List[dict] = []
    for path in sorted(keymap_dir().glob("*.json")):
        if path.name == _ACTIVE_FILE:
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:      # noqa: BLE001
            log.warning("按键布局读取失败 %s: %s", path.name, exc)
            continue
        key = path.stem.removeprefix("preset-")
        items.append({
            "file": path.name,
            "key": key,
            "name": str(data.get("name") or path.stem),
            "screen": str(data.get("screen") or "landscape"),
            "buttons": len(data.get("buttons") or []),
            "directions": len(data.get("directions") or []),
            "builtin": path.name.startswith("preset-"),
            "label": PRESET_LABELS.get(key, str(data.get("name") or path.stem)),
        })
    return items


def load(key: str) -> Optional[KeymapLayout]:
    """按 key（文件名去掉 .json）或预设名加载布局。"""
    if not key:
        return None
    path = keymap_dir() / (key if key.endswith(".json") else f"{key}.json")
    if not path.exists() and key in PRESETS:
        path = keymap_dir() / f"preset-{key}.json"
        if not path.exists():
            ensure_presets()
    if not path.exists():
        return None
    try:
        return KeymapLayout.from_dict(json.loads(path.read_text(encoding="utf-8")))
    except Exception as exc:      # noqa: BLE001
        log.warning("按键布局解析失败 %s: %s", path.name, exc)
        return None


def save(layout: KeymapLayout, key: str = "") -> Path:
    """保存布局；key 为空时用布局名（会做文件名安全处理）。"""
    safe = "".join(ch for ch in (key or layout.name or "layout") if ch.isalnum() or ch in "-_")
    target = keymap_dir() / f"{safe or 'layout'}.json"
    target.write_text(layout.to_json(), encoding="utf-8")
    return target


def delete(key: str) -> bool:
    path = keymap_dir() / (key if key.endswith(".json") else f"{key}.json")
    if path.exists() and not path.name.startswith("preset-"):
        path.unlink()
        return True
    return False


def active_name() -> str:
    """当前启用的布局 key（安卓端启动时读它）。"""
    path = keymap_dir() / _ACTIVE_FILE
    if path.exists():
        try:
            return str(json.loads(path.read_text(encoding="utf-8")).get("active") or "")
        except Exception:      # noqa: BLE001
            return ""
    return "preset-minimal"


def set_active(key: str) -> None:
    (keymap_dir() / _ACTIVE_FILE).write_text(
        json.dumps({"active": key.removeprefix("preset-") if key.startswith("preset-") else key},
                   ensure_ascii=False, indent=2), encoding="utf-8")
