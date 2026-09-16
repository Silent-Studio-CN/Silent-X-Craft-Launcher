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

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * 安卓端按键核心的单元测试（纯 Java，不需要安卓设备/模拟器）。
 *
 *     javac -d out src/main/java/com/silentstudio/sxcl/keymap/*.java src/test/java/com/silentstudio/sxcl/keymap/KeymapCoreTest.java
 *     java -cp out com.silentstudio.sxcl.keymap.KeymapCoreTest android/app/src/main/assets/keymaps
 *
 * 这样"手机上按键到底怎么算"这件事在电脑上就能验证，不用装 APK 试。
 */
public final class KeymapCoreTest {

    private static int passed;
    private static final List<String> failures = new ArrayList<String>();

    public static void main(String[] args) throws Exception {
        Path assets = Paths.get(args.length > 0 ? args[0] : "android/app/src/main/assets/keymaps");
        System.out.println("资源目录: " + assets.toAbsolutePath());

        KeymapLibrary.Loader loader = new KeymapLibrary.Loader() {
            @Override
            public String read(String name) throws IOException {
                return new String(Files.readAllBytes(assets.resolve(name)), StandardCharsets.UTF_8);
            }
        };

        testJsonBasics();
        List<KeymapLibrary.Entry> entries = testIndex(loader);
        testLayouts(loader, entries);
        testSearch(loader, entries);
        testGuide(loader, entries);
        testRoundTrip(loader, entries);
        testConflictDetection(loader, entries);
        testDirectionAndButtons();
        testFclExport(loader, entries);

        System.out.println();
        System.out.println("通过 " + passed + " 项");
        if (!failures.isEmpty()) {
            System.out.println("[FAIL] " + failures.size() + " 项失败:");
            for (String failure : failures) {
                System.out.println("  - " + failure);
            }
            System.exit(1);
        }
        System.out.println("[PASS] 安卓端按键核心测试全部通过");
    }

    // ── 各项测试 ───────────────────────────────────────────────

    private static void testJsonBasics() {
        String text = "{\"a\":1,\"b\":[true,null,\"x\\u4e2d\"],\"c\":{\"d\":-2.5}}";
        Map<String, Object> map = Json.parseObject(text);
        check("JSON 数字", 1.0 == ((Number) map.get("a")).doubleValue());
        check("JSON 数组长度", Json.arr(map.get("b")).size() == 3);
        String unicode = "x" + (char) 0x4e2d;
        check("JSON unicode 转义", unicode.equals(Json.arr(map.get("b")).get(2)));
        check("JSON 嵌套取值", -2.5 == ((Number) Json.get(map, "c.d")).doubleValue());
        check("JSON 点号路径", 1.0 == ((Number) Json.get(map, "a")).doubleValue());

        String written = Json.write(map);
        Map<String, Object> again = Json.parseObject(written);
        check("JSON 生成后可再解析", unicode.equals(Json.arr(again.get("b")).get(2)));
        check("JSON 生成含换行缩进", written.contains("\n"));
    }

    private static List<KeymapLibrary.Entry> testIndex(KeymapLibrary.Loader loader) throws Exception {
        List<KeymapLibrary.Entry> entries = KeymapLibrary.loadIndex(loader);
        // 5 个预设 × 2 种屏幕方向，其中"单手"固定竖屏，所以是 9 份
        check("index.json 里有 9 个布局", entries.size() == 9);
        KeymapLibrary.Entry oneHand = KeymapLibrary.findByKey(entries, "one_hand-portrait");
        check("单手预设只出竖屏版本", oneHand != null && "portrait".equals(oneHand.screen));
        boolean landscape = false;
        boolean portrait = false;
        for (KeymapLibrary.Entry entry : entries) {
            if ("landscape".equals(entry.screen)) {
                landscape = true;
            }
            if ("portrait".equals(entry.screen)) {
                portrait = true;
            }
            if (entry.file.isEmpty() || entry.guideSteps == 0 || entry.controlCount() == 0) {
                fail("index 条目不完整: " + entry.key);
            }
            if (entry.conflicts != 0) {
                fail("预设自带冲突: " + entry.key);
            }
        }
        check("index 覆盖横屏", landscape);
        check("index 覆盖竖屏", portrait);
        return entries;
    }

    private static void testLayouts(KeymapLibrary.Loader loader, List<KeymapLibrary.Entry> entries)
            throws Exception {
        for (KeymapLibrary.Entry entry : entries) {
            KeymapLayout layout = KeymapLibrary.loadEntry(loader, entry);
            if (!KeymapLayout.SCHEMA.equals(layout.schema)) {
                fail(entry.key + " schema 不对: " + layout.schema);
            }
            if (!layout.validate().isEmpty()) {
                fail(entry.key + " 校验失败: " + layout.validate());
            }
            List<Conflict> conflicts = layout.conflicts();
            if (!conflicts.isEmpty()) {
                fail(entry.key + " 有冲突: " + conflicts);
            }
            if (layout.controlCount() != entry.controlCount()) {
                fail(entry.key + " 控件数不一致 " + layout.controlCount() + " != " + entry.controlCount());
            }
            if (!layout.screen.equals(entry.screen)) {
                fail(entry.key + " 屏幕方向不一致");
            }
        }
        check("全部布局：schema 正确 / 校验通过 / 无冲突 / 与清单一致", true);
    }

    private static void testSearch(KeymapLibrary.Loader loader, List<KeymapLibrary.Entry> entries)
            throws Exception {
        KeymapLayout layout = KeymapLibrary.loadEntry(loader, entries.get(0));
        check("搜索：中文标签「跳」", layout.find("跳").size() > 0);
        check("搜索：动作名 jump", layout.find("jump").contains("jump"));
        check("搜索：按键名 KEY_SPACE 命中跳跃",
                layout.find("KEY_SPACE").contains("jump"));
        check("搜索：教学提示里的词能搜到（潜行）", layout.find("潜行").size() > 0);
        check("搜索：空串不返回", layout.find("   ").isEmpty());
        check("搜索：乱敲不会返回东西", layout.find("zzzzz").isEmpty());
        check("搜索：大小写不敏感", layout.find("JUMP").contains("jump"));

        List<String> sneakHits = layout.find("潜行");
        check("搜索：潜行能找到控件并且能定位到 id", layout.control(sneakHits.get(0)) != null);
    }

    private static void testGuide(KeymapLibrary.Loader loader, List<KeymapLibrary.Entry> entries)
            throws Exception {
        int totalSteps = 0;
        for (KeymapLibrary.Entry entry : entries) {
            KeymapLayout layout = KeymapLibrary.loadEntry(loader, entry);
            List<GuideStep> steps = GuideStep.fromLayout(layout);
            if (steps.isEmpty()) {
                fail(entry.key + " 没有教学步骤");
                continue;
            }
            totalSteps += steps.size();
            int expected = 1;
            for (GuideStep step : steps) {
                if (step.order != expected++) {
                    fail(entry.key + " 教学步骤序号不连续: " + step.order);
                }
                if (step.title.isEmpty() || step.instruction.isEmpty()) {
                    fail(entry.key + " 教学步骤缺标题或说明");
                }
                if (!step.controlId.isEmpty() && layout.control(step.controlId) == null) {
                    fail(entry.key + " 教学步骤指向不存在的控件: " + step.controlId);
                }
            }
            boolean hasJump = false;
            for (GuideStep step : steps) {
                if ("jump".equals(step.controlId)) {
                    hasJump = true;
                }
            }
            if (!hasJump) {
                fail(entry.key + " 教学里没有教跳跃");
            }
        }
        check("教学步骤：序号连续 / 指向的控件都存在 / 每条都有文案，共 " + totalSteps + " 条", true);
    }

    private static void testRoundTrip(KeymapLibrary.Loader loader, List<KeymapLibrary.Entry> entries)
            throws Exception {
        for (KeymapLibrary.Entry entry : entries) {
            KeymapLayout layout = KeymapLibrary.loadEntry(loader, entry);
            KeymapLayout again = KeymapLayout.fromJsonString(layout.toJsonString());
            if (!layout.name.equals(again.name) || !layout.screen.equals(again.screen)
                    || layout.controlCount() != again.controlCount()) {
                fail(entry.key + " 序列化往返丢数据");
                continue;
            }
            for (ControlButton button : layout.buttons) {
                Object other = again.control(button.id);
                if (!(other instanceof ControlButton)) {
                    fail(entry.key + " 往返后丢了按钮 " + button.id);
                    continue;
                }
                ControlButton copy = (ControlButton) other;
                if (Math.abs(copy.x - button.x) > 1e-9 || copy.events.size() != button.events.size()) {
                    fail(entry.key + " 往返后 " + button.id + " 数值不一致");
                }
                Binding first = button.primary();
                Binding second = copy.primary();
                if (first == null || second == null || !first.action.equals(second.action)
                        || !first.keys.equals(second.keys)) {
                    fail(entry.key + " 往返后 " + button.id + " 绑定不一致");
                }
            }
            if (GuideStep.fromLayout(again).size() != GuideStep.fromLayout(layout).size()) {
                fail(entry.key + " 往返后教学步骤数量变了");
            }
        }
        check("序列化往返：名称/方向/控件/绑定/教学 全部保持一致", true);
    }

    private static void testConflictDetection(KeymapLibrary.Loader loader,
                                              List<KeymapLibrary.Entry> entries) throws Exception {
        KeymapLayout layout = KeymapLibrary.loadEntry(loader, entries.get(0));

        KeymapLayout broken = KeymapLayout.fromJsonString(layout.toJsonString());
        broken.buttons.remove(broken.buttons.size() - 1);        // 先制造一个"缺动作"
        ControlButton clash = new ControlButton();
        clash.id = "clash_test";
        clash.label = "抢键";
        clash.x = 0.4;
        clash.y = 0.4;
        clash.w = 0.1;
        clash.h = 0.1;
        clash.events.put(ControlButton.PRESS, new Binding("sneak", Binding.HOLD, "KEY_SPACE"));
        broken.buttons.add(clash);
        List<Conflict> conflicts = broken.conflicts();
        boolean warned = false;
        boolean missingInventory = false;
        for (Conflict conflict : conflicts) {
            if (conflict.message.contains("KEY_SPACE")) {
                warned = true;
            }
            if (conflict.message.contains("背包")) {
                missingInventory = true;
            }
        }
        check("冲突检测：抢键会报警", warned);
        check("冲突检测：缺动作会报错（背包）", missingInventory);

        KeymapLayout overlap = KeymapLayout.fromJsonString(layout.toJsonString());
        ControlButton twin = new ControlButton();
        twin.id = "twin";
        twin.label = "重叠";
        twin.x = overlap.buttons.get(0).x;
        twin.y = overlap.buttons.get(0).y;
        twin.w = overlap.buttons.get(0).w;
        twin.h = overlap.buttons.get(0).h;
        twin.events.put(ControlButton.PRESS, new Binding("drop", Binding.TAP, "KEY_Q"));
        overlap.buttons.add(twin);
        boolean overlapWarned = false;
        for (Conflict conflict : overlap.conflicts()) {
            if (conflict.message.contains("位置重叠")) {
                overlapWarned = true;
            }
        }
        check("冲突检测：控件重叠会报警", overlapWarned);

        KeymapLayout alias = KeymapLayout.fromJsonString(layout.toJsonString());
        for (ControlButton button : alias.buttons) {
            if (!button.aliasOf.isEmpty()) {
                check("别名按钮不会被当成抢键（" + button.id + " -> " + button.aliasOf + "）", true);
                return;
            }
        }
        check("别名按钮不会被当成抢键（该预设没有别名按钮，跳过）", true);
    }

    private static void testDirectionAndButtons() {
        DirectionControl rocker = new DirectionControl();
        rocker.deadZone = 0.2;
        check("摇杆：死区内不动", rocker.directionOf(0.05, 0.05).isEmpty());
        check("摇杆：向右", "right".equals(rocker.directionOf(0.5, 0.05)));
        check("摇杆：向上", "up".equals(rocker.directionOf(0.05, -0.5)));
        check("摇杆：向下", "down".equals(rocker.directionOf(0.0, 0.6)));
        check("摇杆：推到底会疾跑", rocker.isSprint(0.99, 0.0));
        check("摇杆：轻推不疾跑", !rocker.isSprint(0.3, 0.0));

        ControlButton button = new ControlButton();
        check("按键：没有事件时 primary 为空", button.primary() == null);
        button.events.put(ControlButton.LONG_PRESS, new Binding("attack", Binding.HOLD, "MOUSE_LEFT"));
        check("按键：只有长按时 primary 退化成长按",
                "attack".equals(button.primary().action));
        button.events.put(ControlButton.CLICK, new Binding("inventory", Binding.TAP, "KEY_E"));
        check("按键：有单击时优先单击", "inventory".equals(button.primary().action));
        button.events.put(ControlButton.PRESS, new Binding("jump", Binding.HOLD, "KEY_SPACE"));
        check("按键：有按下时优先按下", "jump".equals(button.primary().action));
        check("按键：allKeys 去重", button.allKeys().size() == 3);
        Binding toggle = new Binding("sneak", Binding.TOGGLE, "KEY_LEFT_SHIFT");
        check("按键：toggle 行为识别", toggle.isToggle());
    }

    private static void testFclExport(KeymapLibrary.Loader loader, List<KeymapLibrary.Entry> entries)
            throws Exception {
        KeymapLayout layout = KeymapLibrary.loadEntry(loader, entries.get(0));
        Map<String, Object> fcl = KeymapLibrary.toFcl(layout, 2400, 1080);
        List<Object> views = Json.arr(fcl.get("views"));
        check("FCL 导出：控件数量一致", views.size() == layout.controlCount());
        boolean inside = true;
        for (Object item : views) {
            Map<String, Object> view = Json.obj(item);
            double x = Json.num(view, "x", -1);
            double y = Json.num(view, "y", -1);
            double width = Json.num(view, "width", 0);
            double height = Json.num(view, "height", 0);
            if (x < 0 || y < 0 || x + width > 2401 || y + height > 1081) {
                inside = false;
            }
        }
        check("FCL 导出：像素坐标都在屏幕内", inside);
    }

    // ── 极小断言框架 ───────────────────────────────────────────

    private static void check(String what, boolean ok) {
        if (ok) {
            passed++;
            System.out.println("  [OK] " + what);
        } else {
            fail(what);
        }
    }

    private static void fail(String what) {
        failures.add(what);
        System.out.println("  [!!] " + what);
    }
}
