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
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

/**
 * 一份按键布局（schema: sxcl.keymap.v1）。
 *
 * 这是桌面端和手机端唯一的数据契约：桌面端编辑器写它，手机端读它，
 * 安卓端在手机上改了也能再导回桌面端继续编辑。
 */
public final class KeymapLayout {

    public static final String SCHEMA = "sxcl.keymap.v1";

    public String schema = SCHEMA;
    public String name = "默认";
    public String screen = "landscape";      // landscape / portrait
    public String mcVersion = "";
    public String description = "";
    public Map<String, Object> meta = new LinkedHashMap<String, Object>();
    public List<ControlButton> buttons = new ArrayList<ControlButton>();
    public List<DirectionControl> directions = new ArrayList<DirectionControl>();

    // ── 解析/生成 ──────────────────────────────────────────────

    public static KeymapLayout fromJsonString(String text) {
        return fromJson(Json.parse(text));
    }

    public static KeymapLayout fromJson(Object value) {
        Map<String, Object> map = Json.obj(value);
        KeymapLayout layout = new KeymapLayout();
        layout.schema = Json.str(map, "schema", SCHEMA);
        layout.name = Json.str(map, "name", "默认");
        layout.screen = Json.str(map, "screen", "landscape");
        layout.mcVersion = Json.str(map, "mc_version", "");
        layout.description = Json.str(map, "description", "");
        layout.meta = Json.obj(map.get("meta"));
        for (Object item : Json.arr(map.get("directions"))) {
            layout.directions.add(DirectionControl.fromJson(item));
        }
        for (Object item : Json.arr(map.get("buttons"))) {
            layout.buttons.add(ControlButton.fromJson(item));
        }
        return layout;
    }

    public Map<String, Object> toJson() {
        Map<String, Object> map = new LinkedHashMap<String, Object>();
        map.put("schema", schema);
        map.put("name", name);
        map.put("screen", screen);
        map.put("mc_version", mcVersion);
        map.put("description", description);
        map.put("meta", new LinkedHashMap<String, Object>(meta));
        List<Object> dirs = new ArrayList<Object>();
        for (DirectionControl control : directions) {
            dirs.add(control.toJson());
        }
        map.put("directions", dirs);
        List<Object> btns = new ArrayList<Object>();
        for (ControlButton button : buttons) {
            btns.add(button.toJson());
        }
        map.put("buttons", btns);
        return map;
    }

    public String toJsonString() {
        return Json.write(toJson());
    }

    // ── 查询 ───────────────────────────────────────────────────

    /** 摇杆在前、按键在后，绘制和搜索都用这个顺序。 */
    public List<Object> controls() {
        List<Object> all = new ArrayList<Object>();
        all.addAll(directions);
        all.addAll(buttons);
        return all;
    }

    public Object control(String id) {
        for (Object control : controls()) {
            if (idOf(control).equals(id)) {
                return control;
            }
        }
        return null;
    }

    public static String idOf(Object control) {
        if (control instanceof ControlButton) {
            return ((ControlButton) control).id;
        }
        if (control instanceof DirectionControl) {
            return ((DirectionControl) control).id;
        }
        return "";
    }

    public static String labelOf(Object control) {
        if (control instanceof ControlButton) {
            return ((ControlButton) control).shortLabel();
        }
        if (control instanceof DirectionControl) {
            return ((DirectionControl) control).shortLabel();
        }
        return "";
    }

    public static String hintOf(Object control) {
        if (control instanceof ControlButton) {
            return ((ControlButton) control).hint;
        }
        if (control instanceof DirectionControl) {
            return ((DirectionControl) control).hint;
        }
        return "";
    }

    public static List<String> keysOf(Object control) {
        if (control instanceof ControlButton) {
            return ((ControlButton) control).allKeys();
        }
        if (control instanceof DirectionControl) {
            return ((DirectionControl) control).allKeys();
        }
        return new ArrayList<String>();
    }

    public static double[] rectOf(Object control) {
        if (control instanceof ControlButton) {
            ControlButton button = (ControlButton) control;
            return new double[] {button.x, button.y, button.w, button.h};
        }
        DirectionControl direction = (DirectionControl) control;
        return new double[] {direction.x, direction.y, direction.w, direction.h};
    }

    /** 动作 -> 拥有它的控件 id（"这个键在哪"搜索靠它）。 */
    public Map<String, List<String>> actionIndex() {
        Map<String, List<String>> index = new LinkedHashMap<String, List<String>>();
        for (ControlButton button : buttons) {
            for (Binding binding : button.events.values()) {
                if (!binding.action.isEmpty()) {
                    List<String> owners = index.get(binding.action);
                    if (owners == null) {
                        owners = new ArrayList<String>();
                        index.put(binding.action, owners);
                    }
                    owners.add(button.id);
                }
            }
        }
        for (DirectionControl direction : directions) {
            for (String keyName : direction.keys.keySet()) {
                List<String> owners = index.get(keyName);
                if (owners == null) {
                    owners = new ArrayList<String>();
                    index.put(keyName, owners);
                }
                owners.add(direction.id);
            }
        }
        return index;
    }

    /**
     * 模糊搜索控件 id：id / 标签 / 教学提示 / 按键名 / 动作名都能搜到。
     * 这是"比 FCL 好的按键帮助"里最实用的一条 —— FCL 只能肉眼在满屏按钮里找。
     */
    public List<String> find(String text) {
        List<String> hits = new ArrayList<String>();
        String query = text == null ? "" : text.trim().toLowerCase();
        if (query.isEmpty()) {
            return hits;
        }
        for (Object control : controls()) {
            List<String> haystack = new ArrayList<String>();
            haystack.add(idOf(control));
            haystack.add(labelOf(control));
            haystack.add(hintOf(control));
            haystack.addAll(keysOf(control));
            if (control instanceof ControlButton) {
                for (Binding binding : ((ControlButton) control).events.values()) {
                    haystack.add(binding.action);
                }
            } else {
                haystack.addAll(((DirectionControl) control).keys.keySet());
                haystack.add("sprint");
            }
            for (String item : haystack) {
                if (item != null && !item.isEmpty() && item.toLowerCase().contains(query)) {
                    hits.add(idOf(control));
                    break;
                }
            }
        }
        return hits;
    }

    // ── 校验 / 冲突 ────────────────────────────────────────────

    public List<String> validate() {
        List<String> errors = new ArrayList<String>();
        Set<String> seen = new LinkedHashSet<String>();
        for (Object control : controls()) {
            String id = idOf(control);
            if (id.isEmpty()) {
                errors.add("存在没有 id 的控件");
                continue;
            }
            if (!seen.add(id)) {
                errors.add("控件 id 重复: " + id);
            }
            double[] rect = rectOf(control);
            if (rect[0] + rect[2] > 1.001 || rect[1] + rect[3] > 1.001) {
                errors.add(id + ": 超出屏幕范围");
            }
            if (control instanceof ControlButton && ((ControlButton) control).events.isEmpty()) {
                errors.add(id + ": 没有绑定任何事件");
            }
        }
        return errors;
    }

    /**
     * 冲突检测（FCL 没有）。三类问题：
     *   1. 同一个按键被不同动作抢（同一个控件按下+长按同键、别名按钮不算）
     *   2. 两个控件位置叠在一起（触屏容易误触）
     *   3. 少了关键动作（跳跃/移动/背包），新手进游戏寸步难行
     */
    public List<Conflict> conflicts() {
        List<Conflict> result = new ArrayList<Conflict>();

        Map<String, List<String[]>> perKey = new TreeMap<String, List<String[]>>();
        for (Object control : controls()) {
            Map<String, Set<String>> actionsByKey = new LinkedHashMap<String, Set<String>>();
            String alias = control instanceof ControlButton ? ((ControlButton) control).aliasOf : "";
            for (String key : keysOf(control)) {
                Set<String> actions = new LinkedHashSet<String>();
                if (alias != null && !alias.isEmpty()) {
                    actions.add(alias);
                } else if (control instanceof ControlButton) {
                    for (Binding binding : ((ControlButton) control).events.values()) {
                        if (binding.keys.contains(key) && !binding.action.isEmpty()) {
                            actions.add(binding.action);
                        }
                    }
                }
                Set<String> existing = actionsByKey.get(key);
                if (existing == null) {
                    actionsByKey.put(key, actions);
                } else {
                    existing.addAll(actions);
                }
            }
            for (Map.Entry<String, Set<String>> entry : actionsByKey.entrySet()) {
                List<String> sorted = new ArrayList<String>(entry.getValue());
                java.util.Collections.sort(sorted);
                List<String[]> owners = perKey.get(entry.getKey());
                if (owners == null) {
                    owners = new ArrayList<String[]>();
                    perKey.put(entry.getKey(), owners);
                }
                owners.add(new String[] {idOf(control), String.join(",", sorted)});
            }
        }

        for (Map.Entry<String, List<String[]>> entry : perKey.entrySet()) {
            List<String[]> owners = entry.getValue();
            if (owners.size() < 2) {
                continue;
            }
            Set<String> allActions = new LinkedHashSet<String>();
            boolean multi = false;
            for (String[] owner : owners) {
                for (String action : owner[1].split(",")) {
                    if (!action.isEmpty()) {
                        allActions.add(action);
                    }
                }
                if (owner[1].contains(",")) {
                    multi = true;
                }
            }
            if (allActions.size() <= 1 && !multi) {
                continue;
            }
            StringBuilder names = new StringBuilder();
            List<String> ownerIds = new ArrayList<String>();
            for (String[] owner : owners) {
                if (names.length() > 0) {
                    names.append("、");
                }
                names.append(owner[0]);
                ownerIds.add(owner[0]);
            }
            result.add(new Conflict(Conflict.WARNING,
                    "按键 " + entry.getKey() + " 被 " + owners.size() + " 个控件绑到不同动作（"
                            + names + "），会互相打架", ownerIds));
        }

        List<Object> all = controls();
        for (int i = 0; i < all.size(); i++) {
            for (int j = i + 1; j < all.size(); j++) {
                double ratio = overlapRatio(rectOf(all.get(i)), rectOf(all.get(j)));
                if (ratio > 0.6) {
                    result.add(new Conflict(Conflict.WARNING,
                            idOf(all.get(i)) + " 与 " + idOf(all.get(j)) + " 位置重叠（"
                                    + Math.round(ratio * 100) + "%），触屏容易误触",
                            new ArrayList<String>(java.util.Arrays.asList(
                                    idOf(all.get(i)), idOf(all.get(j))))));
                }
            }
        }

        Set<String> actions = actionIndex().keySet();
        addMissing(result, actions, "jump", "跳跃");
        if (directions.isEmpty()) {
            addMissing(result, actions, "forward", "移动");
        }
        addMissing(result, actions, "inventory", "打开背包");
        return result;
    }

    private static void addMissing(List<Conflict> result, Set<String> actions, String action, String tip) {
        if (!actions.contains(action)) {
            result.add(new Conflict(Conflict.ERROR, "没有绑定「" + tip + "」，进游戏后会寸步难行"));
        }
    }

    private static double overlapRatio(double[] first, double[] second) {
        double left = Math.max(first[0], second[0]);
        double top = Math.max(first[1], second[1]);
        double right = Math.min(first[0] + first[2], second[0] + second[2]);
        double bottom = Math.min(first[1] + first[3], second[1] + second[3]);
        double width = right - left;
        double height = bottom - top;
        if (width <= 0 || height <= 0) {
            return 0;
        }
        double overlap = width * height;
        double smaller = Math.min(first[2] * first[3], second[2] * second[3]);
        return smaller <= 0 ? 0 : overlap / smaller;
    }

    /** 布局里的控件总数（列表页显示用）。 */
    public int controlCount() {
        return buttons.size() + directions.size();
    }

    public String screenLabel() {
        return "portrait".equals(screen) ? "竖屏" : "横屏";
    }

    @Override
    public String toString() {
        return name + "(" + screen + ", " + controlCount() + " 控件)";
    }
}
