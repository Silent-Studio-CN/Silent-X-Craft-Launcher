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

/** 屏幕上的一个按键（坐标是 0~1 归一化，任何分辨率都不会错位 —— FCL 用像素坐标会错位）。 */
public final class ControlButton {

    /** FCL 兼容的四种事件。 */
    public static final String PRESS = "press";
    public static final String LONG_PRESS = "long_press";
    public static final String CLICK = "click";
    public static final String DOUBLE_CLICK = "double_click";

    public String id = "";
    public String label = "";
    public String hint = "";
    public double x;
    public double y;
    public double w = 0.09;
    public double h = 0.16;
    public String shape = "round";        // round / rect
    public double opacity = 0.55;
    public String group = "";             // left / right / center
    public String aliasOf = "";           // 别名按钮：与某个动作等价（如「盾」= 右键）
    public Map<String, Binding> events = new LinkedHashMap<String, Binding>();

    public static ControlButton fromJson(Object value) {
        Map<String, Object> map = Json.obj(value);
        ControlButton button = new ControlButton();
        button.id = Json.str(map, "id", "");
        button.label = Json.str(map, "label", "");
        button.hint = Json.str(map, "hint", "");
        button.x = Json.num(map, "x", 0.1);
        button.y = Json.num(map, "y", 0.6);
        button.w = Json.num(map, "w", 0.09);
        button.h = Json.num(map, "h", 0.16);
        button.shape = Json.str(map, "shape", "round");
        button.opacity = Json.num(map, "opacity", 0.55);
        button.group = Json.str(map, "group", "");
        button.aliasOf = Json.str(map, "alias_of", "");
        Map<String, Object> events = Json.obj(map.get("events"));
        for (Map.Entry<String, Object> entry : events.entrySet()) {
            button.events.put(entry.getKey(), Binding.fromJson(entry.getValue()));
        }
        return button;
    }

    public Map<String, Object> toJson() {
        Map<String, Object> map = new LinkedHashMap<String, Object>();
        map.put("id", id);
        map.put("label", label);
        map.put("hint", hint);
        map.put("x", x);
        map.put("y", y);
        map.put("w", w);
        map.put("h", h);
        map.put("shape", shape);
        map.put("opacity", opacity);
        map.put("group", group);
        map.put("alias_of", aliasOf);
        Map<String, Object> out = new LinkedHashMap<String, Object>();
        for (Map.Entry<String, Binding> entry : events.entrySet()) {
            out.put(entry.getKey(), entry.getValue().toJson());
        }
        map.put("events", out);
        return map;
    }

    public Binding event(String kind) {
        return events.get(kind);
    }

    /** 该控件会用到的全部按键（冲突检测/搜索用）。 */
    public List<String> allKeys() {
        List<String> keys = new ArrayList<String>();
        for (Binding binding : events.values()) {
            for (String key : binding.keys) {
                if (!keys.contains(key)) {
                    keys.add(key);
                }
            }
        }
        return keys;
    }

    /** 按下的最简行为：能按住就按住，否则退化成点击。 */
    public Binding primary() {
        Binding binding = events.get(PRESS);
        if (binding == null) {
            binding = events.get(CLICK);
        }
        if (binding == null) {
            binding = events.get(LONG_PRESS);
        }
        return binding == null ? events.get(DOUBLE_CLICK) : binding;
    }

    public String shortLabel() {
        return label == null || label.isEmpty() ? id : label;
    }

    @Override
    public String toString() {
        return id;
    }
}
