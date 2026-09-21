/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/keymap.h"

#include "keymap_assets.inc"

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what)
{
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (got && want && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)");
    }
}

static void check_size(size_t got, size_t want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %lu want %lu\n", what, (unsigned long)got, (unsigned long)want);
    }
}

static void check_int(int got, int want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %d want %d\n", what, got, want);
    }
}

static int nearly(double a, double b)
{
    double diff = a - b;
    if (diff < 0) {
        diff = -diff;
    }
    return diff < 1e-6;
}

static void check_near(double got, double want, const char *what)
{
    if (nearly(got, want)) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %.6f want %.6f\n", what, got, want);
    }
}

/* ── 两个布局是否逐字段相等(why 里给第一处不同,便于定位) ── */

static int bindings_equal(const sxcl_keymap_binding *a, const sxcl_keymap_binding *b)
{
    if (strcmp(a->action, b->action) != 0 || strcmp(a->behavior, b->behavior) != 0 ||
        a->key_count != b->key_count) {
        return 0;
    }
    for (size_t i = 0; i < a->key_count; ++i) {
        if (strcmp(a->keys[i], b->keys[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

static int controls_equal(const sxcl_keymap_layout *a, const sxcl_keymap_layout *b, const char **why)
{
    if (a->button_count != b->button_count) {
        *why = "按钮数量不同";
        return 0;
    }
    if (a->direction_count != b->direction_count) {
        *why = "方向控件数量不同";
        return 0;
    }
    for (size_t i = 0; i < a->button_count; ++i) {
        const sxcl_keymap_button *x = &a->buttons[i];
        const sxcl_keymap_button *y = &b->buttons[i];
        if (strcmp(x->id, y->id) != 0) { *why = "按钮 id 不同"; return 0; }
        if (strcmp(x->label, y->label) != 0) { *why = "按钮 label 不同"; return 0; }
        if (strcmp(x->hint, y->hint) != 0) { *why = "按钮 hint 不同"; return 0; }
        if (strcmp(x->icon, y->icon) != 0) { *why = "按钮 icon 不同"; return 0; }
        if (!nearly(x->x, y->x) || !nearly(x->y, y->y) || !nearly(x->w, y->w) || !nearly(x->h, y->h)) {
            *why = "按钮坐标不同";
            return 0;
        }
        if (strcmp(x->shape, y->shape) != 0) { *why = "按钮 shape 不同"; return 0; }
        if (!nearly(x->opacity, y->opacity)) { *why = "按钮 opacity 不同"; return 0; }
        if (strcmp(x->group, y->group) != 0) { *why = "按钮 group 不同"; return 0; }
        if (strcmp(x->alias_of, y->alias_of) != 0) { *why = "按钮 alias_of 不同"; return 0; }
        if (x->event_mask != y->event_mask) { *why = "按钮事件集合不同"; return 0; }
        for (size_t e = 0; e < SXCL_KEYMAP_EVENT_COUNT; ++e) {
            if ((x->event_mask & (1u << (unsigned)e)) == 0) {
                continue;
            }
            if (!bindings_equal(&x->events[e], &y->events[e])) { *why = "事件绑定不同"; return 0; }
        }
    }
    for (size_t i = 0; i < a->direction_count; ++i) {
        const sxcl_keymap_direction *x = &a->directions[i];
        const sxcl_keymap_direction *y = &b->directions[i];
        if (strcmp(x->id, y->id) != 0) { *why = "方向 id 不同"; return 0; }
        if (strcmp(x->label, y->label) != 0) { *why = "方向 label 不同"; return 0; }
        if (strcmp(x->hint, y->hint) != 0) { *why = "方向 hint 不同"; return 0; }
        if (!nearly(x->x, y->x) || !nearly(x->y, y->y) || !nearly(x->w, y->w) || !nearly(x->h, y->h)) {
            *why = "方向坐标不同";
            return 0;
        }
        if (strcmp(x->style, y->style) != 0) { *why = "方向 style 不同"; return 0; }
        if (!nearly(x->opacity, y->opacity) || !nearly(x->dead_zone, y->dead_zone)) {
            *why = "方向透明度/死区不同";
            return 0;
        }
        if (strcmp(x->group, y->group) != 0) { *why = "方向 group 不同"; return 0; }
        if (strcmp(x->up, y->up) != 0 || strcmp(x->down, y->down) != 0 ||
            strcmp(x->left, y->left) != 0 || strcmp(x->right, y->right) != 0) {
            *why = "方向 keys 不同";
            return 0;
        }
        if (strcmp(x->sprint_key, y->sprint_key) != 0) { *why = "方向 sprint_key 不同"; return 0; }
    }
    return 1;
}

/* 比较两份文本(忽略 CR:仓库里检出成 CRLF 还是 LF 不该让漂移检查变红)。 */
static int text_equal_no_cr(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a == '\r') { ++a; continue; }
        if (*b == '\r') { ++b; continue; }
        if (*a != *b) {
            return 0;
        }
        ++a;
        ++b;
    }
    while (*a == '\r') { ++a; }
    while (*b == '\r') { ++b; }
    return *a == '\0' && *b == '\0';
}

/* ── 1) 安卓端 9 套资产 ── */

static void test_android_assets(void)
{
    check_size(k_keymap_asset_count, 9, "资产: 内嵌了 9 套布局");
    check_size(sizeof k_keymap_expect / sizeof k_keymap_expect[0], 9, "资产: 9 行期望值");

    for (size_t i = 0; i < k_keymap_asset_count; ++i) {
        const char *file = k_keymap_assets[i].file;
        const char *text = k_keymap_assets[i].text;
        char what[160];

        sxcl_keymap_issues issues;
        sxcl_keymap_issues_reset(&issues);
        sxcl_keymap_layout layout;
        char err[160];
        const int rc = sxcl_keymap_parse(text, strlen(text), &layout, &issues, err, sizeof err);
        (void)snprintf(what, sizeof what, "资产 %s: 解析成功", file);
        check_int(rc, SXCL_KEYMAP_OK, what);
        if (rc != SXCL_KEYMAP_OK) {
            printf("        (err=%s)\n", err);
            continue;
        }

        /* 独立解析出来的期望值(PowerShell ConvertFrom-Json) */
        const keymap_expect_row *want = NULL;
        for (size_t k = 0; k < sizeof k_keymap_expect / sizeof k_keymap_expect[0]; ++k) {
            if (strcmp(k_keymap_expect[k].file, file) == 0) {
                want = &k_keymap_expect[k];
            }
        }
        (void)snprintf(what, sizeof what, "资产 %s: 期望值表里有它", file);
        check(want != NULL, what);
        if (!want) {
            sxcl_keymap_layout_free(&layout);
            continue;
        }
        (void)snprintf(what, sizeof what, "资产 %s: name", file);
        check_str(layout.name, want->name, what);
        (void)snprintf(what, sizeof what, "资产 %s: screen", file);
        check_str(layout.screen, want->screen, what);
        (void)snprintf(what, sizeof what, "资产 %s: mc_version", file);
        check_str(layout.mc_version, want->mc_version, what);
        (void)snprintf(what, sizeof what, "资产 %s: schema", file);
        check_str(layout.schema, "sxcl.keymap.v1", what);
        (void)snprintf(what, sizeof what, "资产 %s: 按钮数", file);
        check_size(layout.button_count, (size_t)want->buttons, what);
        (void)snprintf(what, sizeof what, "资产 %s: 方向控件数", file);
        check_size(layout.direction_count, (size_t)want->directions, what);
        if (layout.button_count > 0) {
            (void)snprintf(what, sizeof what, "资产 %s: 第一个按钮 id", file);
            check_str(layout.buttons[0].id, want->first_button, what);
            (void)snprintf(what, sizeof what, "资产 %s: 第一个按钮 x/y", file);
            check(nearly(layout.buttons[0].x, want->first_x) && nearly(layout.buttons[0].y, want->first_y),
                  what);
        }
        /* meta(安卓端在里面放 builtin / preset) */
        char meta_buf[64];
        (void)snprintf(what, sizeof what, "资产 %s: meta.builtin", file);
        check_int(sxcl_keymap_meta_bool(&layout, "builtin", 0), want->builtin, what);
        (void)snprintf(what, sizeof what, "资产 %s: meta.preset", file);
        check(sxcl_keymap_meta_string(&layout, "preset", meta_buf, sizeof meta_buf) == SXCL_KEYMAP_OK,
              what);
        check_str(meta_buf, want->preset, what);
        (void)snprintf(what, sizeof what, "资产 %s: meta 原样保留(非空)", file);
        check(layout.meta_json != NULL && layout.meta_json[0] == '{', what);

        /* 解析 9 套现成资产不该报 error(它们都是官方生成的) */
        size_t errors = 0;
        sxcl_keymap_issues_reset(&issues);
        sxcl_keymap_validate(&layout, &issues);
        for (size_t k = 0; k < issues.count; ++k) {
            if (issues.items[k].level == SXCL_KEYMAP_LEVEL_ERROR) {
                ++errors;
                printf("        (%s: %s)\n", file, issues.items[k].message);
            }
        }
        (void)snprintf(what, sizeof what, "资产 %s: 校验无 error", file);
        check_size(errors, 0, what);

        /* 也不能有冲突(资产是"打开就能玩"的) */
        sxcl_keymap_issues_reset(&issues);
        sxcl_keymap_conflicts(&layout, &issues);
        size_t conflict_errors = 0;
        for (size_t k = 0; k < issues.count; ++k) {
            if (issues.items[k].level == SXCL_KEYMAP_LEVEL_ERROR) {
                ++conflict_errors;
                printf("        (%s 冲突: %s)\n", file, issues.items[k].message);
            }
        }
        (void)snprintf(what, sizeof what, "资产 %s: 无 error 级冲突", file);
        check_size(conflict_errors, 0, what);

        /* 往返:保存 -> 再解析 -> 逐字段等价(含 meta) */
        char *saved = NULL;
        (void)snprintf(what, sizeof what, "资产 %s: 序列化成功", file);
        check_int(sxcl_keymap_to_json(&layout, &saved, err, sizeof err), SXCL_KEYMAP_OK, what);
        if (saved) {
            sxcl_keymap_layout again;
            (void)snprintf(what, sizeof what, "资产 %s: 往返后能再解析", file);
            check_int(sxcl_keymap_parse(saved, strlen(saved), &again, NULL, err, sizeof err),
                      SXCL_KEYMAP_OK, what);
            const char *why = "";
            (void)snprintf(what, sizeof what, "资产 %s: 往返后逐字段等价", file);
            check(controls_equal(&layout, &again, &why), what);
            if (why[0]) {
                printf("        (%s: %s)\n", file, why);
            }
            (void)snprintf(what, sizeof what, "资产 %s: 往返后 name/screen 不变", file);
            check(strcmp(layout.name, again.name) == 0 && strcmp(layout.screen, again.screen) == 0 &&
                      strcmp(layout.description, again.description) == 0, what);
            (void)snprintf(what, sizeof what, "资产 %s: 往返后 meta 不变", file);
            check(layout.meta_json && again.meta_json &&
                      strcmp(layout.meta_json, again.meta_json) == 0, what);
            sxcl_keymap_layout_free(&again);
            free(saved);
        }

        /* 内置预设必须与资产逐控件一致(同一份 presets.py 生成的两边) */
        sxcl_keymap_layout preset;
        (void)snprintf(what, sizeof what, "资产 %s: 能造出同名预设", file);
        check_int(sxcl_keymap_build_preset(want->preset, layout.screen, "", &preset), SXCL_KEYMAP_OK, what);
        const char *preset_why = "";
        (void)snprintf(what, sizeof what, "资产 %s: 预设与资产逐控件一致", file);
        check(controls_equal(&preset, &layout, &preset_why), what);
        if (preset_why[0]) {
            printf("        (%s: %s)\n", file, preset_why);
        }
        (void)snprintf(what, sizeof what, "资产 %s: 预设名字/描述一致", file);
        check(strcmp(preset.name, layout.name) == 0 && strcmp(preset.description, layout.description) == 0,
              what);
        sxcl_keymap_layout_free(&preset);
        sxcl_keymap_layout_free(&layout);
    }

#ifdef SXCL_ANDROID_KEYMAPS_DIR
    /* 内嵌文本与磁盘上的真实资产逐字节比对(资产改了就要重新生成 .inc) */
    for (size_t i = 0; i < k_keymap_asset_count; ++i) {
        char path[1024];
        (void)snprintf(path, sizeof path, "%s/%s", SXCL_ANDROID_KEYMAPS_DIR, k_keymap_assets[i].file);
        FILE *fh = sxcl_fs_fopen(path, "rb");
        char what[200];
        (void)snprintf(what, sizeof what, "漂移检查 %s: 能打开", k_keymap_assets[i].file);
        check(fh != NULL, what);
        if (!fh) {
            continue;
        }
        static char buf[262144];
        const size_t got = fread(buf, 1, sizeof buf - 1, fh);
        buf[got] = '\0';
        fclose(fh);
        (void)snprintf(what, sizeof what, "漂移检查 %s: 与内嵌文本一致", k_keymap_assets[i].file);
        check(text_equal_no_cr(buf, k_keymap_assets[i].text), what);
    }
#endif
}

/* ── 2) 四事件与非法项 ── */

static void test_events_and_validate(void)
{
    static const char *const text =
        "{\n"
        "  \"schema\": \"sxcl.keymap.v1\",\n"
        "  \"name\": \"事件测试\",\n"
        "  \"screen\": \"landscape\",\n"
        "  \"buttons\": [\n"
        "    { \"id\": \"all4\", \"label\": \"全事件\", \"x\": 0.5, \"y\": 0.5, \"w\": 0.1, \"h\": 0.1,\n"
        "      \"events\": {\n"
        "        \"press\":        { \"action\": \"jump\",  \"keys\": [\"KEY_SPACE\"], \"behavior\": \"hold\" },\n"
        "        \"long_press\":   { \"action\": \"chat\",  \"keys\": [\"KEY_T\"], \"behavior\": \"hold\" },\n"
        "        \"click\":        { \"action\": \"use\",   \"keys\": [\"MOUSE_RIGHT\"], \"behavior\": \"tap\" },\n"
        "        \"double_click\": { \"action\": \"sneak\", \"keys\": [\"KEY_LEFT_SHIFT\"], \"behavior\": \"toggle\" }\n"
        "      } },\n"
        "    { \"id\": \"onekey\", \"label\": \"字符串键\", \"x\": 0.1, \"y\": 0.1, \"w\": 0.05, \"h\": 0.05,\n"
        "      \"events\": { \"press\": { \"action\": \"inventory\", \"keys\": \"KEY_E\" } } }\n"
        "  ],\n"
        "  \"directions\": [\n"
        "    { \"id\": \"move\", \"x\": 0.03, \"y\": 0.6, \"w\": 0.2, \"h\": 0.3,\n"
        "      \"keys\": { \"up\": \"KEY_W\" } }\n"
        "  ]\n"
        "}";
    sxcl_keymap_layout layout;
    sxcl_keymap_issues issues;
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_parse(text, strlen(text), &layout, &issues, NULL, 0), SXCL_KEYMAP_OK,
              "事件: 解析成功");
    check_size(layout.button_count, 2, "事件: 两个按钮");
    check_size(layout.direction_count, 1, "事件: 一个方向控件");

    check_str(sxcl_keymap_event_id(SXCL_KEYMAP_EVENT_PRESS), "press", "事件 id: press");
    check_str(sxcl_keymap_event_id(SXCL_KEYMAP_EVENT_LONG_PRESS), "long_press", "事件 id: long_press");
    check_str(sxcl_keymap_event_id(SXCL_KEYMAP_EVENT_CLICK), "click", "事件 id: click");
    check_str(sxcl_keymap_event_id(SXCL_KEYMAP_EVENT_DOUBLE_CLICK), "double_click", "事件 id: double_click");
    check_str(sxcl_keymap_event_label(SXCL_KEYMAP_EVENT_PRESS), "按下", "事件中文: 按下");
    check_str(sxcl_keymap_event_label(SXCL_KEYMAP_EVENT_LONG_PRESS), "长按", "事件中文: 长按");
    check_str(sxcl_keymap_event_label(SXCL_KEYMAP_EVENT_CLICK), "单击", "事件中文: 单击");
    check_str(sxcl_keymap_event_label(SXCL_KEYMAP_EVENT_DOUBLE_CLICK), "双击", "事件中文: 双击");
    check_int(sxcl_keymap_event_from_id("double_click"), 3, "事件反查");
    check_int(sxcl_keymap_event_from_id("nope"), -1, "事件反查: 认不出来给 -1");

    const sxcl_keymap_button *all4 = sxcl_keymap_button_by_id(&layout, "all4");
    check(all4 != NULL, "事件: 找到 all4");
    if (all4) {
        check_int((int)all4->event_mask, 0xF, "事件: 四个事件都绑上了");
        const sxcl_keymap_binding *press = sxcl_keymap_button_event(all4, SXCL_KEYMAP_EVENT_PRESS);
        const sxcl_keymap_binding *click = sxcl_keymap_button_event(all4, SXCL_KEYMAP_EVENT_CLICK);
        const sxcl_keymap_binding *dbl = sxcl_keymap_button_event(all4, SXCL_KEYMAP_EVENT_DOUBLE_CLICK);
        check(press && strcmp(press->action, "jump") == 0 && strcmp(press->keys[0], "KEY_SPACE") == 0 &&
                  strcmp(press->behavior, "hold") == 0, "事件: 按下=jump/hold");
        check(click && strcmp(click->behavior, "tap") == 0, "事件: 单击=tap");
        check(dbl && strcmp(dbl->behavior, "toggle") == 0, "事件: 双击=toggle");
        check(sxcl_keymap_button_event(all4, SXCL_KEYMAP_EVENT_COUNT) == NULL, "事件: 越界返回 NULL");
    }
    const sxcl_keymap_button *one = sxcl_keymap_button_by_id(&layout, "onekey");
    check(one && one->event_mask == 1u && strcmp(one->events[0].keys[0], "KEY_E") == 0,
          "事件: keys 写成一个字符串也认");
    const sxcl_keymap_direction *move = sxcl_keymap_direction_by_id(&layout, "move");
    check(move && strcmp(move->up, "KEY_W") == 0, "方向: keys.up 用的是写的值");
    check(move && strcmp(move->down, "KEY_S") == 0, "方向: 没写的方向用默认 KEY_S");
    check(move && strcmp(move->sprint_key, "KEY_LEFT_SHIFT") == 0, "方向: sprint_key 默认值");
    check(move && strcmp(move->style, "dpad_compact") == 0, "方向: style 默认值");

    size_t total_keys = 0;
    const char *keys[SXCL_KEYMAP_KEYS_MAX * SXCL_KEYMAP_EVENT_COUNT];
    if (all4) {
        total_keys = sxcl_keymap_button_keys(all4, keys, 32);
    }
    check_size(total_keys, 4, "事件: all4 用到 4 个不同的键");

    /* 非法项:坐标越界(被钳制)、shape/behavior 认不出来、事件名不认识、没有 id */
    static const char *const bad =
        "{\n"
        "  \"schema\": \"sxcl.keymap.v1\",\n"
        "  \"name\": \"非法项\",\n"
        "  \"screen\": \"sideways\",\n"
        "  \"buttons\": [\n"
        "    { \"id\": \"a\", \"x\": 1.5, \"y\": -0.2, \"w\": 0.5, \"h\": 0.5, \"shape\": \"triangle\",\n"
        "      \"events\": { \"press\": { \"action\": \"jump\", \"keys\": [\"KEY_SPACE\"], \"behavior\": \"squish\" },\n"
        "                    \"triple_click\": { \"action\": \"x\", \"keys\": [\"KEY_X\"] } } },\n"
        "    { \"id\": \"a\", \"x\": 0.1, \"y\": 0.1 }\n"
        "  ]\n"
        "}";
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_parse(bad, strlen(bad), &layout, &issues, NULL, 0), SXCL_KEYMAP_OK,
              "非法项: 依然能解析(不崩、不整份失败)");
    check_str(layout.screen, "landscape", "非法项: screen 被归一");
    check(layout.buttons[0].x <= 1.0 && layout.buttons[0].y >= 0.0, "非法项: 坐标被钳制");
    check_str(layout.buttons[0].shape, "round", "非法项: shape 被归一");
    check_str(layout.buttons[0].events[0].behavior, "hold", "非法项: behavior 被归一");
    size_t warnings = 0;
    for (size_t i = 0; i < issues.count; ++i) {
        if (issues.items[i].level == SXCL_KEYMAP_LEVEL_WARNING) {
            ++warnings;
        }
    }
    check(warnings >= 4, "非法项: 解析时把非法字段报出来(>=4 条告警)");

    /* 没有事件的按钮必须报 error */
    size_t errors = 0;
    sxcl_keymap_issues_reset(&issues);
    sxcl_keymap_validate(&layout, &issues);
    for (size_t i = 0; i < issues.count; ++i) {
        if (issues.items[i].level == SXCL_KEYMAP_LEVEL_ERROR) {
            ++errors;
        }
    }
    check(errors >= 3, "非法项: 校验报出 id 重复/超范围/没绑事件");
    check(sxcl_keymap_issues_has_error(&issues) == 1, "非法项: has_error 为真");
    sxcl_keymap_layout_free(&layout);

    /* 合法布局:校验干净 */
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_parse(text, strlen(text), &layout, &issues, NULL, 0), SXCL_KEYMAP_OK, "重解析");
    sxcl_keymap_issues_reset(&issues);
    check_size(sxcl_keymap_validate(&layout, &issues), 0, "合法布局: 校验 0 条");
    check_int(sxcl_keymap_issues_has_error(&issues), 0, "合法布局: has_error 为假");
    sxcl_keymap_layout_free(&layout);
}

/* ── 3) 冲突检测 ── */

static void test_conflicts(void)
{
    static const char *const text =
        "{\n"
        "  \"name\": \"冲突\",\n"
        "  \"buttons\": [\n"
        "    { \"id\": \"jump\", \"label\": \"跳\", \"x\": 0.8, \"y\": 0.6, \"w\": 0.1, \"h\": 0.1,\n"
        "      \"events\": { \"press\": { \"action\": \"jump\", \"keys\": [\"KEY_SPACE\"] } } },\n"
        "    { \"id\": \"mine\", \"label\": \"挖\", \"x\": 0.80, \"y\": 0.60, \"w\": 0.1, \"h\": 0.1,\n"
        "      \"events\": { \"press\": { \"action\": \"attack\", \"keys\": [\"KEY_SPACE\"] } } },\n"
        "    { \"id\": \"inventory\", \"label\": \"背包\", \"x\": 0.1, \"y\": 0.1, \"w\": 0.1, \"h\": 0.1,\n"
        "      \"events\": { \"press\": { \"action\": \"inventory\", \"keys\": [\"KEY_E\"] } } }\n"
        "  ]\n"
        "}";
    sxcl_keymap_layout layout;
    sxcl_keymap_issues issues;
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_parse(text, strlen(text), &layout, &issues, NULL, 0), SXCL_KEYMAP_OK,
              "冲突: 解析成功");
    sxcl_keymap_issues_reset(&issues);
    check_size(sxcl_keymap_conflicts(&layout, &issues), 3, "冲突: 抢键 + 完全重叠 + 缺「移动」error");
    int found_key = 0;
    int found_overlap = 0;
    for (size_t i = 0; i < issues.count; ++i) {
        if (strstr(issues.items[i].message, "会互相打架") != NULL) {
            found_key = 1;
        }
        if (strstr(issues.items[i].message, "位置重叠") != NULL) {
            found_overlap = 1;
        }
    }
    check(found_key == 1, "冲突: 报出抢键");
    check(found_overlap == 1, "冲突: 报出重叠");
    /* 三个关键动作里有 jump / inventory,少了 forward,但没有方向控件 -> 报 error */
    check_int(sxcl_keymap_issues_has_error(&issues), 1, "冲突: 缺「移动」是 error");
    sxcl_keymap_layout_free(&layout);

    /* 别名按钮(动作相同)不算抢键 */
    static const char *const alias =
        "{ \"name\": \"别名\", \"buttons\": ["
        "  { \"id\": \"place\", \"x\": 0.8, \"y\": 0.6, \"w\": 0.1, \"h\": 0.1,"
        "    \"events\": { \"press\": { \"action\": \"use\", \"keys\": [\"MOUSE_RIGHT\"] } } },"
        "  { \"id\": \"shield\", \"x\": 0.6, \"y\": 0.6, \"w\": 0.1, \"h\": 0.1, \"alias_of\": \"use\","
        "    \"events\": { \"press\": { \"action\": \"use\", \"keys\": [\"MOUSE_RIGHT\"] } } },"
        "  { \"id\": \"jump\", \"x\": 0.2, \"y\": 0.2, \"w\": 0.1, \"h\": 0.1,"
        "    \"events\": { \"press\": { \"action\": \"jump\", \"keys\": [\"KEY_SPACE\"] } } },"
        "  { \"id\": \"inventory\", \"x\": 0.4, \"y\": 0.4, \"w\": 0.1, \"h\": 0.1,"
        "    \"events\": { \"press\": { \"action\": \"inventory\", \"keys\": [\"KEY_E\"] } } }"
        "], \"directions\": [ { \"id\": \"move\" } ] }";
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_parse(alias, strlen(alias), &layout, &issues, NULL, 0), SXCL_KEYMAP_OK,
              "冲突: 别名布局解析");
    sxcl_keymap_issues_reset(&issues);
    check_size(sxcl_keymap_conflicts(&layout, &issues), 0, "冲突: 别名按钮 + 有方向控件 -> 0 条");
    check_size(sxcl_keymap_action_owners(&layout, "use", NULL, 0), 2, "动作索引: use 有两个控件");
    check_size(sxcl_keymap_action_owners(&layout, "forward", NULL, 0), 0,
               "动作索引: 方向控件按 up/down/left/right 记账(与安卓端 actionIndex 一致)");
    check_size(sxcl_keymap_action_owners(&layout, "up", NULL, 0), 1, "动作索引: up 归方向控件");
    sxcl_keymap_layout_free(&layout);
}

/* ── 4) 搜索 ── */

static void test_find(void)
{
    sxcl_keymap_layout layout;
    check_int(sxcl_keymap_build_preset("survival", "landscape", "1.20.1", &layout), SXCL_KEYMAP_OK,
              "搜索: 造一份生存预设");
    sxcl_keymap_hits hits;
    /* 摇杆的提示里也有「潜行」两个字,所以是 2 条;控件顺序是先方向后按钮 */
    check_size(sxcl_keymap_find(&layout, "潜行", &hits), 2, "搜索: 按标签找「潜行」");
    check_str(hits.ids[0], "move", "搜索: 先命中摇杆(它的提示里也有「潜行」)");
    check_str(hits.ids[1], "sneak", "搜索: 再命中潜行按钮");
    check(sxcl_keymap_find(&layout, "jump", &hits) >= 1, "搜索: 按动作名找 jump");
    check(sxcl_keymap_find(&layout, "KEY_LEFT_SHIFT", &hits) >= 1, "搜索: 按按键名找");
    check(sxcl_keymap_find(&layout, "E", &hits) >= 1, "搜索: 单字母 E 能命中(背包)");
    check_size(sxcl_keymap_find(&layout, "   ", &hits), 0, "搜索: 空白返回 0");
    check_size(sxcl_keymap_find(&layout, "绝对不存在的词", &hits), 0, "搜索: 没命中返回 0");
    check_size(sxcl_keymap_find(&layout, "", &hits), 0, "搜索: 空串返回 0");
    check_size(sxcl_keymap_control_count(&layout), 7, "控件总数(生存横屏 = 6 按钮 + 1 摇杆)");
    sxcl_keymap_layout_free(&layout);
}

/* ── 5) 预设 ── */

static void test_presets(void)
{
    check_size(sxcl_keymap_preset_count(), 5, "预设: 5 个");
    check_str(sxcl_keymap_preset_key(0), "minimal", "预设: 第 0 个是 minimal");
    check_str(sxcl_keymap_preset_key(4), "one_hand", "预设: 第 4 个是 one_hand");
    check_str(sxcl_keymap_preset_label(0), "极简（推荐新手）", "预设: 中文名");
    check_str(sxcl_keymap_preset_label(3), "对战", "预设: 中文名 2");
    check(sxcl_keymap_preset_key(99) == NULL, "预设: 越界返回 NULL");

    check_str(sxcl_keymap_preset_screen("one_hand", "landscape"), "portrait", "预设: 单手固定竖屏");
    check_str(sxcl_keymap_preset_screen("minimal", "portrait"), "portrait", "预设: 别的跟着走");
    check_str(sxcl_keymap_recommend_preset("", "landscape"), "minimal", "预设推荐: 没版本 -> 极简");
    check_str(sxcl_keymap_recommend_preset("1.20.1", "landscape"), "survival", "预设推荐: 有版本 -> 生存");
    check_str(sxcl_keymap_recommend_preset("1.20.1", "portrait"), "one_hand", "预设推荐: 竖屏 -> 单手");

    sxcl_keymap_layout layout;
    check_int(sxcl_keymap_build_preset("one_hand", "landscape", "1.21.1", &layout), SXCL_KEYMAP_OK,
              "预设: 造单手");
    check_str(layout.name, "单手", "预设: 名字");
    check_str(layout.screen, "portrait", "预设: 屏幕方向被强制成竖屏");
    check_str(layout.mc_version, "1.21.1", "预设: mc_version 传进去了");
    check_size(layout.button_count, 5, "预设: 单手 5 个按钮");
    check_int(sxcl_keymap_meta_bool(&layout, "builtin", 0), 1, "预设: meta.builtin=true");
    char buf[32];
    (void)sxcl_keymap_meta_string(&layout, "recommended_for", buf, sizeof buf);
    check_str(buf, "one_hand", "预设: meta.recommended_for");
    sxcl_keymap_layout_free(&layout);

    check_int(sxcl_keymap_build_preset("不认识的名字", "landscape", "", &layout), SXCL_KEYMAP_OK,
              "预设: 认不出的名字回落到极简");
    check_str(layout.name, "极简", "预设: 回落的是极简");
    check_size(layout.button_count, 5, "预设: 极简 5 个按钮(横屏)");
    sxcl_keymap_layout_free(&layout);

    /* 建造横屏有 9 格快捷栏 */
    check_int(sxcl_keymap_build_preset("building", "landscape", "", &layout), SXCL_KEYMAP_OK,
              "预设: 造建造");
    const sxcl_keymap_button *slot9 = sxcl_keymap_button_by_id(&layout, "slot9");
    check(slot9 != NULL, "预设: 建造横屏有 slot9");
    if (slot9) {
        check_str(slot9->events[0].keys[0], "KEY_9", "预设: slot9 绑 KEY_9");
        check_str(slot9->shape, "square", "预设: 快捷栏是方的");
        check_near(slot9->opacity, 0.45, "预设: 快捷栏透明度 0.45");
    }
    const sxcl_keymap_button *fly = sxcl_keymap_button_by_id(&layout, "fly_up");
    check(fly && strcmp(fly->alias_of, "jump") == 0, "预设: fly_up 是 jump 的别名");
    check(fly && fly->event_mask == 9u, "预设: fly_up 按下(bit0)+ 双击(bit3)都绑了");
    sxcl_keymap_layout_free(&layout);

    /* 克隆 */
    sxcl_keymap_layout src_layout;
    check_int(sxcl_keymap_build_preset("pvp", "portrait", "1.20.1", &src_layout), SXCL_KEYMAP_OK,
              "克隆: 造对战");
    sxcl_keymap_layout copy;
    check_int(sxcl_keymap_clone(&src_layout, &copy), SXCL_KEYMAP_OK, "克隆: 成功");
    const char *why = "";
    check(controls_equal(&src_layout, &copy, &why), "克隆: 逐字段等价");
    check(strcmp(src_layout.meta_json ? src_layout.meta_json : "", copy.meta_json ? copy.meta_json : "") == 0,
          "克隆: meta 也拷了");
    sxcl_keymap_layout_free(&src_layout);
    sxcl_keymap_layout_free(&copy);
}

/* ── 6) FCL 互转 ── */

static void test_fcl(void)
{
    char buf[64];
    check_str(sxcl_keymap_key_from_glfw(32, buf, sizeof buf), "KEY_SPACE", "FCL: 32 -> KEY_SPACE");
    check_str(sxcl_keymap_key_from_glfw(-1, buf, sizeof buf), "MOUSE_LEFT", "FCL: -1 -> MOUSE_LEFT");
    check_str(sxcl_keymap_key_from_glfw(-2, buf, sizeof buf), "MOUSE_RIGHT", "FCL: -2 -> MOUSE_RIGHT");
    check_str(sxcl_keymap_key_from_glfw(340, buf, sizeof buf), "KEY_LEFT_SHIFT", "FCL: 340 -> KEY_LEFT_SHIFT");
    check_str(sxcl_keymap_key_from_glfw(999, buf, sizeof buf), "KEY_999", "FCL: 认不出的码给 KEY_<码>");
    check_str(sxcl_keymap_key_from_text("空格", buf, sizeof buf), "KEY_SPACE", "FCL: 中文「空格」");
    check_str(sxcl_keymap_key_from_text("右键", buf, sizeof buf), "MOUSE_RIGHT", "FCL: 中文「右键」");
    check_str(sxcl_keymap_key_from_text("e", buf, sizeof buf), "KEY_E", "FCL: 字母 e");
    check_str(sxcl_keymap_key_from_text("65", buf, sizeof buf), "KEY_A", "FCL: 数字 65 -> KEY_A");
    check_str(sxcl_keymap_key_from_text("key_f5", buf, sizeof buf), "KEY_F5", "FCL: 小写 key_f5 -> 大写");
    check_str(sxcl_keymap_key_from_text("", buf, sizeof buf), "", "FCL: 空串 -> 空");
    check_str(sxcl_keymap_action_for_key("KEY_SPACE"), "jump", "FCL: 键 -> 动作");
    check_str(sxcl_keymap_action_for_key("MOUSE_LEFT"), "attack", "FCL: 键 -> 动作 2");
    check_str(sxcl_keymap_action_for_key("KEY_UNKNOWN"), "", "FCL: 没有对应动作给空");

    /* 导入:像素坐标 + GLFW 键码 */
    static const char *const fcl =
        "{\n"
        "  \"name\": \"我的 FCL 布局\",\n"
        "  \"views\": [\n"
        "    { \"type\": \"direction\", \"style\": \"rocker\", \"text\": \"移动\",\n"
        "      \"x\": 72, \"y\": 626, \"width\": 480, \"height\": 389, \"keycodes\": [87, 83, 65, 68] },\n"
        "    { \"type\": \"button\", \"text\": \"跳\", \"x\": 2064, \"y\": 670, \"width\": 216, \"height\": 173,\n"
        "      \"alpha\": 0.6, \"keycodes\": [32] },\n"
        "    { \"type\": \"button\", \"text\": \"挖\", \"x\": 1776, \"y\": 799, \"width\": 240, \"height\": 194,\n"
        "      \"keycodes\": [-1], \"longPressKeycodes\": [84], \"doubleClickKeycodes\": [] },\n"
        "    { \"type\": \"squareButton\", \"id\": \"custom\", \"text\": \"盾\", \"x\": 0.5, \"y\": 0.5,\n"
        "      \"width\": 0.1, \"height\": 0.1, \"keycodes\": [\"MOUSE_RIGHT\"] }\n"
        "  ]\n"
        "}";
    sxcl_keymap_layout layout;
    sxcl_keymap_issues issues;
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_import_fcl(fcl, strlen(fcl), NULL, 2400, 1080, &layout, &issues, NULL, 0),
              SXCL_KEYMAP_OK, "FCL: 导入成功");
    check_str(layout.name, "我的 FCL 布局", "FCL: 名字从 name 里取");
    check_str(layout.screen, "landscape", "FCL: 屏幕方向按 2400x1080");
    check_size(sxcl_keymap_control_count(&layout), 4, "FCL: 4 个控件");
    check_size(layout.direction_count, 1, "FCL: 1 个方向控件");
    const sxcl_keymap_direction *move = sxcl_keymap_direction_by_id(&layout, "move0");
    check(move != NULL, "FCL: 没写 id 的方向控件按 move<序号> 命名");
    if (move) {
        /* fcl.py 只看**视图类型**决定 style:类型是 "direction" 就给 dpad_compact,
         * 视图里那个显式的 "style": "rocker" 它不认(C 版原来多认了这个字段,
         * 为了一致性已改成与 fcl.py 相同;要 rocker 请把类型写成 joystick/rocker)。 */
        check_str(move->style, "dpad_compact", "FCL: direction -> dpad_compact(与 fcl.py 一致)");
        check_near(move->x, 72.0 / 2400.0, "FCL: 方向 x 归一化");
        check_near(move->w, 480.0 / 2400.0, "FCL: 方向 w 归一化");
        check_str(move->up, "KEY_W", "FCL: 方向键码 87 -> KEY_W");
        check_str(move->right, "KEY_D", "FCL: 方向键码 68 -> KEY_D");
    }
    const sxcl_keymap_button *jump = NULL;
    const sxcl_keymap_button *mine = NULL;
    const sxcl_keymap_button *shield = NULL;
    for (size_t i = 0; i < layout.button_count; ++i) {
        if (strcmp(layout.buttons[i].id, "fcl1") == 0) { jump = &layout.buttons[i]; }
        if (strcmp(layout.buttons[i].id, "fcl2") == 0) { mine = &layout.buttons[i]; }
        if (strcmp(layout.buttons[i].id, "custom") == 0) { shield = &layout.buttons[i]; }
    }
    check(jump != NULL, "FCL: 没写 id 的按钮按 fcl<序号> 命名");
    if (jump) {
        check_str(jump->events[0].action, "jump", "FCL: 32 -> KEY_SPACE -> 动作 jump");
        check_str(jump->events[0].keys[0], "KEY_SPACE", "FCL: 键码 32 存成 KEY_SPACE");
        check_near(jump->opacity, 0.6, "FCL: alpha 读进来了");
        check_near(jump->x, 2064.0 / 2400.0, "FCL: 像素 x 归一化");
    }
    if (mine) {
        check_str(mine->events[0].action, "attack", "FCL: -1 -> MOUSE_LEFT -> attack");
        check_int((int)mine->event_mask, 3,
                  "FCL: 按下 + 长按两个事件,空的 doubleClickKeycodes 不建事件");
    }
    check(shield != NULL, "FCL: 归一化坐标(0.5)不会被再除一次");
    if (shield) {
        check_near(shield->x, 0.5, "FCL: 已归一化的 x 保持");
        check_str(shield->shape, "square", "FCL: squareButton -> shape=square");
        check_str(shield->events[0].keys[0], "MOUSE_RIGHT", "FCL: 字符串按键写法");
    }
    check(layout.meta_json != NULL && strstr(layout.meta_json, "fcl_raw") != NULL,
          "FCL: 原始数据塞进 meta.fcl_raw");

    /* 导出:像素坐标 + FCL 风格字段 */
    char *out = NULL;
    check_int(sxcl_keymap_to_fcl(&layout, 2400, 1080, &out, NULL, 0), SXCL_KEYMAP_OK, "FCL: 导出成功");
    if (out) {
        check(strstr(out, "\"views\"") != NULL, "FCL: 导出里有 views");
        check(strstr(out, "\"screenWidth\": 2400") != NULL, "FCL: 导出里有 screenWidth");
        check(strstr(out, "\"longPressKeycodes\"") != NULL, "FCL: 导出里有长按键码");
        check(strstr(out, "\"type\": \"direction\"") != NULL, "FCL: 导出里有方向控件");
        check(strstr(out, "2064") != NULL, "FCL: 导出回像素坐标(2064)");
        free(out);
    }
    sxcl_keymap_layout_free(&layout);

    /* 坏输入 */
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_import_fcl("不是 JSON", strlen("不是 JSON"), NULL, 2400, 1080, &layout, &issues,
                                     NULL, 0), SXCL_KEYMAP_ERR_FORMAT, "FCL: 坏 JSON 返回 FORMAT");
    check_int(sxcl_keymap_import_fcl("42", 2, NULL, 2400, 1080, &layout, &issues, NULL, 0),
              SXCL_KEYMAP_ERR_FORMAT, "FCL: 既不是对象也不是数组 -> FORMAT");
    /* 数组形态(fcl.py 也认) */
    static const char *const arr = "[ { \"type\": \"button\", \"text\": \"跳\", \"x\": 1200, \"y\": 540,"
                                    " \"width\": 120, \"height\": 120, \"keycodes\": [32] } ]";
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_import_fcl(arr, strlen(arr), "数组形态", 2400, 1080, &layout, &issues, NULL, 0),
              SXCL_KEYMAP_OK, "FCL: 数组形态也能导入");
    check_str(layout.name, "数组形态", "FCL: 显式名字优先");
    check_size(layout.button_count, 1, "FCL: 数组形态 1 个按钮");
    sxcl_keymap_layout_free(&layout);
}

/* ── 7) 存取文件 ── */

static void test_file_io(void)
{
    sxcl_keymap_layout layout;
    check_int(sxcl_keymap_build_preset("building", "portrait", "1.21.1", &layout), SXCL_KEYMAP_OK,
              "存取: 造建造(竖屏)");
    const char *path = "keymap_test_out/my-layout.json";
    char err[160];
    check_int(sxcl_keymap_save_file(&layout, path, err, sizeof err), SXCL_KEYMAP_OK, "存取: 保存成功");
    sxcl_keymap_layout loaded;
    sxcl_keymap_issues issues;
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_load_file(path, &loaded, &issues, err, sizeof err), SXCL_KEYMAP_OK,
              "存取: 读回来成功");
    const char *why = "";
    check(controls_equal(&layout, &loaded, &why), "存取: 读回来逐字段等价");
    if (why[0]) {
        printf("        (%s)\n", why);
    }
    check_str(loaded.screen, "portrait", "存取: 屏幕方向");
    check_str(loaded.mc_version, "1.21.1", "存取: mc_version");
    sxcl_keymap_layout_free(&loaded);
    sxcl_keymap_layout_free(&layout);

    sxcl_keymap_layout missing;
    check_int(sxcl_keymap_load_file("keymap_test_out/不存在.json", &missing, NULL, err, sizeof err),
              SXCL_KEYMAP_ERR_IO, "存取: 文件不存在返回 IO");
    check_int(sxcl_keymap_save_file(NULL, path, err, sizeof err), SXCL_KEYMAP_ERR_ARG, "存取: 空指针返回 ARG");
}

/* ── 8) 坏输入 ── */

static void test_bad_input(void)
{
    sxcl_keymap_layout layout;
    char err[160];
    check_int(sxcl_keymap_parse(NULL, 0, &layout, NULL, err, sizeof err), SXCL_KEYMAP_ERR_ARG,
              "坏输入: NULL");
    check_int(sxcl_keymap_parse("", 0, &layout, NULL, err, sizeof err), SXCL_KEYMAP_ERR_FORMAT,
              "坏输入: 空串");
    check_int(sxcl_keymap_parse("{不是 JSON", strlen("{不是 JSON"), &layout, NULL, err, sizeof err),
              SXCL_KEYMAP_ERR_FORMAT, "坏输入: 截断 JSON");
    check_int(sxcl_keymap_parse("[1,2,3]", 7, &layout, NULL, err, sizeof err), SXCL_KEYMAP_ERR_FORMAT,
              "坏输入: 顶层是数组");
    check_int(sxcl_keymap_parse("{}", 2, &layout, NULL, err, sizeof err), SXCL_KEYMAP_OK,
              "空对象: 能给一份空布局(全用默认值)");
    check_str(layout.name, "未命名", "空对象: 默认名字");
    check_str(layout.schema, "sxcl.keymap.v1", "空对象: 默认 schema");
    check_size(layout.button_count, 0, "空对象: 没有按钮");
    sxcl_keymap_layout_free(&layout);
    check_str(sxcl_keymap_event_id((sxcl_keymap_event)99), "", "坏输入: 事件 id 越界给空串");
    check_str(sxcl_keymap_normalize_behavior(NULL, NULL), "hold", "坏输入: behavior NULL -> hold");
    check_str(sxcl_keymap_normalize_shape("pill", NULL), "pill", "坏输入: shape 合法值原样");
    check_str(sxcl_keymap_normalize_style("dpad", NULL), "dpad", "坏输入: style 合法值原样");
    check_str(sxcl_keymap_normalize_screen("portrait", NULL), "portrait", "坏输入: screen 合法值原样");
    check_size(sxcl_keymap_control_count(NULL), 0, "坏输入: 空布局计数为 0");
    check(sxcl_keymap_button_by_id(NULL, "x") == NULL, "坏输入: 空布局查 id 返回 NULL");
    check(sxcl_keymap_find(NULL, "x", NULL) == 0, "坏输入: 空布局搜索 0");
    check_size(sxcl_keymap_action_owners(NULL, "jump", NULL, 0), 0, "坏输入: 空布局动作索引 0");
    check_int(sxcl_keymap_issues_has_error(NULL), 0, "坏输入: has_error(NULL) 为 0");
    sxcl_keymap_layout_free(NULL);
    sxcl_keymap_issues_reset(NULL);
    check(1, "坏输入: free/reset 传 NULL 不崩");
}

int main(void)
{
    test_android_assets();
    test_events_and_validate();
    test_conflicts();
    test_find();
    test_presets();
    test_fcl();
    test_file_io();
    test_bad_input();

    printf("键位映射核心测试: 通过 %d 失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
