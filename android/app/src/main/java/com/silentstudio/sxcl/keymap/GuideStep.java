/*
 * 版权所有 © Silent X Craft Launcher Dev 开发团队
 *
 * Silent X Craft Launcher (SXCL) 是一款由 Silent X Craft Launcher Dev 团队开发，
 * 隶属于 SilentCodeTeams 旗下，并由 SilentStudio 管理的 Minecraft 第三方启动器。
 *
 * Copyright © Silent X Craft Launcher Development Team
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version, WITH the Additional Terms described
 * in the LICENSE file accompanying this program.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

package com.silentstudio.sxcl.keymap;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 一条教学步骤 —— "比 FCL 更好的按键帮助"的核心内容。
 *
 * 文案不在这边手写：桌面端 `src/core/keymap/guide.py` 生成后写进布局 JSON 的
 * meta.guide，这里只负责读出来显示。文案改一处（Python），两端一起变。
 */
public final class GuideStep {

    public int order;
    public String title = "";
    public String instruction = "";
    public String why = "";
    public String controlId = "";
    public List<String> keys = new ArrayList<String>();
    public String tap = "press";         // press / long_press / click / double_click
    public boolean optional;

    public static GuideStep fromJson(Object value) {
        Map<String, Object> map = Json.obj(value);
        GuideStep step = new GuideStep();
        step.order = (int) Json.num(map, "order", 0);
        step.title = Json.str(map, "title", "");
        step.instruction = Json.str(map, "instruction", "");
        step.why = Json.str(map, "why", "");
        step.controlId = Json.str(map, "control_id", "");
        step.tap = Json.str(map, "tap", "press");
        step.optional = Json.bool(map, "optional", false);
        for (Object key : Json.arr(map.get("keys"))) {
            step.keys.add(String.valueOf(key));
        }
        return step;
    }

    public Map<String, Object> toJson() {
        Map<String, Object> map = new LinkedHashMap<String, Object>();
        map.put("order", order);
        map.put("title", title);
        map.put("instruction", instruction);
        map.put("why", why);
        map.put("control_id", controlId);
        map.put("keys", new ArrayList<Object>(keys));
        map.put("tap", tap);
        map.put("optional", optional);
        return map;
    }

    /** 手势的中文说法，界面上显示成小标签。 */
    public String tapLabel() {
        switch (tap) {
            case "long_press": return "长按";
            case "double_click": return "双击";
            case "click": return "单击";
            default: return "按住";
        }
    }

    /**
     * 取布局里的教学步骤；没有 meta.guide 时（比如别人手写的 JSON）
     * 退化生成"每个控件一句话"，保证界面上永远有可看的东西。
     */
    public static List<GuideStep> fromLayout(KeymapLayout layout) {
        List<GuideStep> steps = new ArrayList<GuideStep>();
        Object raw = layout.meta == null ? null : layout.meta.get("guide");
        for (Object item : Json.arr(raw)) {
            steps.add(fromJson(item));
        }
        if (!steps.isEmpty()) {
            return steps;
        }
        int order = 1;
        for (Object control : layout.controls()) {
            GuideStep step = new GuideStep();
            step.order = order++;
            step.title = KeymapLayout.labelOf(control);
            step.controlId = KeymapLayout.idOf(control);
            step.instruction = KeymapLayout.hintOf(control);
            if (step.instruction.isEmpty()) {
                step.instruction = "点这个按钮试试";
            }
            step.keys = KeymapLayout.keysOf(control);
            steps.add(step);
        }
        return steps;
    }
}
