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
 * 极简 JSON 解析/生成。
 *
 * 为什么不用 org.json：android.jar 里的 org.json 在电脑 JVM 上是 "Stub!" 直接抛异常，
 * 我们在电脑上跑单元测试就废了；自带一个 200 行的解析器，桌面 JVM 和安卓都能用，
 * 也省掉一个依赖（依赖越少，跨平台打包越省事）。
 */
public final class Json {

    private final String text;
    private int pos;

    private Json(String text) {
        this.text = text;
    }

    public static Object parse(String text) {
        Json parser = new Json(text == null ? "" : text);
        parser.skipWhitespace();
        Object value = parser.readValue();
        return value;
    }

    @SuppressWarnings("unchecked")
    public static Map<String, Object> parseObject(String text) {
        Object value = parse(text);
        return value instanceof Map ? (Map<String, Object>) value : new LinkedHashMap<String, Object>();
    }

    // ── 取值辅助（容错，缺字段不炸） ──────────────────────────────

    @SuppressWarnings("unchecked")
    public static Map<String, Object> obj(Object value) {
        return value instanceof Map ? (Map<String, Object>) value : new LinkedHashMap<String, Object>();
    }

    @SuppressWarnings("unchecked")
    public static List<Object> arr(Object value) {
        return value instanceof List ? (List<Object>) value : new ArrayList<Object>();
    }

    public static String str(Map<String, Object> map, String key, String fallback) {
        Object value = map == null ? null : map.get(key);
        return value == null ? fallback : String.valueOf(value);
    }

    public static double num(Map<String, Object> map, String key, double fallback) {
        Object value = map == null ? null : map.get(key);
        if (value instanceof Number) {
            return ((Number) value).doubleValue();
        }
        if (value instanceof String) {
            try {
                return Double.parseDouble((String) value);
            } catch (NumberFormatException ignored) {
                return fallback;
            }
        }
        return fallback;
    }

    public static boolean bool(Map<String, Object> map, String key, boolean fallback) {
        Object value = map == null ? null : map.get(key);
        if (value instanceof Boolean) {
            return (Boolean) value;
        }
        return value == null ? fallback : Boolean.parseBoolean(String.valueOf(value));
    }

    /** 支持 "a.b.c" 点号路径取值；也支持 "a[0].b"。 */
    public static Object get(Object root, String path) {
        Object current = root;
        for (String part : path.split("\\.")) {
            int bracket = part.indexOf('[');
            String name = bracket < 0 ? part : part.substring(0, bracket);
            if (!name.isEmpty()) {
                current = obj(current).get(name);
            }
            while (bracket >= 0 && current instanceof List) {
                int end = part.indexOf(']', bracket);
                String index = part.substring(bracket + 1, end < 0 ? part.length() : end);
                try {
                    current = ((List<?>) current).get(Integer.parseInt(index.trim()));
                } catch (RuntimeException ignored) {
                    return null;
                }
                bracket = part.indexOf('[', end + 1);
            }
        }
        return current;
    }

    // ── 生成 ────────────────────────────────────────────────────

    public static String write(Object value) {
        StringBuilder out = new StringBuilder();
        writeValue(out, value, 0);
        return out.toString();
    }

    private static void writeValue(StringBuilder out, Object value, int depth) {
        if (value == null) {
            out.append("null");
        } else if (value instanceof String) {
            writeString(out, (String) value);
        } else if (value instanceof Boolean || value instanceof Number) {
            out.append(String.valueOf(value));
        } else if (value instanceof Map) {
            Map<?, ?> map = (Map<?, ?>) value;
            if (map.isEmpty()) {
                out.append("{}");
                return;
            }
            out.append("{\n");
            int index = 0;
            for (Map.Entry<?, ?> entry : map.entrySet()) {
                indent(out, depth + 1);
                writeString(out, String.valueOf(entry.getKey()));
                out.append(": ");
                writeValue(out, entry.getValue(), depth + 1);
                if (++index < map.size()) {
                    out.append(',');
                }
                out.append('\n');
            }
            indent(out, depth);
            out.append('}');
        } else if (value instanceof List) {
            List<?> list = (List<?>) value;
            if (list.isEmpty()) {
                out.append("[]");
                return;
            }
            out.append("[\n");
            for (int i = 0; i < list.size(); i++) {
                indent(out, depth + 1);
                writeValue(out, list.get(i), depth + 1);
                if (i + 1 < list.size()) {
                    out.append(',');
                }
                out.append('\n');
            }
            indent(out, depth);
            out.append(']');
        } else {
            writeString(out, String.valueOf(value));
        }
    }

    private static void indent(StringBuilder out, int depth) {
        for (int i = 0; i < depth * 2; i++) {
            out.append(' ');
        }
    }

    private static void writeString(StringBuilder out, String value) {
        out.append('"');
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            switch (c) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default:
                    if (c < 0x20) {
                        out.append(String.format("\\u%04x", (int) c));
                    } else {
                        out.append(c);
                    }
            }
        }
        out.append('"');
    }

    // ── 解析实现 ────────────────────────────────────────────────

    private Object readValue() {
        skipWhitespace();
        if (pos >= text.length()) {
            return null;
        }
        char c = text.charAt(pos);
        switch (c) {
            case '{': return readObject();
            case '[': return readArray();
            case '"': return readString();
            case 't': expect("true"); return Boolean.TRUE;
            case 'f': expect("false"); return Boolean.FALSE;
            case 'n': expect("null"); return null;
            default: return readNumber();
        }
    }

    private Map<String, Object> readObject() {
        Map<String, Object> map = new LinkedHashMap<String, Object>();
        pos++;                       // '{'
        skipWhitespace();
        if (pos < text.length() && text.charAt(pos) == '}') {
            pos++;
            return map;
        }
        while (pos < text.length()) {
            skipWhitespace();
            String key = readString();
            skipWhitespace();
            if (pos < text.length() && text.charAt(pos) == ':') {
                pos++;
            }
            map.put(key, readValue());
            skipWhitespace();
            if (pos < text.length() && text.charAt(pos) == ',') {
                pos++;
                continue;
            }
            if (pos < text.length() && text.charAt(pos) == '}') {
                pos++;
            }
            break;
        }
        return map;
    }

    private List<Object> readArray() {
        List<Object> list = new ArrayList<Object>();
        pos++;                       // '['
        skipWhitespace();
        if (pos < text.length() && text.charAt(pos) == ']') {
            pos++;
            return list;
        }
        while (pos < text.length()) {
            list.add(readValue());
            skipWhitespace();
            if (pos < text.length() && text.charAt(pos) == ',') {
                pos++;
                continue;
            }
            if (pos < text.length() && text.charAt(pos) == ']') {
                pos++;
            }
            break;
        }
        return list;
    }

    private String readString() {
        StringBuilder out = new StringBuilder();
        skipWhitespace();
        if (pos < text.length() && text.charAt(pos) == '"') {
            pos++;
        }
        while (pos < text.length()) {
            char c = text.charAt(pos++);
            if (c == '"') {
                break;
            }
            if (c != '\\') {
                out.append(c);
                continue;
            }
            char esc = text.charAt(pos++);
            switch (esc) {
                case 'n': out.append('\n'); break;
                case 'r': out.append('\r'); break;
                case 't': out.append('\t'); break;
                case 'b': out.append('\b'); break;
                case 'f': out.append('\f'); break;
                case 'u':
                    out.append((char) Integer.parseInt(text.substring(pos, pos + 4), 16));
                    pos += 4;
                    break;
                default: out.append(esc);
            }
        }
        return out.toString();
    }

    private Object readNumber() {
        int start = pos;
        while (pos < text.length() && "+-0123456789.eE".indexOf(text.charAt(pos)) >= 0) {
            pos++;
        }
        String raw = text.substring(start, pos);
        try {
            return Double.valueOf(raw);
        } catch (NumberFormatException ignored) {
            return Double.valueOf(0);
        }
    }

    private void expect(String literal) {
        if (text.startsWith(literal, pos)) {
            pos += literal.length();
        } else {
            pos = text.length();
        }
    }

    private void skipWhitespace() {
        while (pos < text.length() && Character.isWhitespace(text.charAt(pos))) {
            pos++;
        }
    }
}
