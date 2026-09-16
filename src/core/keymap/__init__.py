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
"""按键映射核心（跨端共用）。

    from src.core.keymap import build_preset, build_guide, list_layouts

* model.py    布局/控件/绑定数据模型 + 校验 + 冲突检测（schema: sxcl.keymap.v1）
* presets.py  内置场景预设（极简/生存/建造/对战/单手）
* guide.py    教学步骤生成（"比 FCL 更好的按键帮助"的核心）
* fcl.py      FCL 布局导入/导出，方便从 FCL 迁移
* store.py    布局存取（配置目录 keymaps/，安卓端读同一份）
"""

from src.core.keymap.fcl import import_layout as import_fcl_layout
from src.core.keymap.fcl import to_fcl
from src.core.keymap.guide import GuideStep, build_guide, guide_to_markdown
from src.core.keymap.model import (
    SCHEMA_VERSION,
    Binding,
    Conflict,
    ControlButton,
    DirectionControl,
    KeymapLayout,
    load_layout,
    save_layout,
)
from src.core.keymap.presets import PRESET_LABELS, PRESETS, build_preset, preset_names, recommend_preset
from src.core.keymap.store import (
    active_name,
    delete,
    ensure_presets,
    keymap_dir,
    list_layouts,
    load,
    save,
    set_active,
)

__all__ = [
    "SCHEMA_VERSION", "Binding", "ControlButton", "DirectionControl", "KeymapLayout",
    "Conflict", "load_layout", "save_layout",
    "PRESETS", "PRESET_LABELS", "build_preset", "preset_names", "recommend_preset",
    "GuideStep", "build_guide", "guide_to_markdown",
    "import_fcl_layout", "to_fcl",
    "keymap_dir", "list_layouts", "load", "save", "delete",
    "active_name", "set_active", "ensure_presets",
]
