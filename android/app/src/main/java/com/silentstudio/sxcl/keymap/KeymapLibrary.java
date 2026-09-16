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
 * 布局仓库：把"资源/文件里的 JSON"变成对象。
 *
 * 安卓端由 AssetLoader 从 assets/keymaps 读，桌面端测试直接从磁盘读，
 * 这样同一套解析代码在两边都能验证。
 */
public final class KeymapLibrary {

    /** 由调用方提供"文件名 -> 文本"的读取方式（安卓用 AssetManager，桌面用文件）。 */
    public interface Loader {
        String read(String name) throws Exception;
    }

    /** assets/keymaps/index.json 里的一条。 */
    public static final class Entry {
        public String file = "";
        public String key = "";
        public String name = "";
        public String label = "";
        public String screen = "landscape";
        public String screenLabel = "";
        public String description = "";
        public int buttons;
        public int directions;
        public int guideSteps;
        public int hints;
        public int conflicts;

        public static Entry fromJson(Object value) {
            Map<String, Object> map = Json.obj(value);
            Entry entry = new Entry();
            entry.file = Json.str(map, "file", "");
            entry.key = Json.str(map, "key", "");
            entry.name = Json.str(map, "name", "");
            entry.label = Json.str(map, "label", entry.name);
            entry.screen = Json.str(map, "screen", "landscape");
            entry.screenLabel = Json.str(map, "screen_label", "");
            entry.description = Json.str(map, "description", "");
            entry.buttons = (int) Json.num(map, "buttons", 0);
            entry.directions = (int) Json.num(map, "directions", 0);
            entry.guideSteps = (int) Json.num(map, "guide_steps", 0);
            entry.hints = (int) Json.num(map, "hints", 0);
            entry.conflicts = (int) Json.num(map, "conflicts", 0);
            return entry;
        }

        public int controlCount() {
            return buttons + directions;
        }
    }

    public static final String INDEX_FILE = "index.json";

    public static List<Entry> loadIndex(Loader loader) throws Exception {
        List<Entry> entries = new ArrayList<Entry>();
        String text = loader.read(INDEX_FILE);
        if (text == null || text.isEmpty()) {
            return entries;
        }
        for (Object item : Json.arr(Json.get(Json.parse(text), "items"))) {
            entries.add(Entry.fromJson(item));
        }
        return entries;
    }

    public static KeymapLayout load(Loader loader, String fileName) throws Exception {
        return KeymapLayout.fromJsonString(loader.read(fileName));
    }

    public static KeymapLayout loadEntry(Loader loader, Entry entry) throws Exception {
        return load(loader, entry.file);
    }

    /** 按 key 找一条；找不到返回 null。 */
    public static Entry findByKey(List<Entry> entries, String key) {
        for (Entry entry : entries) {
            if (entry.key.equals(key)) {
                return entry;
            }
        }
        return null;
    }

    /**
     * 把一份布局导出成 FCL 能读的 JSON（迁移用）。
     * 坐标换算成像素，因为 FCL 只认像素坐标。
     */
    public static Map<String, Object> toFcl(KeymapLayout layout, int screenWidth, int screenHeight) {
        List<Object> views = new ArrayList<Object>();
        for (Object control : layout.controls()) {
            Map<String, Object> view = new LinkedHashMap<String, Object>();
            double[] rect = KeymapLayout.rectOf(control);
            view.put("name", KeymapLayout.idOf(control));
            view.put("x", Math.round(rect[0] * screenWidth));
            view.put("y", Math.round(rect[1] * screenHeight));
            view.put("width", Math.round(rect[2] * screenWidth));
            view.put("height", Math.round(rect[3] * screenHeight));
            List<Object> keys = new ArrayList<Object>();
            if (control instanceof ControlButton) {
                for (Binding binding : ((ControlButton) control).events.values()) {
                    keys.addAll(binding.keys);
                }
            } else {
                DirectionControl direction = (DirectionControl) control;
                keys.addAll(direction.keys.values());
            }
            view.put("keys", keys);
            views.add(view);
        }
        Map<String, Object> root = new LinkedHashMap<String, Object>();
        root.put("views", views);
        root.put("screenWidth", screenWidth);
        root.put("screenHeight", screenHeight);
        return root;
    }
}
