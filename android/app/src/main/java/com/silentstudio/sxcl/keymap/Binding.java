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

/** 一个事件（按下/长按/点击/双击）背后的逻辑动作与要注入的按键。 */
public final class Binding {

    public static final String HOLD = "hold";       // 按住时保持按下（挖矿、潜行）
    public static final String TOGGLE = "toggle";   // 点一下切换（潜行这种可以常开）
    public static final String TAP = "tap";         // 只发一次按下+抬起（背包、Esc）

    public String action = "";
    public List<String> keys = new ArrayList<String>();
    public String behavior = HOLD;

    public Binding() {
    }

    public Binding(String action, String behavior, String... keys) {
        this.action = action;
        this.behavior = behavior;
        for (String key : keys) {
            this.keys.add(key);
        }
    }

    public boolean isToggle() {
        return TOGGLE.equals(behavior);
    }

    public static Binding fromJson(Object value) {
        Map<String, Object> map = Json.obj(value);
        Binding binding = new Binding();
        binding.action = Json.str(map, "action", "");
        binding.behavior = Json.str(map, "behavior", HOLD);
        for (Object key : Json.arr(map.get("keys"))) {
            binding.keys.add(String.valueOf(key));
        }
        return binding;
    }

    public Map<String, Object> toJson() {
        Map<String, Object> map = new LinkedHashMap<String, Object>();
        map.put("action", action);
        map.put("keys", new ArrayList<Object>(keys));
        map.put("behavior", behavior);
        return map;
    }

    @Override
    public String toString() {
        return action + keys + "/" + behavior;
    }
}
