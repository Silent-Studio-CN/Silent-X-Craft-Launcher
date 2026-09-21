/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include "keymap_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GLFW 键码 -> 我们的键名(fcl.py 的 GLFW_KEYS,一条不多一条不少)。 */
typedef struct glfw_entry {
    int code;
    const char *name;
} glfw_entry;

static const glfw_entry k_glfw[] = {
    { 32, "KEY_SPACE" }, { 39, "KEY_APOSTROPHE" }, { 44, "KEY_COMMA" }, { 45, "KEY_MINUS" },
    { 46, "KEY_PERIOD" }, { 47, "KEY_SLASH" }, { 48, "KEY_0" }, { 49, "KEY_1" }, { 50, "KEY_2" },
    { 51, "KEY_3" }, { 52, "KEY_4" }, { 53, "KEY_5" }, { 54, "KEY_6" }, { 55, "KEY_7" },
    { 56, "KEY_8" }, { 57, "KEY_9" }, { 59, "KEY_SEMICOLON" }, { 61, "KEY_EQUAL" },
    { 65, "KEY_A" }, { 66, "KEY_B" }, { 67, "KEY_C" }, { 68, "KEY_D" }, { 69, "KEY_E" },
    { 70, "KEY_F" }, { 71, "KEY_G" }, { 72, "KEY_H" }, { 73, "KEY_I" }, { 74, "KEY_J" },
    { 75, "KEY_K" }, { 76, "KEY_L" }, { 77, "KEY_M" }, { 78, "KEY_N" }, { 79, "KEY_O" },
    { 80, "KEY_P" }, { 81, "KEY_Q" }, { 82, "KEY_R" }, { 83, "KEY_S" }, { 84, "KEY_T" },
    { 85, "KEY_U" }, { 86, "KEY_V" }, { 87, "KEY_W" }, { 88, "KEY_X" }, { 89, "KEY_Y" },
    { 90, "KEY_Z" }, { 96, "KEY_GRAVE_ACCENT" }, { 256, "KEY_ESCAPE" }, { 257, "KEY_ENTER" },
    { 258, "KEY_TAB" }, { 259, "KEY_BACKSPACE" }, { 260, "KEY_INSERT" }, { 261, "KEY_DELETE" },
    { 262, "KEY_RIGHT" }, { 263, "KEY_LEFT" }, { 264, "KEY_DOWN" }, { 265, "KEY_UP" },
    { 266, "KEY_PAGE_UP" }, { 267, "KEY_PAGE_DOWN" }, { 268, "KEY_HOME" }, { 269, "KEY_END" },
    { 280, "KEY_CAPS_LOCK" }, { 290, "KEY_F1" }, { 291, "KEY_F2" }, { 292, "KEY_F3" },
    { 293, "KEY_F4" }, { 294, "KEY_F5" }, { 295, "KEY_F6" }, { 296, "KEY_F7" }, { 297, "KEY_F8" },
    { 298, "KEY_F9" }, { 299, "KEY_F10" }, { 300, "KEY_F11" }, { 301, "KEY_F12" },
    { 340, "KEY_LEFT_SHIFT" }, { 341, "KEY_LEFT_CONTROL" }, { 342, "KEY_LEFT_ALT" },
    { 343, "KEY_LEFT_SUPER" }, { 344, "KEY_RIGHT_SHIFT" }, { 345, "KEY_RIGHT_CONTROL" },
    { 346, "KEY_RIGHT_ALT" }, { 347, "KEY_RIGHT_SUPER" },
    { -1, "MOUSE_LEFT" }, { -2, "MOUSE_RIGHT" }, { -3, "MOUSE_MIDDLE" },
};

typedef struct text_key_entry {
    const char *text;   /* 小写 */
    const char *key;
} text_key_entry;

static const text_key_entry k_text_keys[] = {
    { "space", "KEY_SPACE" }, { "空格", "KEY_SPACE" }, { "shift", "KEY_LEFT_SHIFT" },
    { "潜行", "KEY_LEFT_SHIFT" }, { "ctrl", "KEY_LEFT_CONTROL" }, { "control", "KEY_LEFT_CONTROL" },
    { "alt", "KEY_LEFT_ALT" }, { "enter", "KEY_ENTER" }, { "tab", "KEY_TAB" },
    { "esc", "KEY_ESCAPE" }, { "escape", "KEY_ESCAPE" }, { "left", "MOUSE_LEFT" },
    { "右键", "MOUSE_RIGHT" }, { "right", "MOUSE_RIGHT" }, { "左键", "MOUSE_LEFT" },
    { "e", "KEY_E" }, { "q", "KEY_Q" }, { "w", "KEY_W" }, { "a", "KEY_A" }, { "s", "KEY_S" },
    { "d", "KEY_D" }, { "f", "KEY_F" }, { "t", "KEY_T" }, { "f5", "KEY_F5" },
};

typedef struct action_entry {
    const char *key;
    const char *action;
} action_entry;

static const action_entry k_actions[] = {
    { "KEY_SPACE", "jump" }, { "KEY_LEFT_SHIFT", "sneak" }, { "KEY_E", "inventory" },
    { "KEY_Q", "drop" }, { "KEY_F", "swap_offhand" }, { "KEY_F5", "perspective" },
    { "KEY_T", "chat" }, { "KEY_LEFT_CONTROL", "sprint" }, { "MOUSE_LEFT", "attack" },
    { "MOUSE_RIGHT", "use" },
    { "KEY_1", "hotbar_1" }, { "KEY_2", "hotbar_2" }, { "KEY_3", "hotbar_3" }, { "KEY_4", "hotbar_4" },
    { "KEY_5", "hotbar_5" }, { "KEY_6", "hotbar_6" }, { "KEY_7", "hotbar_7" }, { "KEY_8", "hotbar_8" },
    { "KEY_9", "hotbar_9" },
};

const char *sxcl_keymap_action_for_key(const char *key)
{
    if (!key) {
        return "";
    }
    for (size_t i = 0; i < sizeof k_actions / sizeof k_actions[0]; ++i) {
        if (strcmp(k_actions[i].key, key) == 0) {
            return k_actions[i].action;
        }
    }
    return "";
}

const char *sxcl_keymap_key_from_glfw(int code, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return "";
    }
    for (size_t i = 0; i < sizeof k_glfw / sizeof k_glfw[0]; ++i) {
        if (k_glfw[i].code == code) {
            sxcl_kp_copy(out, out_len, k_glfw[i].name);
            return out;
        }
    }
    (void)snprintf(out, out_len, "KEY_%d", code);
    return out;
}

static int is_ascii_digits(const char *text)
{
    if (!text || !*text) {
        return 0;
    }
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    return 1;
}

const char *sxcl_keymap_key_from_text(const char *raw, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return "";
    }
    out[0] = '\0';
    if (!raw) {
        return out;
    }
    char text[128];
    sxcl_kp_copy(text, sizeof text, raw);
    /* 去首尾空白 */
    char *begin = text;
    while (*begin == ' ' || *begin == '\t' || *begin == '\n' || *begin == '\r') {
        ++begin;
    }
    size_t len = strlen(begin);
    while (len > 0 && (begin[len - 1] == ' ' || begin[len - 1] == '\t' || begin[len - 1] == '\n' ||
                       begin[len - 1] == '\r')) {
        begin[--len] = '\0';
    }
    if (len == 0) {
        return out;
    }
    /* 已经是我们的写法:原样大写 */
    if (strncmp(begin, "KEY_", 4) == 0 || strncmp(begin, "MOUSE_", 6) == 0 ||
        strncmp(begin, "key_", 4) == 0 || strncmp(begin, "mouse_", 6) == 0) {
        sxcl_kp_copy(out, out_len, begin);
        for (char *p = out; *p; ++p) {
            if (*p >= 'a' && *p <= 'z') {
                *p = (char)(*p - 'a' + 'A');
            }
        }
        return out;
    }
    if (is_ascii_digits(begin)) {
        return sxcl_keymap_key_from_glfw(atoi(begin), out, out_len);
    }
    char lower[128];
    sxcl_kp_copy(lower, sizeof lower, begin);
    sxcl_kp_lower(lower);
    for (size_t i = 0; i < sizeof k_text_keys / sizeof k_text_keys[0]; ++i) {
        if (strcmp(k_text_keys[i].text, lower) == 0) {
            sxcl_kp_copy(out, out_len, k_text_keys[i].key);
            return out;
        }
    }
    /* 单字母 -> KEY_X;其余一律 KEY_<大写>(与 fcl.py 的最后一行一致) */
    sxcl_kp_copy(out, out_len, begin);
    for (char *p = out; *p; ++p) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - 'a' + 'A');
        }
    }
    char prefixed[160];
    (void)snprintf(prefixed, sizeof prefixed, "KEY_%s", out);
    sxcl_kp_copy(out, out_len, prefixed);
    return out;
}

/* ── 导入 ── */

static double json_to_number(const sxcl_json_value *value, double def)
{
    if (!value) {
        return def;
    }
    if (sxcl_json_type_of(value) == SXCL_JSON_NUMBER) {
        return sxcl_json_number(value);
    }
    if (sxcl_json_type_of(value) == SXCL_JSON_STRING) {
        const char *text = sxcl_json_string(value);
        char *end = NULL;
        const double parsed = strtod(text ? text : "", &end);
        if (end && end != text) {
            return parsed;
        }
    }
    return def;
}

/* _pick(data, *names, default):第一个存在且不是 null 的成员。 */
static const sxcl_json_value *pick_value(const sxcl_json_value *object, const char *const *names,
                                         size_t count)
{
    if (!object) {
        return NULL;
    }
    for (size_t i = 0; i < count; ++i) {
        const sxcl_json_value *value = sxcl_json_get(object, names[i]);
        if (value && sxcl_json_type_of(value) != SXCL_JSON_NULL) {
            return value;
        }
    }
    return NULL;
}

static const char *pick_string(const sxcl_json_value *object, const char *const *names, size_t count,
                               const char *def)
{
    const sxcl_json_value *value = pick_value(object, names, count);
    if (!value) {
        return def;
    }
    if (sxcl_json_type_of(value) == SXCL_JSON_STRING) {
        return sxcl_json_string(value);
    }
    return def;
}

/* fcl.py 里到处是 str(_pick(...) or 后面那个默认值) —— 字段**存在但是空串**时也要回落。
 * (pick_string 只管"有没有这个字段",不管空不空。) */
static const char *pick_string_or(const sxcl_json_value *object, const char *const *names, size_t count,
                                 const char *fallback)
{
    const char *text = pick_string(object, names, count, fallback);
    return (text && *text) ? text : fallback;
}

/* _keys_of:把某个字段里的按键统一成 "KEY_*"/"MOUSE_*"。 */
static size_t keys_of(const sxcl_json_value *item, const char *const *names, size_t count,
                      char keys[][SXCL_KEYMAP_KEY_MAX], size_t cap)
{
    const sxcl_json_value *value = pick_value(item, names, count);
    if (!value) {
        return 0;
    }
    size_t total = 0;
    if (sxcl_json_type_of(value) == SXCL_JSON_ARRAY) {
        const size_t items = sxcl_json_size(value);
        for (size_t i = 0; i < items && total < cap; ++i) {
            const sxcl_json_value *entry = sxcl_json_at(value, i);
            if (!entry) {
                continue;
            }
            if (sxcl_json_type_of(entry) == SXCL_JSON_NUMBER) {
                sxcl_keymap_key_from_glfw((int)sxcl_json_number(entry), keys[total],
                                          SXCL_KEYMAP_KEY_MAX);
            } else if (sxcl_json_type_of(entry) == SXCL_JSON_STRING) {
                sxcl_keymap_key_from_text(sxcl_json_string(entry), keys[total], SXCL_KEYMAP_KEY_MAX);
            } else {
                continue;
            }
            if (keys[total][0]) {
                ++total;
            }
        }
        return total;
    }
    if (sxcl_json_type_of(value) == SXCL_JSON_NUMBER) {
        sxcl_keymap_key_from_glfw((int)sxcl_json_number(value), keys[0], SXCL_KEYMAP_KEY_MAX);
    } else if (sxcl_json_type_of(value) == SXCL_JSON_STRING) {
        sxcl_keymap_key_from_text(sxcl_json_string(value), keys[0], SXCL_KEYMAP_KEY_MAX);
    }
    return keys[0][0] ? 1 : 0;
}

static void rect_of(const sxcl_json_value *item, double screen_w, double screen_h,
                    double *x, double *y, double *w, double *h)
{
    static const char *const x_names[] = { "x", "left" };
    static const char *const y_names[] = { "y", "top" };
    static const char *const w_names[] = { "width", "w" };
    static const char *const h_names[] = { "height", "h" };
    double rx = json_to_number(pick_value(item, x_names, 2), 0.0);
    double ry = json_to_number(pick_value(item, y_names, 2), 0.0);
    double rw = json_to_number(pick_value(item, w_names, 2), 0.0);
    double rh = json_to_number(pick_value(item, h_names, 2), 0.0);
    if (rx > 1.0 || ry > 1.0 || rw > 1.0 || rh > 1.0) {
        /* 像素坐标:按屏幕尺寸归一化(fcl.py:_rect) */
        const double sw = (screen_w > 0.0) ? screen_w : 2400.0;
        const double sh = (screen_h > 0.0) ? screen_h : 1080.0;
        rx /= sw;
        rw /= sw;
        ry /= sh;
        rh /= sh;
    }
    if (!(rw > 0.0)) {
        rw = 0.09;
    }
    if (!(rh > 0.0)) {
        rh = 0.14;
    }
    *x = rx;
    *y = ry;
    *w = rw;
    *h = rh;
}

/* FCL 原始数据塞进 meta.fcl_raw(fcl.py 的做法:方便对照排查)。
 * 顶层是数组时 fcl.py 会包一层 {"views": [...]},这里跟着包,免得两边 meta 结构不一样。 */
static int set_fcl_meta(sxcl_keymap_layout *layout, const sxcl_json_value *root)
{
    const int wrapped = (root && sxcl_json_type_of(root) == SXCL_JSON_ARRAY);
    sxcl_kp_buf buf;
    sxcl_kp_buf_init(&buf);
    int rc = sxcl_kp_buf_puts(&buf, "{\n    \"fcl_raw\": ");
    if (rc == SXCL_KEYMAP_OK && wrapped) {
        rc = sxcl_kp_buf_puts(&buf, "{\n      \"views\": ");
    }
    if (rc == SXCL_KEYMAP_OK) {
        rc = sxcl_kp_json_dump(&buf, root, wrapped ? 3 : 2);
    }
    if (rc == SXCL_KEYMAP_OK && wrapped) {
        rc = sxcl_kp_buf_puts(&buf, "\n    }");
    }
    if (rc == SXCL_KEYMAP_OK) {
        rc = sxcl_kp_buf_puts(&buf, "\n  }");
    }
    if (rc != SXCL_KEYMAP_OK) {
        sxcl_kp_buf_free(&buf);
        return rc;
    }
    free(layout->meta_json);
    layout->meta_json = buf.data;
    return SXCL_KEYMAP_OK;
}

int sxcl_keymap_import_fcl(const char *json_text, size_t len, const char *name,
                           double screen_w, double screen_h, sxcl_keymap_layout *out,
                           sxcl_keymap_issues *issues, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!out) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    if (!json_text) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "没有内容");
        }
        return SXCL_KEYMAP_ERR_ARG;
    }
    if (screen_w <= 0.0) {
        screen_w = 2400.0;   /* fcl.py 的默认值 */
    }
    if (screen_h <= 0.0) {
        screen_h = 1080.0;
    }
    char json_err[160];
    json_err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(json_text, len, json_err, sizeof json_err);
    if (!doc) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "%s", json_err[0] ? json_err : "JSON 解析失败");
        }
        return SXCL_KEYMAP_ERR_FORMAT;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const sxcl_json_value *raw_controls = NULL;
    const char *layout_name = NULL;
    const char *screen = "landscape";
    if (root && sxcl_json_type_of(root) == SXCL_JSON_OBJECT) {
        static const char *const list_names[] = { "views", "controls", "buttons", "widgets" };
        raw_controls = pick_value(root, list_names, 4);
        static const char *const name_names[] = { "name", "title" };
        layout_name = pick_string(root, name_names, 2, NULL);
        screen = (screen_h > screen_w) ? "portrait" : "landscape";
    } else if (root && sxcl_json_type_of(root) == SXCL_JSON_ARRAY) {
        raw_controls = root;
        screen = (screen_w >= screen_h) ? "landscape" : "portrait";
    } else {
        sxcl_json_free(doc);
        if (err && err_len) {
            (void)snprintf(err, err_len, "FCL 布局应该是一个对象(带 views)或一个数组");
        }
        return SXCL_KEYMAP_ERR_FORMAT;
    }

    sxcl_keymap_layout_init(out);
    sxcl_kp_copy(out->name, sizeof out->name,
                 (name && *name) ? name : (layout_name && *layout_name) ? layout_name : "从 FCL 导入");
    sxcl_kp_copy(out->screen, sizeof out->screen, screen);
    sxcl_kp_copy(out->description, sizeof out->description,
                 "由 FCL 布局导入（坐标已按屏幕尺寸归一化）");

    const size_t count = (raw_controls && sxcl_json_type_of(raw_controls) == SXCL_JSON_ARRAY)
                             ? sxcl_json_size(raw_controls)
                             : 0;
    for (size_t i = 0; i < count; ++i) {
        const sxcl_json_value *item = sxcl_json_at(raw_controls, i);
        if (!item || sxcl_json_type_of(item) != SXCL_JSON_OBJECT) {
            continue;
        }
        static const char *const kind_names[] = { "type", "viewType", "kind" };
        char kind[64];
        sxcl_kp_copy(kind, sizeof kind, pick_string(item, kind_names, 3, "button"));
        sxcl_kp_lower(kind);
        double x, y, w, h;
        rect_of(item, screen_w, screen_h, &x, &y, &w, &h);
        const int is_direction = (strstr(kind, "direction") != NULL || strstr(kind, "joystick") != NULL ||
                                  strstr(kind, "dpad") != NULL || strstr(kind, "rocker") != NULL);
        static const char *const id_names[] = { "id" };
        static const char *const text_names[] = { "text", "label", "name" };
        /* 方向控件的字段表与按钮不同:fcl.py 的方向只认 text/label(不认 name) */
        static const char *const direction_text_names[] = { "text", "label" };
        if (is_direction) {
            char id[64];
            (void)snprintf(id, sizeof id, "move%d", (int)i);
            sxcl_keymap_direction direction;
            memset(&direction, 0, sizeof direction);
            /* fcl.py: str(_pick(item, "id", default=f"move{index}") or ...) —— 空 id 也回落 */
            sxcl_kp_copy(direction.id, sizeof direction.id, pick_string_or(item, id_names, 1, id));
            sxcl_kp_copy(direction.label, sizeof direction.label,
                         pick_string_or(item, direction_text_names, 2, "移动"));
            direction.x = x;
            direction.y = y;
            direction.w = w;
            direction.h = h;
            /* style 只由**视图类型**决定(fcl.py 不看视图里那个显式 style 字段):
             * 类型串里有 rocker/joystick 就是 rocker,否则 dpad_compact。 */
            sxcl_kp_copy(direction.style, sizeof direction.style,
                         (strstr(kind, "rocker") || strstr(kind, "joystick")) ? "rocker" : "dpad_compact");
            direction.opacity = 0.5;
            direction.dead_zone = 0.18;
            sxcl_kp_copy(direction.group, sizeof direction.group, "left");
            static const char *const move_keys[] = { "KEY_W", "KEY_S", "KEY_A", "KEY_D" };
            sxcl_kp_copy(direction.up, sizeof direction.up, move_keys[0]);
            sxcl_kp_copy(direction.down, sizeof direction.down, move_keys[1]);
            sxcl_kp_copy(direction.left, sizeof direction.left, move_keys[2]);
            sxcl_kp_copy(direction.right, sizeof direction.right, move_keys[3]);
            sxcl_kp_copy(direction.sprint_key, sizeof direction.sprint_key, "KEY_LEFT_SHIFT");
            if (sxcl_kp_push_direction(out, &direction) != SXCL_KEYMAP_OK) {
                sxcl_json_free(doc);
                sxcl_keymap_layout_free(out);
                return SXCL_KEYMAP_ERR_NOMEM;
            }
            continue;
        }

        sxcl_keymap_button button;
        memset(&button, 0, sizeof button);
        char fallback_id[32];
        (void)snprintf(fallback_id, sizeof fallback_id, "fcl%d", (int)i);
        /* fcl.py: id 空 -> fcl<序号>;label 空 -> 空串(但**没有** label 字段时用 键<序号>) */
        sxcl_kp_copy(button.id, sizeof button.id, pick_string_or(item, id_names, 1, fallback_id));
        char fallback_label[32];
        (void)snprintf(fallback_label, sizeof fallback_label, "键%d", (int)i + 1);
        sxcl_kp_copy(button.label, sizeof button.label,
                     pick_string(item, text_names, 3, fallback_label));
        button.x = x;
        button.y = y;
        button.w = w;
        button.h = h;
        sxcl_kp_copy(button.shape, sizeof button.shape, strstr(kind, "square") ? "square" : "round");
        static const char *const alpha_names[] = { "alpha", "opacity" };
        const sxcl_json_value *alpha = pick_value(item, alpha_names, 2);
        button.opacity = (alpha ? json_to_number(alpha, 0.55) : 0.55);
        if (button.opacity == 0.0) {
            button.opacity = 0.55;   /* fcl.py 的 float(_pick(...) or 0.55):写了 0 也当没写 */
        }
        /* fcl.py 导入时 group 用 ControlButton 的默认值(空串),不是预设里那个 "right" */
        sxcl_kp_copy(button.group, sizeof button.group, "");

        static const char *const press_names[] = { "keycodes", "keys", "codes", "key" };
        static const char *const click_names[] = { "clickKeycodes", "clickKeys" };
        static const char *const long_names[] = { "longPressKeycodes", "longPressKeys" };
        static const char *const double_names[] = { "doubleClickKeycodes", "doubleClickKeys" };
        char event_keys[SXCL_KEYMAP_EVENT_COUNT][SXCL_KEYMAP_KEYS_MAX][SXCL_KEYMAP_KEY_MAX];
        memset(event_keys, 0, sizeof event_keys);
        size_t event_counts[SXCL_KEYMAP_EVENT_COUNT];
        event_counts[0] = keys_of(item, press_names, 4, event_keys[0], SXCL_KEYMAP_KEYS_MAX);
        event_counts[1] = keys_of(item, long_names, 2, event_keys[1], SXCL_KEYMAP_KEYS_MAX);
        event_counts[2] = keys_of(item, click_names, 2, event_keys[2], SXCL_KEYMAP_KEYS_MAX);
        event_counts[3] = keys_of(item, double_names, 2, event_keys[3], SXCL_KEYMAP_KEYS_MAX);
        if (sxcl_kp_push_button(out, &button) != SXCL_KEYMAP_OK) {
            sxcl_json_free(doc);
            sxcl_keymap_layout_free(out);
            return SXCL_KEYMAP_ERR_NOMEM;
        }
        for (size_t e = 0; e < SXCL_KEYMAP_EVENT_COUNT; ++e) {
            if (event_counts[e] == 0) {
                continue;
            }
            const char *action = "";
            for (size_t k = 0; k < event_counts[e]; ++k) {
                const char *found = sxcl_keymap_action_for_key(event_keys[e][k]);
                if (found && *found) {
                    action = found;
                    break;
                }
            }
            const char *behavior = (e == (size_t)SXCL_KEYMAP_EVENT_DOUBLE_CLICK) ? "toggle" : "hold";
            /* sxcl_keymap_bind 收的是"指针数组",这里的二维数组要转一次 */
            const char *pointers[SXCL_KEYMAP_KEYS_MAX];
            for (size_t k = 0; k < event_counts[e]; ++k) {
                pointers[k] = event_keys[e][k];
            }
            if (sxcl_keymap_bind(out, button.id, (sxcl_keymap_event)e, action, behavior, pointers,
                                 event_counts[e]) != SXCL_KEYMAP_OK) {
                sxcl_json_free(doc);
                sxcl_keymap_layout_free(out);
                return SXCL_KEYMAP_ERR_ARG;
            }
        }
    }

    const int meta_rc = set_fcl_meta(out, root);
    (void)issues;   /* 导入成功是"好消息",不往校验/冲突列表里塞(界面自己弹 InfoBar) */
    sxcl_json_free(doc);
    if (meta_rc != SXCL_KEYMAP_OK) {
        sxcl_keymap_layout_free(out);
        return meta_rc;
    }
    return SXCL_KEYMAP_OK;
}

/* ── 导出 ── */

static long round_pos(double value)
{
    return (long)(value + 0.5);   /* 像素坐标一律非负 */
}

int sxcl_keymap_to_fcl(const sxcl_keymap_layout *layout, double screen_w, double screen_h,
                       char **out_text, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!layout || !out_text) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    *out_text = NULL;
    if (screen_w <= 0.0) {
        screen_w = 2400.0;
    }
    if (screen_h <= 0.0) {
        screen_h = 1080.0;
    }
    sxcl_kp_buf buf;
    sxcl_kp_buf_init(&buf);
    int rc = sxcl_kp_buf_puts(&buf, "{");
    sxcl_kp_buf_indent(&buf, 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"name\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(&buf, layout->name) : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, ",") : rc;
    sxcl_kp_buf_indent(&buf, 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"views\": [") : rc;

    const size_t total = sxcl_keymap_control_count(layout);
    for (size_t i = 0; i < total && rc == SXCL_KEYMAP_OK; ++i) {
        if (i) {
            rc = sxcl_kp_buf_puts(&buf, ",");
        }
        sxcl_kp_buf_indent(&buf, 2);
        if (i < layout->direction_count) {
            const sxcl_keymap_direction *d = &layout->directions[i];
            rc = sxcl_kp_buf_puts(&buf, "{");
            sxcl_kp_buf_indent(&buf, 3);
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"type\": \"direction\",") : rc;
            sxcl_kp_buf_indent(&buf, 3);
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"style\": ") : rc;
            rc = (rc == SXCL_KEYMAP_OK)
                     ? sxcl_kp_buf_json_string(&buf, (strcmp(d->style, "rocker") == 0) ? "rocker" : "dpad")
                     : rc;
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, ",") : rc;
            sxcl_kp_buf_indent(&buf, 3);
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"text\": ") : rc;
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(&buf, d->label) : rc;
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, ",") : rc;
            sxcl_kp_buf_indent(&buf, 3);
            rc = (rc == SXCL_KEYMAP_OK)
                     ? sxcl_kp_buf_printf(&buf, "\"x\": %ld, \"y\": %ld, \"width\": %ld, \"height\": %ld,",
                                          round_pos(d->x * screen_w), round_pos(d->y * screen_h),
                                          round_pos(d->w * screen_w), round_pos(d->h * screen_h))
                     : rc;
            sxcl_kp_buf_indent(&buf, 3);
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"keycodes\": [") : rc;
            for (size_t k = 0; k < 4 && rc == SXCL_KEYMAP_OK; ++k) {
                if (k) {
                    rc = sxcl_kp_buf_puts(&buf, ", ");
                }
                rc = (rc == SXCL_KEYMAP_OK)
                         ? sxcl_kp_buf_json_string(&buf, sxcl_kp_direction_key_slot_const(
                                                             d, sxcl_kp_direction_key_name(k)))
                         : rc;
            }
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "]") : rc;
            sxcl_kp_buf_indent(&buf, 2);
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "}") : rc;
            continue;
        }

        const sxcl_keymap_button *b = &layout->buttons[i - layout->direction_count];
        rc = sxcl_kp_buf_puts(&buf, "{");
        sxcl_kp_buf_indent(&buf, 3);
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"type\": \"button\",") : rc;
        sxcl_kp_buf_indent(&buf, 3);
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"text\": ") : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(&buf, b->label) : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, ",") : rc;
        sxcl_kp_buf_indent(&buf, 3);
        rc = (rc == SXCL_KEYMAP_OK)
                 ? sxcl_kp_buf_printf(&buf, "\"x\": %ld, \"y\": %ld, \"width\": %ld, \"height\": %ld,",
                                      round_pos(b->x * screen_w), round_pos(b->y * screen_h),
                                      round_pos(b->w * screen_w), round_pos(b->h * screen_h))
                 : rc;
        sxcl_kp_buf_indent(&buf, 3);
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"alpha\": ") : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_number(&buf, b->opacity) : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, ",") : rc;
        static const struct {
            const char *field;
            sxcl_keymap_event event;
        } event_fields[] = {
            { "keycodes", SXCL_KEYMAP_EVENT_PRESS },
            { "longPressKeycodes", SXCL_KEYMAP_EVENT_LONG_PRESS },
            { "doubleClickKeycodes", SXCL_KEYMAP_EVENT_DOUBLE_CLICK },
        };
        for (size_t f = 0; f < sizeof event_fields / sizeof event_fields[0] && rc == SXCL_KEYMAP_OK; ++f) {
            sxcl_kp_buf_indent(&buf, 3);
            rc = sxcl_kp_buf_puts(&buf, "\"");
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, event_fields[f].field) : rc;
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\": [") : rc;
            const sxcl_keymap_binding *binding = sxcl_keymap_button_event(b, event_fields[f].event);
            for (size_t k = 0; binding && k < binding->key_count && rc == SXCL_KEYMAP_OK; ++k) {
                if (k) {
                    rc = sxcl_kp_buf_puts(&buf, ", ");
                }
                rc = sxcl_kp_buf_json_string(&buf, binding->keys[k]);
            }
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "]") : rc;
            if (f + 1 < sizeof event_fields / sizeof event_fields[0]) {
                rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, ",") : rc;
            }
        }
        sxcl_kp_buf_indent(&buf, 2);
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "}") : rc;
    }
    if (total) {
        sxcl_kp_buf_indent(&buf, 1);
    }
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "],") : rc;
    sxcl_kp_buf_indent(&buf, 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_printf(&buf, "\"screenWidth\": %ld,", round_pos(screen_w)) : rc;
    sxcl_kp_buf_indent(&buf, 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_printf(&buf, "\"screenHeight\": %ld", round_pos(screen_h)) : rc;
    sxcl_kp_buf_indent(&buf, 0);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "}") : rc;

    if (rc != SXCL_KEYMAP_OK) {
        sxcl_kp_buf_free(&buf);
        if (err && err_len) {
            (void)snprintf(err, err_len, "导出 FCL 失败(内存不足?)");
        }
        return rc;
    }
    *out_text = buf.data;
    return SXCL_KEYMAP_OK;
}
