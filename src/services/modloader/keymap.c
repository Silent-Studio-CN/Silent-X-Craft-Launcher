/* SXCL-C 键位映射核心 —— sxcl.keymap.v1 的模型/解析/序列化/校验/冲突/搜索。
 *
 * 与安卓端 Java 实现的对应关系(逐函数):
 *   sxcl_keymap_parse          <- KeymapLayout.fromJson / ControlButton.fromJson / DirectionControl.fromJson
 *   sxcl_keymap_to_json        <- KeymapLayout.toJson(字段顺序也照着来)
 *   sxcl_keymap_validate       <- KeymapLayout.validate()
 *   sxcl_keymap_conflicts      <- KeymapLayout.conflicts()
 *   sxcl_keymap_find           <- KeymapLayout.find()
 *   sxcl_keymap_action_owners  <- KeymapLayout.actionIndex()
 * 与 Python 版(model.py)的差异:Python 会把坐标/透明度**钳制**到合法区间,Java 不钳制。
 * 这里取"钳制 + 把钳制过的项报出来"(非法项要能报出来,而不是悄悄改),
 * 这样安卓端拿到我们保存的文件也不会画出屏幕外的东西。
 * 内置预设见 keymap_presets.c(presets.py),FCL 互转见 keymap_fcl.c(fcl.py)。
 */
#define _CRT_SECURE_NO_WARNINGS 1  /* 用了几处 C 串函数,MSVC 默认标弃用(工程惯例,不压 pragma) */

#include "keymap_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"

/* 一份布局 JSON 的大小上限(9 套资产里最大 ~13KB;给到 8MB 纯属防呆) */
#define SXCL_KP_FILE_MAX (8u * 1024u * 1024u)

/* ── 内部小工具 ── */

int sxcl_kp_copy(char *dst, size_t cap, const char *src)
{
    const size_t len = src ? strlen(src) : 0;
    if (!dst || cap == 0) {
        return -1;
    }
    if (len + 1 > cap) {
        memcpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
        return -1;
    }
    memcpy(dst, src, len + 1);
    return 0;
}

int sxcl_kp_copy_range(char *dst, size_t cap, const char *begin, size_t len)
{
    if (!dst || cap == 0) {
        return -1;
    }
    if (len + 1 > cap) {
        memcpy(dst, begin, cap - 1);
        dst[cap - 1] = '\0';
        return -1;
    }
    memcpy(dst, begin, len);
    dst[len] = '\0';
    return 0;
}

void sxcl_kp_lower(char *text)
{
    if (!text) {
        return;
    }
    for (char *p = text; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (char)(*p - 'A' + 'a');
        }
    }
}

double sxcl_kp_clamp(double value, double low, double high)
{
    if (!(value == value)) {   /* NaN */
        return low;
    }
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

double sxcl_kp_number(const sxcl_json_value *object, const char *key, double def)
{
    const sxcl_json_value *value = object ? sxcl_json_get(object, key) : NULL;
    if (!value || sxcl_json_type_of(value) != SXCL_JSON_NUMBER) {
        return def;
    }
    return sxcl_json_number(value);
}

const char *sxcl_kp_string(const sxcl_json_value *object, const char *key, const char *def)
{
    const sxcl_json_value *value = object ? sxcl_json_get(object, key) : NULL;
    if (!value || sxcl_json_type_of(value) != SXCL_JSON_STRING) {
        return def;
    }
    return sxcl_json_string(value);
}

void sxcl_kp_issue(sxcl_keymap_issues *issues, int level, const char *control_id,
                   const char *fmt, ...)
{
    if (!issues) {
        return;
    }
    if (issues->count >= SXCL_KEYMAP_ISSUE_MAX) {
        ++issues->dropped;
        return;
    }
    sxcl_keymap_issue *item = &issues->items[issues->count];
    memset(item, 0, sizeof *item);
    item->level = level;
    sxcl_kp_copy(item->control_id, sizeof item->control_id, control_id ? control_id : "");
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(item->message, sizeof item->message, fmt, args);
    va_end(args);
    ++issues->count;
}

/* ── 动态缓冲 ── */

void sxcl_kp_buf_init(sxcl_kp_buf *buf)
{
    if (buf) {
        buf->data = NULL;
        buf->len = 0;
        buf->cap = 0;
    }
}

void sxcl_kp_buf_free(sxcl_kp_buf *buf)
{
    if (!buf) {
        return;
    }
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static int buf_reserve(sxcl_kp_buf *buf, size_t extra)
{
    if (!buf) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    const size_t need = buf->len + extra + 1;
    if (need <= buf->cap) {
        return SXCL_KEYMAP_OK;
    }
    size_t cap = buf->cap ? buf->cap : 256;
    while (cap < need) {
        cap *= 2;
    }
    char *grown = (char *)realloc(buf->data, cap);
    if (!grown) {
        return SXCL_KEYMAP_ERR_NOMEM;
    }
    buf->data = grown;
    buf->cap = cap;
    if (buf->len == 0) {
        buf->data[0] = '\0';
    }
    return SXCL_KEYMAP_OK;
}

int sxcl_kp_buf_putc(sxcl_kp_buf *buf, char ch)
{
    const int rc = buf_reserve(buf, 1);
    if (rc != SXCL_KEYMAP_OK) {
        return rc;
    }
    buf->data[buf->len++] = ch;
    buf->data[buf->len] = '\0';
    return SXCL_KEYMAP_OK;
}

int sxcl_kp_buf_puts(sxcl_kp_buf *buf, const char *text)
{
    if (!text) {
        return SXCL_KEYMAP_OK;
    }
    const size_t len = strlen(text);
    const int rc = buf_reserve(buf, len);
    if (rc != SXCL_KEYMAP_OK) {
        return rc;
    }
    memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return SXCL_KEYMAP_OK;
}

int sxcl_kp_buf_printf(sxcl_kp_buf *buf, const char *fmt, ...)
{
    char stack_buf[256];
    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(stack_buf, sizeof stack_buf, fmt, args);
    va_end(args);
    if (written < 0) {
        return SXCL_KEYMAP_ERR_FORMAT;
    }
    if ((size_t)written < sizeof stack_buf) {
        return sxcl_kp_buf_puts(buf, stack_buf);
    }
    char *heap = (char *)malloc((size_t)written + 1);
    if (!heap) {
        return SXCL_KEYMAP_ERR_NOMEM;
    }
    va_start(args, fmt);
    (void)vsnprintf(heap, (size_t)written + 1, fmt, args);
    va_end(args);
    const int rc = sxcl_kp_buf_puts(buf, heap);
    free(heap);
    return rc;
}

int sxcl_kp_buf_json_string(sxcl_kp_buf *buf, const char *text)
{
    int rc = sxcl_kp_buf_putc(buf, '"');
    if (rc != SXCL_KEYMAP_OK) {
        return rc;
    }
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        const unsigned char ch = *p;
        if (ch == '"' || ch == '\\') {
            rc = sxcl_kp_buf_putc(buf, '\\');
            if (rc == SXCL_KEYMAP_OK) {
                rc = sxcl_kp_buf_putc(buf, (char)ch);
            }
        } else if (ch == '\n') {
            rc = sxcl_kp_buf_puts(buf, "\\n");
        } else if (ch == '\r') {
            rc = sxcl_kp_buf_puts(buf, "\\r");
        } else if (ch == '\t') {
            rc = sxcl_kp_buf_puts(buf, "\\t");
        } else if (ch < 0x20) {
            rc = sxcl_kp_buf_printf(buf, "\\u%04x", (unsigned)ch);
        } else {
            rc = sxcl_kp_buf_putc(buf, (char)ch);
        }
        if (rc != SXCL_KEYMAP_OK) {
            return rc;
        }
    }
    return sxcl_kp_buf_putc(buf, '"');
}

int sxcl_kp_buf_json_number(sxcl_kp_buf *buf, double value)
{
    char text[64];
    /* 坐标/透明度都只有几位小数(资产里最多 4 位),%g 既不丢精度也不会写出 0.550000 */
    (void)snprintf(text, sizeof text, "%.4g", (value == value) ? value : 0.0);
    return sxcl_kp_buf_puts(buf, text);
}

int sxcl_kp_buf_indent(sxcl_kp_buf *buf, int indent)
{
    int rc = sxcl_kp_buf_putc(buf, '\n');
    for (int i = 0; i < indent && rc == SXCL_KEYMAP_OK; ++i) {
        rc = sxcl_kp_buf_puts(buf, "  ");
    }
    return rc;
}

int sxcl_kp_json_dump(sxcl_kp_buf *buf, const sxcl_json_value *value, int indent)
{
    if (!buf) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    if (!value) {
        return sxcl_kp_buf_puts(buf, "null");
    }
    if (indent > 32) {
        return SXCL_KEYMAP_ERR_SPACE;   /* 病态嵌套:别把栈打爆 */
    }
    int rc = SXCL_KEYMAP_OK;
    switch (sxcl_json_type_of(value)) {
    case SXCL_JSON_NULL:
        return sxcl_kp_buf_puts(buf, "null");
    case SXCL_JSON_BOOL:
        return sxcl_kp_buf_puts(buf, sxcl_json_bool(value) ? "true" : "false");
    case SXCL_JSON_NUMBER:
        return sxcl_kp_buf_json_number(buf, sxcl_json_number(value));
    case SXCL_JSON_STRING:
        return sxcl_kp_buf_json_string(buf, sxcl_json_string(value));
    case SXCL_JSON_ARRAY: {
        const size_t count = sxcl_json_size(value);
        if (count == 0) {
            return sxcl_kp_buf_puts(buf, "[]");
        }
        rc = sxcl_kp_buf_putc(buf, '[');
        for (size_t i = 0; i < count && rc == SXCL_KEYMAP_OK; ++i) {
            if (i) {
                rc = sxcl_kp_buf_putc(buf, ',');
            }
            sxcl_kp_buf_indent(buf, indent + 1);
            rc = sxcl_kp_json_dump(buf, sxcl_json_at(value, i), indent + 1);
        }
        sxcl_kp_buf_indent(buf, indent);
        if (rc == SXCL_KEYMAP_OK) {
            rc = sxcl_kp_buf_putc(buf, ']');
        }
        return rc;
    }
    case SXCL_JSON_OBJECT:
    default: {
        const size_t count = sxcl_json_member_count(value);
        if (count == 0) {
            return sxcl_kp_buf_puts(buf, "{}");
        }
        rc = sxcl_kp_buf_putc(buf, '{');
        for (size_t i = 0; i < count && rc == SXCL_KEYMAP_OK; ++i) {
            if (i) {
                rc = sxcl_kp_buf_putc(buf, ',');
            }
            sxcl_kp_buf_indent(buf, indent + 1);
            rc = sxcl_kp_buf_json_string(buf, sxcl_json_member_key(value, i));
            if (rc == SXCL_KEYMAP_OK) {
                rc = sxcl_kp_buf_puts(buf, ": ");
            }
            if (rc == SXCL_KEYMAP_OK) {
                rc = sxcl_kp_json_dump(buf, sxcl_json_member_value(value, i), indent + 1);
            }
        }
        sxcl_kp_buf_indent(buf, indent);
        if (rc == SXCL_KEYMAP_OK) {
            rc = sxcl_kp_buf_putc(buf, '}');
        }
        return rc;
    }
    }
}

/* ── 事件与枚举串 ── */

const char *sxcl_keymap_event_id(sxcl_keymap_event event)
{
    switch (event) {
    case SXCL_KEYMAP_EVENT_PRESS: return "press";
    case SXCL_KEYMAP_EVENT_LONG_PRESS: return "long_press";
    case SXCL_KEYMAP_EVENT_CLICK: return "click";
    case SXCL_KEYMAP_EVENT_DOUBLE_CLICK: return "double_click";
    case SXCL_KEYMAP_EVENT_COUNT:
    default: return "";
    }
}

const char *sxcl_keymap_event_label(sxcl_keymap_event event)
{
    switch (event) {
    case SXCL_KEYMAP_EVENT_PRESS: return "按下";
    case SXCL_KEYMAP_EVENT_LONG_PRESS: return "长按";
    case SXCL_KEYMAP_EVENT_CLICK: return "单击";
    case SXCL_KEYMAP_EVENT_DOUBLE_CLICK: return "双击";
    case SXCL_KEYMAP_EVENT_COUNT:
    default: return "";
    }
}

int sxcl_keymap_event_from_id(const char *id)
{
    if (!id) {
        return -1;
    }
    for (int i = 0; i < (int)SXCL_KEYMAP_EVENT_COUNT; ++i) {
        if (strcmp(id, sxcl_keymap_event_id((sxcl_keymap_event)i)) == 0) {
            return i;
        }
    }
    return -1;
}

static const char *normalize_from(const char *value, const char *const *allowed, size_t count,
                                  const char *fallback, int *normalized)
{
    if (normalized) {
        *normalized = 0;
    }
    if (value && *value) {
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(value, allowed[i]) == 0) {
                return allowed[i];
            }
        }
        if (normalized) {
            *normalized = 1;
        }
    }
    return fallback;
}

const char *sxcl_keymap_normalize_behavior(const char *value, int *normalized)
{
    static const char *const allowed[] = { "hold", "toggle", "tap" };
    return normalize_from(value, allowed, 3, "hold", normalized);
}

const char *sxcl_keymap_normalize_shape(const char *value, int *normalized)
{
    static const char *const allowed[] = { "round", "square", "pill" };
    return normalize_from(value, allowed, 3, "round", normalized);
}

const char *sxcl_keymap_normalize_style(const char *value, int *normalized)
{
    static const char *const allowed[] = { "dpad", "rocker", "dpad_compact" };
    return normalize_from(value, allowed, 3, "dpad_compact", normalized);
}

const char *sxcl_keymap_normalize_screen(const char *value, int *normalized)
{
    static const char *const allowed[] = { "landscape", "portrait" };
    return normalize_from(value, allowed, 2, "landscape", normalized);
}

/* ── 布局对象 ── */

void sxcl_keymap_layout_init(sxcl_keymap_layout *layout)
{
    if (!layout) {
        return;
    }
    memset(layout, 0, sizeof *layout);
    sxcl_kp_copy(layout->schema, sizeof layout->schema, SXCL_KEYMAP_SCHEMA);
    sxcl_kp_copy(layout->name, sizeof layout->name, "未命名");
    sxcl_kp_copy(layout->screen, sizeof layout->screen, "landscape");
}

void sxcl_keymap_layout_free(sxcl_keymap_layout *layout)
{
    if (!layout) {
        return;
    }
    free(layout->buttons);
    free(layout->directions);
    free(layout->meta_json);
    layout->buttons = NULL;
    layout->directions = NULL;
    layout->meta_json = NULL;
    layout->button_count = 0;
    layout->button_cap = 0;
    layout->direction_count = 0;
    layout->direction_cap = 0;
}

int sxcl_kp_push_button(sxcl_keymap_layout *layout, const sxcl_keymap_button *button)
{
    if (!layout || !button) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    if (layout->button_count >= layout->button_cap) {
        const size_t cap = layout->button_cap ? layout->button_cap * 2 : 8;
        sxcl_keymap_button *grown = (sxcl_keymap_button *)realloc(layout->buttons, cap * sizeof *grown);
        if (!grown) {
            return SXCL_KEYMAP_ERR_NOMEM;
        }
        layout->buttons = grown;
        layout->button_cap = cap;
    }
    layout->buttons[layout->button_count++] = *button;
    return SXCL_KEYMAP_OK;
}

int sxcl_kp_push_direction(sxcl_keymap_layout *layout, const sxcl_keymap_direction *direction)
{
    if (!layout || !direction) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    if (layout->direction_count >= layout->direction_cap) {
        const size_t cap = layout->direction_cap ? layout->direction_cap * 2 : 4;
        sxcl_keymap_direction *grown =
            (sxcl_keymap_direction *)realloc(layout->directions, cap * sizeof *grown);
        if (!grown) {
            return SXCL_KEYMAP_ERR_NOMEM;
        }
        layout->directions = grown;
        layout->direction_cap = cap;
    }
    layout->directions[layout->direction_count++] = *direction;
    return SXCL_KEYMAP_OK;
}

int sxcl_keymap_add_button(sxcl_keymap_layout *layout, const sxcl_keymap_button *button)
{
    return sxcl_kp_push_button(layout, button);
}

int sxcl_keymap_add_direction(sxcl_keymap_layout *layout, const sxcl_keymap_direction *direction)
{
    return sxcl_kp_push_direction(layout, direction);
}

const char *sxcl_kp_direction_key_name(size_t index)
{
    static const char *const names[] = { "up", "down", "left", "right" };
    return (index < 4) ? names[index] : "";
}

char *sxcl_kp_direction_key_slot(sxcl_keymap_direction *direction, const char *name)
{
    if (!direction || !name) {
        return NULL;
    }
    if (strcmp(name, "up") == 0) {
        return direction->up;
    }
    if (strcmp(name, "down") == 0) {
        return direction->down;
    }
    if (strcmp(name, "left") == 0) {
        return direction->left;
    }
    if (strcmp(name, "right") == 0) {
        return direction->right;
    }
    return NULL;
}

const char *sxcl_kp_direction_key_slot_const(const sxcl_keymap_direction *direction, const char *name)
{
    return sxcl_kp_direction_key_slot((sxcl_keymap_direction *)direction, name);
}

const sxcl_keymap_button *sxcl_keymap_button_by_id(const sxcl_keymap_layout *layout, const char *id)
{
    if (!layout || !id || !*id) {
        return NULL;
    }
    for (size_t i = 0; i < layout->button_count; ++i) {
        if (strcmp(layout->buttons[i].id, id) == 0) {
            return &layout->buttons[i];
        }
    }
    return NULL;
}

const sxcl_keymap_direction *sxcl_keymap_direction_by_id(const sxcl_keymap_layout *layout, const char *id)
{
    if (!layout || !id || !*id) {
        return NULL;
    }
    for (size_t i = 0; i < layout->direction_count; ++i) {
        if (strcmp(layout->directions[i].id, id) == 0) {
            return &layout->directions[i];
        }
    }
    return NULL;
}

size_t sxcl_keymap_control_count(const sxcl_keymap_layout *layout)
{
    return layout ? layout->button_count + layout->direction_count : 0;
}

const sxcl_keymap_binding *sxcl_keymap_button_event(const sxcl_keymap_button *button, sxcl_keymap_event event)
{
    if (!button || (int)event < 0 || (int)event >= (int)SXCL_KEYMAP_EVENT_COUNT) {
        return NULL;
    }
    if ((button->event_mask & (1u << (unsigned)event)) == 0) {
        return NULL;
    }
    return &button->events[event];
}

size_t sxcl_keymap_button_keys(const sxcl_keymap_button *button, const char *keys[], size_t cap)
{
    if (!button || !keys) {
        return 0;
    }
    size_t total = 0;
    for (size_t e = 0; e < SXCL_KEYMAP_EVENT_COUNT; ++e) {
        if ((button->event_mask & (1u << (unsigned)e)) == 0) {
            continue;
        }
        for (size_t k = 0; k < button->events[e].key_count; ++k) {
            const char *key = button->events[e].keys[k];
            int seen = 0;
            for (size_t t = 0; t < total; ++t) {
                if (strcmp(keys[t], key) == 0) {
                    seen = 1;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            if (total < cap) {
                keys[total] = key;
            }
            ++total;
        }
    }
    return total;
}

int sxcl_keymap_bind(sxcl_keymap_layout *layout, const char *button_id, sxcl_keymap_event event,
                     const char *action, const char *behavior,
                     const char *const *keys, size_t key_count)
{
    if (!layout || !button_id || (int)event < 0 || (int)event >= (int)SXCL_KEYMAP_EVENT_COUNT) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    for (size_t i = 0; i < layout->button_count; ++i) {
        if (strcmp(layout->buttons[i].id, button_id) != 0) {
            continue;
        }
        sxcl_keymap_binding *binding = &layout->buttons[i].events[event];
        memset(binding, 0, sizeof *binding);
        sxcl_kp_copy(binding->action, sizeof binding->action, action ? action : "");
        sxcl_kp_copy(binding->behavior, sizeof binding->behavior,
                     sxcl_keymap_normalize_behavior(behavior, NULL));
        if (keys) {
            for (size_t k = 0; k < key_count && k < SXCL_KEYMAP_KEYS_MAX; ++k) {
                sxcl_kp_copy(binding->keys[k], sizeof binding->keys[k], keys[k]);
            }
            binding->key_count = (key_count < SXCL_KEYMAP_KEYS_MAX) ? key_count : SXCL_KEYMAP_KEYS_MAX;
        }
        layout->buttons[i].event_mask |= (1u << (unsigned)event);
        return SXCL_KEYMAP_OK;
    }
    return SXCL_KEYMAP_ERR_ARG;
}

int sxcl_keymap_clone(const sxcl_keymap_layout *src, sxcl_keymap_layout *dst)
{
    if (!src || !dst) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    char *text = NULL;
    int rc = sxcl_keymap_to_json(src, &text, NULL, 0);
    if (rc != SXCL_KEYMAP_OK) {
        return rc;
    }
    rc = sxcl_keymap_parse(text, strlen(text), dst, NULL, NULL, 0);
    free(text);
    return rc;
}

/* ── 解析 ── */

static void parse_binding(const sxcl_json_value *value, sxcl_keymap_binding *out,
                          const char *control_id, const char *event_id, sxcl_keymap_issues *issues)
{
    memset(out, 0, sizeof *out);
    sxcl_kp_copy(out->action, sizeof out->action, sxcl_kp_string(value, "action", ""));
    int normalized = 0;
    const char *behavior = sxcl_kp_string(value, "behavior", "hold");
    const char *fixed = sxcl_keymap_normalize_behavior(behavior, &normalized);
    sxcl_kp_copy(out->behavior, sizeof out->behavior, fixed);
    if (normalized) {
        sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_WARNING, control_id,
                      "%s 的 behavior「%s」认不出来,已按 %s 处理", event_id, behavior, fixed);
    }
    const sxcl_json_value *keys = sxcl_json_get(value, "keys");
    if (keys && sxcl_json_type_of(keys) == SXCL_JSON_STRING) {
        /* Python 版允许只写一个字符串 */
        sxcl_kp_copy(out->keys[0], sizeof out->keys[0], sxcl_json_string(keys));
        out->key_count = 1;
    } else if (keys && sxcl_json_type_of(keys) == SXCL_JSON_ARRAY) {
        const size_t count = sxcl_json_size(keys);
        for (size_t i = 0; i < count; ++i) {
            const sxcl_json_value *item = sxcl_json_at(keys, i);
            if (!item || sxcl_json_type_of(item) != SXCL_JSON_STRING) {
                continue;
            }
            if (out->key_count >= SXCL_KEYMAP_KEYS_MAX) {
                sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_WARNING, control_id,
                              "%s 的按键超过 %d 个,后面的丢掉了", event_id, (int)SXCL_KEYMAP_KEYS_MAX);
                break;
            }
            sxcl_kp_copy(out->keys[out->key_count], sizeof out->keys[0], sxcl_json_string(item));
            ++out->key_count;
        }
    }
}

static void parse_control(const sxcl_json_value *item, int is_button, sxcl_keymap_layout *layout,
                          size_t index, sxcl_keymap_issues *issues)
{
    char id[SXCL_KEYMAP_ID_MAX];
    sxcl_kp_copy(id, sizeof id, sxcl_kp_string(item, "id", ""));
    char where[SXCL_KEYMAP_ID_MAX];
    if (id[0]) {
        sxcl_kp_copy(where, sizeof where, id);
    } else {
        (void)snprintf(where, sizeof where, "%s[%d]", is_button ? "buttons" : "directions", (int)index);
    }

    if (is_button) {
        sxcl_keymap_button button;
        memset(&button, 0, sizeof button);
        sxcl_kp_copy(button.id, sizeof button.id, id);
        sxcl_kp_copy(button.label, sizeof button.label, sxcl_kp_string(item, "label", ""));
        sxcl_kp_copy(button.hint, sizeof button.hint, sxcl_kp_string(item, "hint", ""));
        sxcl_kp_copy(button.icon, sizeof button.icon, sxcl_kp_string(item, "icon", ""));
        button.x = sxcl_kp_clamp(sxcl_kp_number(item, "x", 0.1), 0.0, 1.0);
        button.y = sxcl_kp_clamp(sxcl_kp_number(item, "y", 0.6), 0.0, 1.0);
        button.w = sxcl_kp_clamp(sxcl_kp_number(item, "w", 0.09), 0.01, 1.0);
        button.h = sxcl_kp_clamp(sxcl_kp_number(item, "h", 0.16), 0.01, 1.0);
        int normalized = 0;
        const char *shape = sxcl_keymap_normalize_shape(sxcl_kp_string(item, "shape", "round"), &normalized);
        sxcl_kp_copy(button.shape, sizeof button.shape, shape);
        if (normalized) {
            sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_WARNING, where,
                          "shape「%s」认不出来,已按 %s 处理", sxcl_kp_string(item, "shape", ""), shape);
        }
        button.opacity = sxcl_kp_clamp(sxcl_kp_number(item, "opacity", 0.55), 0.0, 1.0);
        sxcl_kp_copy(button.group, sizeof button.group, sxcl_kp_string(item, "group", ""));
        sxcl_kp_copy(button.alias_of, sizeof button.alias_of, sxcl_kp_string(item, "alias_of", ""));

        const sxcl_json_value *events = sxcl_json_get(item, "events");
        if (events && sxcl_json_type_of(events) == SXCL_JSON_OBJECT) {
            for (size_t i = 0; i < SXCL_KEYMAP_EVENT_COUNT; ++i) {
                const sxcl_keymap_event event = (sxcl_keymap_event)i;
                const sxcl_json_value *binding = sxcl_json_get(events, sxcl_keymap_event_id(event));
                if (!binding || sxcl_json_type_of(binding) != SXCL_JSON_OBJECT) {
                    continue;
                }
                parse_binding(binding, &button.events[i], where, sxcl_keymap_event_id(event), issues);
                button.event_mask |= (1u << (unsigned)i);
            }
            const size_t members = sxcl_json_member_count(events);
            for (size_t i = 0; i < members; ++i) {
                const char *key = sxcl_json_member_key(events, i);
                if (sxcl_keymap_event_from_id(key) < 0) {
                    sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_WARNING, where,
                                  "不认识的事件「%s」被丢掉了(只认 press/long_press/click/double_click)",
                                  key ? key : "");
                }
            }
        }
        if (sxcl_kp_push_button(layout, &button) != SXCL_KEYMAP_OK) {
            sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_ERROR, where, "内存不足,这个按钮没读进来");
        }
        return;
    }

    sxcl_keymap_direction direction;
    memset(&direction, 0, sizeof direction);
    sxcl_kp_copy(direction.id, sizeof direction.id, id);
    sxcl_kp_copy(direction.label, sizeof direction.label, sxcl_kp_string(item, "label", ""));
    sxcl_kp_copy(direction.hint, sizeof direction.hint, sxcl_kp_string(item, "hint", ""));
    direction.x = sxcl_kp_clamp(sxcl_kp_number(item, "x", 0.03), 0.0, 1.0);
    direction.y = sxcl_kp_clamp(sxcl_kp_number(item, "y", 0.55), 0.0, 1.0);
    direction.w = sxcl_kp_clamp(sxcl_kp_number(item, "w", 0.22), 0.02, 1.0);
    direction.h = sxcl_kp_clamp(sxcl_kp_number(item, "h", 0.38), 0.02, 1.0);
    int normalized = 0;
    const char *style = sxcl_keymap_normalize_style(sxcl_kp_string(item, "style", "dpad_compact"), &normalized);
    sxcl_kp_copy(direction.style, sizeof direction.style, style);
    if (normalized) {
        sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_WARNING, where,
                      "style「%s」认不出来,已按 %s 处理", sxcl_kp_string(item, "style", ""), style);
    }
    direction.opacity = sxcl_kp_clamp(sxcl_kp_number(item, "opacity", 0.5), 0.0, 1.0);
    direction.dead_zone = sxcl_kp_clamp(sxcl_kp_number(item, "dead_zone", 0.18), 0.0, 0.6);
    sxcl_kp_copy(direction.group, sizeof direction.group, sxcl_kp_string(item, "group", "left"));
    sxcl_kp_copy(direction.sprint_key, sizeof direction.sprint_key,
                 sxcl_kp_string(item, "sprint_key", "KEY_LEFT_SHIFT"));
    static const char *const defaults[] = { "KEY_W", "KEY_S", "KEY_A", "KEY_D" };
    const sxcl_json_value *keys = sxcl_json_get(item, "keys");
    for (size_t i = 0; i < 4; ++i) {
        const char *name = sxcl_kp_direction_key_name(i);
        char *slot = sxcl_kp_direction_key_slot(&direction, name);
        const char *got = (keys && sxcl_json_type_of(keys) == SXCL_JSON_OBJECT)
                              ? sxcl_kp_string(keys, name, defaults[i])
                              : defaults[i];
        sxcl_kp_copy(slot, SXCL_KEYMAP_KEY_MAX, got);
    }
    if (sxcl_kp_push_direction(layout, &direction) != SXCL_KEYMAP_OK) {
        sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_ERROR, where, "内存不足,这个方向控件没读进来");
    }
}

int sxcl_keymap_parse(const char *text, size_t len, sxcl_keymap_layout *out,
                      sxcl_keymap_issues *issues, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!text || !out) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "参数不合法");
        }
        return SXCL_KEYMAP_ERR_ARG;
    }
    sxcl_keymap_layout_init(out);

    char json_err[160];
    json_err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(text, len, json_err, sizeof json_err);
    if (!doc) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "%s", json_err[0] ? json_err : "JSON 解析失败");
        }
        return SXCL_KEYMAP_ERR_FORMAT;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    if (!root || sxcl_json_type_of(root) != SXCL_JSON_OBJECT) {
        sxcl_json_free(doc);
        if (err && err_len) {
            (void)snprintf(err, err_len, "顶层不是 JSON 对象,读不出按键布局");
        }
        return SXCL_KEYMAP_ERR_FORMAT;
    }

    sxcl_kp_copy(out->schema, sizeof out->schema, sxcl_kp_string(root, "schema", SXCL_KEYMAP_SCHEMA));
    sxcl_kp_copy(out->name, sizeof out->name, sxcl_kp_string(root, "name", "未命名"));
    int normalized = 0;
    const char *screen = sxcl_keymap_normalize_screen(sxcl_kp_string(root, "screen", "landscape"),
                                                      &normalized);
    sxcl_kp_copy(out->screen, sizeof out->screen, screen);
    if (normalized) {
        sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_WARNING, NULL,
                      "screen「%s」认不出来,已按 %s 处理", sxcl_kp_string(root, "screen", ""), screen);
    }
    sxcl_kp_copy(out->mc_version, sizeof out->mc_version, sxcl_kp_string(root, "mc_version", ""));
    sxcl_kp_copy(out->description, sizeof out->description, sxcl_kp_string(root, "description", ""));

    /* meta 原样保留(安卓端在里面放 builtin / preset / guide) */
    const sxcl_json_value *meta = sxcl_json_get(root, "meta");
    if (meta && sxcl_json_type_of(meta) == SXCL_JSON_OBJECT) {
        sxcl_kp_buf buf;
        sxcl_kp_buf_init(&buf);
        if (sxcl_kp_json_dump(&buf, meta, 1) == SXCL_KEYMAP_OK && buf.data) {
            if (buf.len <= SXCL_KEYMAP_META_MAX) {
                out->meta_json = buf.data;
                buf.data = NULL;
            } else {
                sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_WARNING, NULL,
                              "meta 太大(%d 字节),超过了 %d,已丢掉", (int)buf.len,
                              (int)SXCL_KEYMAP_META_MAX);
            }
        }
        sxcl_kp_buf_free(&buf);
    }

    const sxcl_json_value *directions = sxcl_json_get(root, "directions");
    if (directions && sxcl_json_type_of(directions) == SXCL_JSON_ARRAY) {
        const size_t count = sxcl_json_size(directions);
        for (size_t i = 0; i < count; ++i) {
            const sxcl_json_value *item = sxcl_json_at(directions, i);
            if (!item || sxcl_json_type_of(item) != SXCL_JSON_OBJECT) {
                sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_WARNING, NULL,
                              "directions[%d] 不是对象,跳过了", (int)i);
                continue;
            }
            parse_control(item, 0, out, i, issues);
        }
    }
    const sxcl_json_value *buttons = sxcl_json_get(root, "buttons");
    if (buttons && sxcl_json_type_of(buttons) == SXCL_JSON_ARRAY) {
        const size_t count = sxcl_json_size(buttons);
        for (size_t i = 0; i < count; ++i) {
            const sxcl_json_value *item = sxcl_json_at(buttons, i);
            if (!item || sxcl_json_type_of(item) != SXCL_JSON_OBJECT) {
                sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_WARNING, NULL,
                              "buttons[%d] 不是对象,跳过了", (int)i);
                continue;
            }
            parse_control(item, 1, out, i, issues);
        }
    }

    sxcl_json_free(doc);
    return SXCL_KEYMAP_OK;
}

int sxcl_keymap_load_file(const char *path, sxcl_keymap_layout *out, sxcl_keymap_issues *issues,
                          char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!path || !out) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    FILE *fh = sxcl_fs_fopen(path, "rb");
    if (!fh) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "打不开文件: %s", path);
        }
        return SXCL_KEYMAP_ERR_IO;
    }
    sxcl_kp_buf buf;
    sxcl_kp_buf_init(&buf);
    char chunk[4096];
    size_t got = 0;
    while ((got = fread(chunk, 1, sizeof chunk, fh)) > 0) {
        if (buf.len + got > SXCL_KP_FILE_MAX) {
            fclose(fh);
            sxcl_kp_buf_free(&buf);
            if (err && err_len) {
                (void)snprintf(err, err_len, "文件太大,不像是一份按键布局");
            }
            return SXCL_KEYMAP_ERR_SPACE;
        }
        const int rc = buf_reserve(&buf, got);
        if (rc != SXCL_KEYMAP_OK) {
            fclose(fh);
            sxcl_kp_buf_free(&buf);
            return rc;
        }
        memcpy(buf.data + buf.len, chunk, got);   /* 原样字节(别走 puts:里面有 NUL 就断了) */
        buf.len += got;
        buf.data[buf.len] = '\0';
    }
    fclose(fh);
    const int rc = sxcl_keymap_parse(buf.data ? buf.data : "", buf.len, out, issues, err, err_len);
    sxcl_kp_buf_free(&buf);
    return rc;
}

/* ── 序列化 ── */

static int dump_binding(sxcl_kp_buf *buf, const sxcl_keymap_binding *binding, int indent)
{
    int rc = sxcl_kp_buf_puts(buf, "{");
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"action\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, binding->action) : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"keys\": [") : rc;
    for (size_t i = 0; i < binding->key_count; ++i) {
        if (i) {
            rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ", ") : rc;
        }
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, binding->keys[i]) : rc;
    }
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "],") : rc;
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"behavior\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, binding->behavior) : rc;
    sxcl_kp_buf_indent(buf, indent);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "}") : rc;
    return rc;
}

static int dump_button(sxcl_kp_buf *buf, const sxcl_keymap_button *button, int indent)
{
    int rc = sxcl_kp_buf_puts(buf, "{");
    struct field_string { const char *key; const char *value; };
    const struct field_string strings[] = {
        { "id", button->id }, { "label", button->label },
        { "hint", button->hint }, { "icon", button->icon },
    };
    for (size_t i = 0; i < sizeof strings / sizeof strings[0] && rc == SXCL_KEYMAP_OK; ++i) {
        sxcl_kp_buf_indent(buf, indent + 1);
        rc = sxcl_kp_buf_json_string(buf, strings[i].key);
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ": ") : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, strings[i].value) : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    }
    struct field_number { const char *key; double value; };
    const struct field_number numbers[] = {
        { "x", button->x }, { "y", button->y }, { "w", button->w }, { "h", button->h },
    };
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0] && rc == SXCL_KEYMAP_OK; ++i) {
        sxcl_kp_buf_indent(buf, indent + 1);
        rc = sxcl_kp_buf_json_string(buf, numbers[i].key);
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ": ") : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_number(buf, numbers[i].value) : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    }
    struct field_plain { const char *head; const char *raw; };
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"shape\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, button->shape) : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"opacity\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_number(buf, button->opacity) : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"group\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, button->group) : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"alias_of\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, button->alias_of) : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"events\": {") : rc;
    int first = 1;
    for (size_t i = 0; i < SXCL_KEYMAP_EVENT_COUNT && rc == SXCL_KEYMAP_OK; ++i) {
        if ((button->event_mask & (1u << (unsigned)i)) == 0) {
            continue;
        }
        if (!first) {
            rc = sxcl_kp_buf_puts(buf, ",");
        }
        first = 0;
        sxcl_kp_buf_indent(buf, indent + 2);
        rc = (rc == SXCL_KEYMAP_OK)
                 ? sxcl_kp_buf_json_string(buf, sxcl_keymap_event_id((sxcl_keymap_event)i))
                 : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ": ") : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? dump_binding(buf, &button->events[i], indent + 2) : rc;
    }
    if (!first) {
        sxcl_kp_buf_indent(buf, indent + 1);
    }
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "}") : rc;
    sxcl_kp_buf_indent(buf, indent);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "}") : rc;
    return rc;
}

static int dump_direction(sxcl_kp_buf *buf, const sxcl_keymap_direction *direction, int indent)
{
    int rc = sxcl_kp_buf_puts(buf, "{");
    struct field_string { const char *key; const char *value; };
    const struct field_string strings[] = {
        { "id", direction->id }, { "label", direction->label }, { "hint", direction->hint },
    };
    for (size_t i = 0; i < sizeof strings / sizeof strings[0] && rc == SXCL_KEYMAP_OK; ++i) {
        sxcl_kp_buf_indent(buf, indent + 1);
        rc = sxcl_kp_buf_json_string(buf, strings[i].key);
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ": ") : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, strings[i].value) : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    }
    struct field_number { const char *key; double value; };
    const struct field_number numbers[] = {
        { "x", direction->x }, { "y", direction->y },
        { "w", direction->w }, { "h", direction->h },
    };
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0] && rc == SXCL_KEYMAP_OK; ++i) {
        sxcl_kp_buf_indent(buf, indent + 1);
        rc = sxcl_kp_buf_json_string(buf, numbers[i].key);
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ": ") : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_number(buf, numbers[i].value) : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    }
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"style\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, direction->style) : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"opacity\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_number(buf, direction->opacity) : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"dead_zone\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_number(buf, direction->dead_zone) : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"group\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, direction->group) : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ",") : rc;
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"keys\": {") : rc;
    for (size_t i = 0; i < 4 && rc == SXCL_KEYMAP_OK; ++i) {
        if (i) {
            rc = sxcl_kp_buf_puts(buf, ",");
        }
        sxcl_kp_buf_indent(buf, indent + 2);
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, sxcl_kp_direction_key_name(i)) : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, ": ") : rc;
        rc = (rc == SXCL_KEYMAP_OK)
                 ? sxcl_kp_buf_json_string(buf, sxcl_kp_direction_key_slot_const(
                                                     direction, sxcl_kp_direction_key_name(i)))
                 : rc;
    }
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "},") : rc;
    sxcl_kp_buf_indent(buf, indent + 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "\"sprint_key\": ") : rc;
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(buf, direction->sprint_key) : rc;
    sxcl_kp_buf_indent(buf, indent);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(buf, "}") : rc;
    return rc;
}

int sxcl_keymap_to_json(const sxcl_keymap_layout *layout, char **out_text, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!layout || !out_text) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    *out_text = NULL;
    sxcl_kp_buf buf;
    sxcl_kp_buf_init(&buf);
    int rc = sxcl_kp_buf_puts(&buf, "{");
    struct { const char *key; const char *value; } strings[] = {
        { "schema", layout->schema },
        { "name", layout->name },
        { "screen", layout->screen },
        { "mc_version", layout->mc_version },
        { "description", layout->description },
    };
    for (size_t i = 0; i < sizeof strings / sizeof strings[0] && rc == SXCL_KEYMAP_OK; ++i) {
        sxcl_kp_buf_indent(&buf, 1);
        rc = sxcl_kp_buf_json_string(&buf, strings[i].key);
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, ": ") : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_json_string(&buf, strings[i].value) : rc;
        rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, ",") : rc;
    }
    sxcl_kp_buf_indent(&buf, 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"meta\": ") : rc;
    if (rc == SXCL_KEYMAP_OK) {
        rc = sxcl_kp_buf_puts(&buf, (layout->meta_json && layout->meta_json[0]) ? layout->meta_json
                                                                                : "{}");
    }
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, ",") : rc;

    sxcl_kp_buf_indent(&buf, 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"buttons\": [") : rc;
    for (size_t i = 0; i < layout->button_count && rc == SXCL_KEYMAP_OK; ++i) {
        if (i) {
            rc = sxcl_kp_buf_puts(&buf, ",");
        }
        sxcl_kp_buf_indent(&buf, 2);
        rc = (rc == SXCL_KEYMAP_OK) ? dump_button(&buf, &layout->buttons[i], 2) : rc;
    }
    if (layout->button_count) {
        sxcl_kp_buf_indent(&buf, 1);
    }
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "],") : rc;

    sxcl_kp_buf_indent(&buf, 1);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "\"directions\": [") : rc;
    for (size_t i = 0; i < layout->direction_count && rc == SXCL_KEYMAP_OK; ++i) {
        if (i) {
            rc = sxcl_kp_buf_puts(&buf, ",");
        }
        sxcl_kp_buf_indent(&buf, 2);
        rc = (rc == SXCL_KEYMAP_OK) ? dump_direction(&buf, &layout->directions[i], 2) : rc;
    }
    if (layout->direction_count) {
        sxcl_kp_buf_indent(&buf, 1);
    }
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "]") : rc;
    sxcl_kp_buf_indent(&buf, 0);
    rc = (rc == SXCL_KEYMAP_OK) ? sxcl_kp_buf_puts(&buf, "}") : rc;

    if (rc != SXCL_KEYMAP_OK) {
        sxcl_kp_buf_free(&buf);
        if (err && err_len) {
            (void)snprintf(err, err_len, "序列化失败(内存不足?)");
        }
        return rc;
    }
    *out_text = buf.data;   /* 所有权交给调用方 */
    return SXCL_KEYMAP_OK;
}

int sxcl_keymap_save_file(const sxcl_keymap_layout *layout, const char *path, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!layout || !path || !*path) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    char *text = NULL;
    int rc = sxcl_keymap_to_json(layout, &text, err, err_len);
    if (rc != SXCL_KEYMAP_OK) {
        return rc;
    }
    if (sxcl_fs_mkdirs_for_file(path) != 0) {
        free(text);
        if (err && err_len) {
            (void)snprintf(err, err_len, "建不了目录: %s", path);
        }
        return SXCL_KEYMAP_ERR_IO;
    }
    char tmp[1024];
    if (sxcl_kp_copy(tmp, sizeof tmp, path) != 0 || strlen(tmp) + 5 > sizeof tmp) {
        free(text);
        if (err && err_len) {
            (void)snprintf(err, err_len, "路径太长: %s", path);
        }
        return SXCL_KEYMAP_ERR_SPACE;
    }
    strcat(tmp, ".tmp");
    FILE *fh = sxcl_fs_fopen(tmp, "wb");
    if (!fh) {
        free(text);
        if (err && err_len) {
            (void)snprintf(err, err_len, "写不了临时文件: %s", tmp);
        }
        return SXCL_KEYMAP_ERR_IO;
    }
    const size_t len = strlen(text);
    const size_t written = fwrite(text, 1, len, fh);
    fclose(fh);
    free(text);
    if (written != len) {
        (void)sxcl_fs_remove(tmp);
        if (err && err_len) {
            (void)snprintf(err, err_len, "写文件没写完: %s", tmp);
        }
        return SXCL_KEYMAP_ERR_IO;
    }
    if (sxcl_fs_rename_replace(tmp, path) != 0) {
        (void)sxcl_fs_remove(tmp);
        if (err && err_len) {
            (void)snprintf(err, err_len, "改名失败: %s", path);
        }
        return SXCL_KEYMAP_ERR_IO;
    }
    return SXCL_KEYMAP_OK;
}

/* ── meta 读取 ── */

static sxcl_json *meta_doc(const sxcl_keymap_layout *layout)
{
    if (!layout || !layout->meta_json || !layout->meta_json[0]) {
        return NULL;
    }
    char err[64];
    return sxcl_json_parse(layout->meta_json, strlen(layout->meta_json), err, sizeof err);
}

int sxcl_keymap_meta_bool(const sxcl_keymap_layout *layout, const char *key, int def)
{
    sxcl_json *doc = meta_doc(layout);
    if (!doc) {
        return def;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const sxcl_json_value *value = (root && key) ? sxcl_json_get(root, key) : NULL;
    int result = def;
    if (value && sxcl_json_type_of(value) == SXCL_JSON_BOOL) {
        result = sxcl_json_bool(value);
    }
    sxcl_json_free(doc);
    return result;
}

int sxcl_keymap_meta_string(const sxcl_keymap_layout *layout, const char *key, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    out[0] = '\0';
    sxcl_json *doc = meta_doc(layout);
    if (!doc) {
        return SXCL_KEYMAP_ERR_FORMAT;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const sxcl_json_value *value = (root && key) ? sxcl_json_get(root, key) : NULL;
    int rc = SXCL_KEYMAP_ERR_FORMAT;
    if (value && sxcl_json_type_of(value) == SXCL_JSON_STRING) {
        sxcl_kp_copy(out, out_len, sxcl_json_string(value));
        rc = SXCL_KEYMAP_OK;
    }
    sxcl_json_free(doc);
    return rc;
}

/* ── 校验 ── */

void sxcl_keymap_issues_reset(sxcl_keymap_issues *issues)
{
    if (!issues) {
        return;
    }
    issues->count = 0;
    issues->dropped = 0;
}

int sxcl_keymap_issues_has_error(const sxcl_keymap_issues *issues)
{
    if (!issues) {
        return 0;
    }
    for (size_t i = 0; i < issues->count; ++i) {
        if (issues->items[i].level == SXCL_KEYMAP_LEVEL_ERROR) {
            return 1;
        }
    }
    return 0;
}

/* 控件统一访问(前 directions 段、后 buttons 段),与安卓端 controls() 的顺序一致。 */
typedef struct control_ref {
    const char *id;
    double x, y, w, h;
    int is_button;
    const sxcl_keymap_button *button;       /* is_button 时非空 */
    const sxcl_keymap_direction *direction; /* !is_button 时非空 */
} control_ref;

static int control_ref_at(const sxcl_keymap_layout *layout, size_t index, control_ref *out)
{
    if (!layout || !out) {
        return 0;
    }
    memset(out, 0, sizeof *out);
    if (index < layout->direction_count) {
        const sxcl_keymap_direction *d = &layout->directions[index];
        out->id = d->id;
        out->x = d->x; out->y = d->y; out->w = d->w; out->h = d->h;
        out->is_button = 0;
        out->direction = d;
        return 1;
    }
    index -= layout->direction_count;
    if (index >= layout->button_count) {
        return 0;
    }
    const sxcl_keymap_button *b = &layout->buttons[index];
    out->id = b->id;
    out->x = b->x; out->y = b->y; out->w = b->w; out->h = b->h;
    out->is_button = 1;
    out->button = b;
    return 1;
}

size_t sxcl_keymap_validate(const sxcl_keymap_layout *layout, sxcl_keymap_issues *issues)
{
    if (!layout || !issues) {
        return 0;
    }
    const size_t before = issues->count;
    const size_t total = sxcl_keymap_control_count(layout);
    for (size_t i = 0; i < total; ++i) {
        control_ref ref;
        if (!control_ref_at(layout, i, &ref)) {
            continue;
        }
        const char *id = ref.id;
        if (!id[0]) {
            sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_ERROR, "", "存在没有 id 的控件");
            continue;
        }
        for (size_t j = 0; j < i; ++j) {
            control_ref earlier;
            if (control_ref_at(layout, j, &earlier) && earlier.id[0] && strcmp(earlier.id, id) == 0) {
                sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_ERROR, id, "控件 id 重复: %s", id);
                break;
            }
        }
        if (ref.x + ref.w > 1.001 || ref.y + ref.h > 1.001 || ref.x < 0.0 || ref.y < 0.0) {
            sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_ERROR, id, "%s: 超出屏幕范围", id);
        }
        if (ref.is_button && ref.button->event_mask == 0) {
            sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_ERROR, id, "%s: 没有绑定任何事件", id);
        }
    }
    return issues->count - before;
}

/* 重叠面积 / 较小者面积(安卓端 overlapRatio)。 */
static double overlap_ratio(double ax, double ay, double aw, double ah,
                            double bx, double by, double bw, double bh)
{
    const double left = (ax > bx) ? ax : bx;
    const double top = (ay > by) ? ay : by;
    const double right = ((ax + aw) < (bx + bw)) ? (ax + aw) : (bx + bw);
    const double bottom = ((ay + ah) < (by + bh)) ? (ay + ah) : (by + bh);
    const double width = right - left;
    const double height = bottom - top;
    if (width <= 0.0 || height <= 0.0) {
        return 0.0;
    }
    const double overlap = width * height;
    const double area_a = aw * ah;
    const double area_b = bw * bh;
    const double smaller = (area_a < area_b) ? area_a : area_b;
    return (smaller <= 0.0) ? 0.0 : overlap / smaller;
}

/* ── 冲突检测 ── */

/* 冲突检测里"控件 -> 这个键上挂着哪些动作"的中间表。
 * 控件与键的数量都不多(几十个),定长表足够;超出就丢掉后面的(不会崩,也不会误报)。 */
#define SXCL_KP_OWNER_MAX 64
#define SXCL_KP_KEY_MAX 48
#define SXCL_KP_ACTION_LIST 128
#define SXCL_KP_ACTION_ITEMS 8

typedef struct owner_row {
    char id[SXCL_KEYMAP_ID_MAX];
    char actions[SXCL_KP_ACTION_LIST];   /* 已排序、逗号分隔(与安卓端 String.join(",") 同形) */
} owner_row;

typedef struct key_row {
    char key[SXCL_KEYMAP_KEY_MAX];
    owner_row owners[SXCL_KP_OWNER_MAX];
    size_t owner_count;
} key_row;

static void append_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0 || !src) {
        return;
    }
    const size_t len = strlen(dst);
    if (len + 1 >= cap) {
        return;
    }
    sxcl_kp_copy(dst + len, cap - len, src);
}

/* 逗号分隔串 -> 数组(不依赖 strtok:本模块要保持可重入,不能有隐藏状态)。 */
static size_t csv_split(const char *csv, char items[][SXCL_KEYMAP_ACTION_MAX], size_t cap)
{
    size_t count = 0;
    if (!csv) {
        return 0;
    }
    const char *p = csv;
    while (*p && count < cap) {
        while (*p == ',') {
            ++p;
        }
        if (!*p) {
            break;
        }
        const char *begin = p;
        while (*p && *p != ',') {
            ++p;
        }
        sxcl_kp_copy_range(items[count], SXCL_KEYMAP_ACTION_MAX, begin, (size_t)(p - begin));
        ++count;
    }
    return count;
}

static size_t csv_count(const char *csv)
{
    char items[SXCL_KP_ACTION_ITEMS][SXCL_KEYMAP_ACTION_MAX];
    return csv_split(csv, items, SXCL_KP_ACTION_ITEMS);
}

/* 加一个动作(去重 + 排序,与 Java 的 LinkedHashSet + Collections.sort 等价)。 */
static void csv_add(char *list, size_t cap, const char *token)
{
    if (!token || !*token) {
        return;
    }
    char items[SXCL_KP_ACTION_ITEMS][SXCL_KEYMAP_ACTION_MAX];
    size_t count = csv_split(list, items, SXCL_KP_ACTION_ITEMS);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(items[i], token) == 0) {
            return;
        }
    }
    if (count < SXCL_KP_ACTION_ITEMS) {
        sxcl_kp_copy(items[count++], SXCL_KEYMAP_ACTION_MAX, token);
    }
    for (size_t i = 1; i < count; ++i) {
        char tmp[SXCL_KEYMAP_ACTION_MAX];
        sxcl_kp_copy(tmp, sizeof tmp, items[i]);
        size_t j = i;
        while (j > 0 && strcmp(items[j - 1], tmp) > 0) {
            sxcl_kp_copy(items[j], SXCL_KEYMAP_ACTION_MAX, items[j - 1]);
            --j;
        }
        sxcl_kp_copy(items[j], SXCL_KEYMAP_ACTION_MAX, tmp);
    }
    list[0] = '\0';
    for (size_t i = 0; i < count; ++i) {
        if (i) {
            append_str(list, cap, ",");
        }
        append_str(list, cap, items[i]);
    }
}

static void csv_merge(char *list, size_t cap, const char *csv)
{
    char items[SXCL_KP_ACTION_ITEMS][SXCL_KEYMAP_ACTION_MAX];
    const size_t count = csv_split(csv, items, SXCL_KP_ACTION_ITEMS);
    for (size_t i = 0; i < count; ++i) {
        csv_add(list, cap, items[i]);
    }
}

size_t sxcl_keymap_conflicts(const sxcl_keymap_layout *layout, sxcl_keymap_issues *issues)
{
    if (!layout || !issues) {
        return 0;
    }
    const size_t before = issues->count;
    const size_t total = sxcl_keymap_control_count(layout);

    /* 1) 同一个按键被不同动作的控件抢:
     *    * 同一个控件按下/长按绑同一个键 = 正常(FCL 的挖矿就是这样);
     *    * 两个控件绑同一个键但动作相同 = 别名按钮(比如"盾"和"放"都是右键),不算错;
     *    * 动作不同才是真冲突。 */
    key_row keys[SXCL_KP_KEY_MAX];
    size_t key_count = 0;
    memset(keys, 0, sizeof keys);

    for (size_t i = 0; i < total; ++i) {
        control_ref ref;
        if (!control_ref_at(layout, i, &ref) || !ref.id[0]) {
            continue;
        }
        const char *control_keys[SXCL_KEYMAP_KEYS_MAX * SXCL_KEYMAP_EVENT_COUNT];
        size_t control_key_count = 0;
        char alias_action[SXCL_KEYMAP_ACTION_MAX];
        alias_action[0] = '\0';
        if (ref.is_button) {
            control_key_count = sxcl_keymap_button_keys(ref.button, control_keys,
                                                        sizeof control_keys / sizeof control_keys[0]);
            if (ref.button->alias_of[0]) {
                sxcl_kp_copy(alias_action, sizeof alias_action, ref.button->alias_of);
            }
        } else {
            for (size_t k = 0; k < 4; ++k) {
                control_keys[control_key_count++] = sxcl_kp_direction_key_slot_const(
                    ref.direction, sxcl_kp_direction_key_name(k));
            }
        }

        for (size_t k = 0; k < control_key_count; ++k) {
            const char *key = control_keys[k];
            if (!key || !*key) {
                continue;
            }
            char acts[SXCL_KP_ACTION_LIST];
            acts[0] = '\0';
            if (alias_action[0]) {
                sxcl_kp_copy(acts, sizeof acts, alias_action);
            } else if (ref.is_button) {
                for (size_t e = 0; e < SXCL_KEYMAP_EVENT_COUNT; ++e) {
                    if ((ref.button->event_mask & (1u << (unsigned)e)) == 0) {
                        continue;
                    }
                    const sxcl_keymap_binding *binding = &ref.button->events[e];
                    for (size_t b = 0; b < binding->key_count; ++b) {
                        if (strcmp(binding->keys[b], key) == 0) {
                            csv_add(acts, sizeof acts, binding->action);
                        }
                    }
                }
            }
            key_row *row = NULL;
            for (size_t r = 0; r < key_count; ++r) {
                if (strcmp(keys[r].key, key) == 0) {
                    row = &keys[r];
                    break;
                }
            }
            if (!row) {
                if (key_count >= SXCL_KP_KEY_MAX) {
                    continue;
                }
                row = &keys[key_count++];
                memset(row, 0, sizeof *row);
                sxcl_kp_copy(row->key, sizeof row->key, key);
            }
            owner_row *owner = NULL;
            for (size_t o = 0; o < row->owner_count; ++o) {
                if (strcmp(row->owners[o].id, ref.id) == 0) {
                    owner = &row->owners[o];
                    break;
                }
            }
            if (!owner) {
                if (row->owner_count >= SXCL_KP_OWNER_MAX) {
                    continue;
                }
                owner = &row->owners[row->owner_count++];
                memset(owner, 0, sizeof *owner);
                sxcl_kp_copy(owner->id, sizeof owner->id, ref.id);
            }
            csv_merge(owner->actions, sizeof owner->actions, acts);
        }
    }

    for (size_t r = 0; r < key_count; ++r) {
        const key_row *row = &keys[r];
        if (row->owner_count < 2) {
            continue;
        }
        char all[SXCL_KP_ACTION_LIST * 2];
        all[0] = '\0';
        int multi = 0;
        for (size_t o = 0; o < row->owner_count; ++o) {
            if (strchr(row->owners[o].actions, ',') != NULL) {
                multi = 1;   /* 同一个控件自己就在这个键上挂了好几个动作 */
            }
            csv_merge(all, sizeof all, row->owners[o].actions);
        }
        if (csv_count(all) <= 1 && !multi) {
            continue;
        }
        char names[320];
        names[0] = '\0';
        for (size_t o = 0; o < row->owner_count; ++o) {
            if (o) {
                append_str(names, sizeof names, "、");
            }
            append_str(names, sizeof names, row->owners[o].id);
        }
        sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_WARNING, row->owners[0].id,
                      "按键 %s 被 %d 个控件绑到不同动作（%s），会互相打架", row->key,
                      (int)row->owner_count, names);
    }

    /* 2) 两个控件位置重叠超过 60%(触屏容易误触) */
    for (size_t i = 0; i < total; ++i) {
        control_ref first;
        if (!control_ref_at(layout, i, &first)) {
            continue;
        }
        for (size_t j = i + 1; j < total; ++j) {
            control_ref second;
            if (!control_ref_at(layout, j, &second)) {
                continue;
            }
            const double ratio = overlap_ratio(first.x, first.y, first.w, first.h,
                                               second.x, second.y, second.w, second.h);
            if (ratio > 0.6) {
                sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_WARNING, first.id,
                              "%s 与 %s 位置重叠（%d%%），触屏容易误触", first.id, second.id,
                              (int)(ratio * 100.0 + 0.5));
            }
        }
    }

    /* 3) 关键动作缺失 —— 新手最容易卡住的地方 */
    struct required_row { const char *action; const char *tip; int needs_direction; };
    static const struct required_row required[] = {
        { "jump", "跳跃", 0 },
        { "forward", "移动", 1 },
        { "inventory", "打开背包", 0 },
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i) {
        if (required[i].needs_direction && layout->direction_count > 0) {
            continue;   /* 有方向控件就等于"移动"绑好了 */
        }
        char owners_buf[8][SXCL_KEYMAP_ID_MAX];
        if (sxcl_keymap_action_owners(layout, required[i].action, owners_buf, 8) == 0) {
            sxcl_kp_issue(issues, SXCL_KEYMAP_LEVEL_ERROR, "",
                          "没有绑定「%s」，进游戏后会寸步难行", required[i].tip);
        }
    }

    return issues->count - before;
}

/* ── 搜索 / 动作索引 ── */

size_t sxcl_keymap_action_owners(const sxcl_keymap_layout *layout, const char *action,
                                 char (*ids)[SXCL_KEYMAP_ID_MAX], size_t cap)
{
    if (!layout || !action || !*action) {
        return 0;
    }
    size_t total = 0;
    for (size_t i = 0; i < layout->button_count; ++i) {
        const sxcl_keymap_button *button = &layout->buttons[i];
        int owned = 0;
        for (size_t e = 0; e < SXCL_KEYMAP_EVENT_COUNT && !owned; ++e) {
            if ((button->event_mask & (1u << (unsigned)e)) == 0) {
                continue;
            }
            if (strcmp(button->events[e].action, action) == 0) {
                owned = 1;
            }
        }
        if (owned) {
            if (ids && total < cap) {
                sxcl_kp_copy(ids[total], SXCL_KEYMAP_ID_MAX, button->id);
            }
            ++total;
        }
    }
    for (size_t i = 0; i < layout->direction_count; ++i) {
        const sxcl_keymap_direction *direction = &layout->directions[i];
        for (size_t k = 0; k < 4; ++k) {
            if (strcmp(sxcl_kp_direction_key_name(k), action) == 0) {
                if (ids && total < cap) {
                    sxcl_kp_copy(ids[total], SXCL_KEYMAP_ID_MAX, direction->id);
                }
                ++total;
                break;
            }
        }
    }
    return total;
}

static int text_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) {
        return 0;
    }
    const size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i]) {
            char a = p[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            ++i;
        }
        if (i == nlen) {
            return 1;
        }
    }
    return 0;
}

size_t sxcl_keymap_find(const sxcl_keymap_layout *layout, const char *query, sxcl_keymap_hits *hits)
{
    if (hits) {
        hits->count = 0;
        hits->dropped = 0;
    }
    if (!layout || !query || !*query) {
        return 0;
    }
    char needle[SXCL_KEYMAP_HINT_MAX];
    sxcl_kp_copy(needle, sizeof needle, query);
    char *start = needle;
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    size_t len = strlen(start);
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) {
        start[--len] = '\0';
    }
    if (len == 0) {
        return 0;
    }

    const size_t total_controls = sxcl_keymap_control_count(layout);
    size_t total = 0;
    for (size_t i = 0; i < total_controls; ++i) {
        control_ref ref;
        if (!control_ref_at(layout, i, &ref)) {
            continue;
        }
        int hit = text_contains_ci(ref.id, start);
        if (ref.is_button) {
            const sxcl_keymap_button *button = ref.button;
            hit = hit || text_contains_ci(button->label, start) || text_contains_ci(button->hint, start);
            const char *keys[SXCL_KEYMAP_KEYS_MAX * SXCL_KEYMAP_EVENT_COUNT];
            const size_t key_count = sxcl_keymap_button_keys(button, keys,
                                                            sizeof keys / sizeof keys[0]);
            for (size_t k = 0; k < key_count && !hit; ++k) {
                hit = text_contains_ci(keys[k], start);
            }
            for (size_t e = 0; e < SXCL_KEYMAP_EVENT_COUNT && !hit; ++e) {
                if ((button->event_mask & (1u << (unsigned)e)) == 0) {
                    continue;
                }
                hit = text_contains_ci(button->events[e].action, start);
            }
        } else {
            const sxcl_keymap_direction *direction = ref.direction;
            hit = hit || text_contains_ci(direction->label, start) ||
                  text_contains_ci(direction->hint, start);
            for (size_t k = 0; k < 4 && !hit; ++k) {
                const char *name = sxcl_kp_direction_key_name(k);
                hit = text_contains_ci(name, start) ||
                      text_contains_ci(sxcl_kp_direction_key_slot_const(direction, name), start);
            }
            if (!hit) {
                hit = text_contains_ci("sprint", start) || text_contains_ci(direction->sprint_key, start);
            }
        }
        if (!hit) {
            continue;
        }
        if (hits && hits->count < SXCL_KEYMAP_HITS_MAX) {
            sxcl_kp_copy(hits->ids[hits->count], SXCL_KEYMAP_ID_MAX, ref.id);
            ++hits->count;
        } else if (hits) {
            ++hits->dropped;
        }
        ++total;
    }
    return total;
}

