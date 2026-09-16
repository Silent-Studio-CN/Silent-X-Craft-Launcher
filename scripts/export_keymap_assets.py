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
"""把按键布局 + 教学步骤导出成安卓端要用的资源（单一数据源）。

    python scripts/export_keymap_assets.py

为什么要有这个脚本：
    预设和教学文案只写在 Python 里（src/core/keymap/presets.py、guide.py），
    导出时把 `build_guide()` 的结果塞进布局 JSON 的 meta.guide，
    安卓端（Java）直接读，不用把文案再抄一遍，改文案只改一处。

产物：
    android/app/src/main/assets/keymaps/{preset}-{screen}.json   单个布局
    android/app/src/main/assets/keymaps/index.json               清单（列表页用）
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from src.core.constants import APP_VERSION                              # noqa: E402
from src.core.keymap import PRESET_LABELS, build_guide, build_preset, preset_names  # noqa: E402

SCREENS = {
    "landscape": "横屏",
    "portrait": "竖屏",
}
OUT_DIR = ROOT / "android" / "app" / "src" / "main" / "assets" / "keymaps"


def export() -> list[dict]:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    index: list[dict] = []
    seen: set[tuple[str, str]] = set()

    for requested, screen_label in SCREENS.items():
        for key in preset_names():
            layout = build_preset(key, requested)
            # 有些预设自带屏幕方向（单手模式永远是竖屏），以布局为准，别写出名不副实的文件
            screen = layout.screen if layout.screen in SCREENS else requested
            if (key, screen) in seen:
                continue
            seen.add((key, screen))
            screen_label = SCREENS[screen]
            steps = build_guide(layout)
            meta = dict(layout.meta or {})
            meta["guide"] = [step.to_dict() for step in steps]
            meta["generator"] = f"SXCL {APP_VERSION}"
            meta["screen_label"] = screen_label
            meta["preset"] = key
            layout.meta = meta

            path = OUT_DIR / f"{key}-{screen}.json"
            path.write_text(layout.to_json(), encoding="utf-8")

            index.append({
                "file": path.name,
                "key": f"{key}-{screen}",
                "preset": key,
                "name": layout.name,
                "label": f"{PRESET_LABELS.get(key, key)} · {screen_label}",
                "screen": screen,
                "screen_label": screen_label,
                "requested_screen": requested if screen != requested else "",
                "description": layout.description,
                "buttons": len(layout.buttons),
                "directions": len(layout.directions),
                "guide_steps": len(steps),
                "hints": sum(1 for c in layout.controls if getattr(c, "hint", "")),
                "conflicts": len(layout.conflicts()),
            })

    (OUT_DIR / "index.json").write_text(
        json.dumps({"schema": "sxcl.keymap.index.v1", "generator": f"SXCL {APP_VERSION}",
                    "items": index}, ensure_ascii=False, indent=2),
        encoding="utf-8")
    return index


def main() -> int:
    items = export()
    total = sum(p.stat().st_size for p in OUT_DIR.glob("*.json"))
    print(f"导出目录: {OUT_DIR}")
    for item in items:
        print(f"  {item['file']:<28} {item['label']:<20} 按钮 {item['buttons']} "
              f"摇杆 {item['directions']} 教学 {item['guide_steps']} 冲突 {item['conflicts']}")
    print(f"共 {len(items)} 个布局，{total / 1024:.1f} KB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
