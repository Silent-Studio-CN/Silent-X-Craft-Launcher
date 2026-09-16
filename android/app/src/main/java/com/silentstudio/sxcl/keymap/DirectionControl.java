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

/** 摇杆 / 方向键：FCL 有 dpad 与 rocker 两种，这里用 style 区分。 */
public final class DirectionControl {

    public static final String DPAD = "dpad_compact";
    public static final String ROCKER = "rocker";

    public String id = "";
    public String label = "";
    public String hint = "";
    public double x = 0.03;
    public double y = 0.55;
    public double w = 0.22;
    public double h = 0.38;
    public String style = DPAD;
    public double opacity = 0.5;
    public double deadZone = 0.18;        // 死区：手指在死区内视为不动，避免手一直按着走
    public String group = "left";
    public Map<String, String> keys = new LinkedHashMap<String, String>();
    public String sprintKey = "KEY_LEFT_SHIFT";   // 推到底 + 该键 = 疾跑

    public DirectionControl() {
        keys.put("up", "KEY_W");
        keys.put("down", "KEY_S");
        keys.put("left", "KEY_A");
        keys.put("right", "KEY_D");
    }

    public static DirectionControl fromJson(Object value) {
        Map<String, Object> map = Json.obj(value);
        DirectionControl control = new DirectionControl();
        control.id = Json.str(map, "id", "");
        control.label = Json.str(map, "label", "");
        control.hint = Json.str(map, "hint", "");
        control.x = Json.num(map, "x", 0.03);
        control.y = Json.num(map, "y", 0.55);
        control.w = Json.num(map, "w", 0.22);
        control.h = Json.num(map, "h", 0.38);
        control.style = Json.str(map, "style", DPAD);
        control.opacity = Json.num(map, "opacity", 0.5);
        control.deadZone = Json.num(map, "dead_zone", 0.18);
        control.group = Json.str(map, "group", "left");
        control.sprintKey = Json.str(map, "sprint_key", "KEY_LEFT_SHIFT");
        Map<String, Object> keys = Json.obj(map.get("keys"));
        control.keys.clear();
        for (Map.Entry<String, Object> entry : keys.entrySet()) {
            control.keys.put(entry.getKey(), String.valueOf(entry.getValue()));
        }
        if (control.keys.isEmpty()) {
            control.keys.put("up", "KEY_W");
            control.keys.put("down", "KEY_S");
            control.keys.put("left", "KEY_A");
            control.keys.put("right", "KEY_D");
        }
        return control;
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
        map.put("style", style);
        map.put("opacity", opacity);
        map.put("dead_zone", deadZone);
        map.put("group", group);
        map.put("keys", new LinkedHashMap<String, Object>(keys));
        map.put("sprint_key", sprintKey);
        return map;
    }

    /** 按归一化偏移算出该发哪个方向的键（死区内返回空）。 */
    public String directionOf(double dx, double dy) {
        if (Math.hypot(dx, dy) < deadZone) {
            return "";
        }
        if (Math.abs(dx) >= Math.abs(dy)) {
            return dx >= 0 ? "right" : "left";
        }
        return dy >= 0 ? "down" : "up";
    }

    public boolean isSprint(double dx, double dy) {
        return Math.hypot(dx, dy) >= 0.92;      // 推到底 = 疾跑
    }

    public List<String> allKeys() {
        return new ArrayList<String>(keys.values());
    }

    public String shortLabel() {
        return label == null || label.isEmpty() ? id : label;
    }

    @Override
    public String toString() {
        return id;
    }
}
