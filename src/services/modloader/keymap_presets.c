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

/* 一个事件上的绑定:动作 + 一个键 + 行为(presets.py 里每个事件最多一个键)。 */
typedef struct bind_spec {
    const char *action;
    const char *key;
    const char *behavior;   /* hold / tap / toggle */
} bind_spec;

static const bind_spec B_JUMP = { "jump", "KEY_SPACE", "hold" };
static const bind_spec B_JUMP_TOGGLE = { "jump", "KEY_SPACE", "toggle" };
static const bind_spec B_ATTACK = { "attack", "MOUSE_LEFT", "hold" };
static const bind_spec B_USE = { "use", "MOUSE_RIGHT", "hold" };
static const bind_spec B_SNEAK = { "sneak", "KEY_LEFT_SHIFT", "hold" };
static const bind_spec B_SNEAK_TOGGLE = { "sneak", "KEY_LEFT_SHIFT", "toggle" };
static const bind_spec B_INVENTORY = { "inventory", "KEY_E", "hold" };
static const bind_spec B_CHAT = { "chat", "KEY_T", "hold" };
static const bind_spec B_DROP = { "drop", "KEY_Q", "hold" };
static const bind_spec B_SPRINT = { "sprint", "KEY_LEFT_CTRL", "hold" };
static const bind_spec B_SWAP = { "swap_offhand", "KEY_F", "hold" };
static const bind_spec B_PERSPECTIVE = { "perspective", "KEY_F5", "hold" };

static const char *const k_shift = "KEY_LEFT_SHIFT";
static const char *const k_move_keys[] = { "KEY_W", "KEY_S", "KEY_A", "KEY_D" };

static int add_button(sxcl_keymap_layout *layout, const char *id, const char *label,
                      double x, double y, double w, double h,
                      const bind_spec *press, const bind_spec *long_press,
                      const bind_spec *click, const bind_spec *double_click,
                      const char *hint, const char *group, const char *shape,
                      double opacity, const char *alias_of)
{
    sxcl_keymap_button button;
    memset(&button, 0, sizeof button);
    sxcl_kp_copy(button.id, sizeof button.id, id);
    sxcl_kp_copy(button.label, sizeof button.label, label);
    sxcl_kp_copy(button.hint, sizeof button.hint, hint ? hint : "");
    button.x = x;
    button.y = y;
    button.w = w;
    button.h = h;
    sxcl_kp_copy(button.shape, sizeof button.shape, shape ? shape : "round");
    button.opacity = opacity;
    sxcl_kp_copy(button.group, sizeof button.group, group ? group : "right");
    sxcl_kp_copy(button.alias_of, sizeof button.alias_of, alias_of ? alias_of : "");
    if (sxcl_kp_push_button(layout, &button) != SXCL_KEYMAP_OK) {
        return SXCL_KEYMAP_ERR_NOMEM;
    }
    const bind_spec *specs[SXCL_KEYMAP_EVENT_COUNT] = { press, long_press, click, double_click };
    for (size_t e = 0; e < SXCL_KEYMAP_EVENT_COUNT; ++e) {
        if (!specs[e]) {
            continue;
        }
        const char *key = specs[e]->key;
        if (sxcl_keymap_bind(layout, id, (sxcl_keymap_event)e, specs[e]->action,
                             specs[e]->behavior, &key, 1) != SXCL_KEYMAP_OK) {
            return SXCL_KEYMAP_ERR_ARG;
        }
    }
    return SXCL_KEYMAP_OK;
}

static int add_move(sxcl_keymap_layout *layout, const char *screen)
{
    sxcl_keymap_direction direction;
    memset(&direction, 0, sizeof direction);
    sxcl_kp_copy(direction.id, sizeof direction.id, "move");
    sxcl_kp_copy(direction.label, sizeof direction.label, "移动");
    sxcl_kp_copy(direction.hint, sizeof direction.hint, "推到底可以快走，配合潜行键=疾跑");
    if (strcmp(screen, "portrait") == 0) {
        direction.x = 0.06; direction.y = 0.62;
        direction.w = 0.34; direction.h = 0.22;
    } else {
        direction.x = 0.03; direction.y = 0.58;
        direction.w = 0.20; direction.h = 0.36;
    }
    sxcl_kp_copy(direction.style, sizeof direction.style, "rocker");
    direction.opacity = 0.5;
    direction.dead_zone = 0.18;
    sxcl_kp_copy(direction.group, sizeof direction.group, "left");
    sxcl_kp_copy(direction.up, sizeof direction.up, k_move_keys[0]);
    sxcl_kp_copy(direction.down, sizeof direction.down, k_move_keys[1]);
    sxcl_kp_copy(direction.left, sizeof direction.left, k_move_keys[2]);
    sxcl_kp_copy(direction.right, sizeof direction.right, k_move_keys[3]);
    sxcl_kp_copy(direction.sprint_key, sizeof direction.sprint_key, k_shift);
    return sxcl_kp_push_direction(layout, &direction);
}

/* 移动/跳跃/挖掘/放置 —— 任何预设都该有的四个。 */
static int add_core_buttons(sxcl_keymap_layout *layout, const char *screen)
{
    const int portrait = (strcmp(screen, "portrait") == 0);
    int rc;
    if (portrait) {
        rc = add_button(layout, "jump", "跳", 0.78, 0.66, 0.16, 0.09, &B_JUMP, NULL, NULL, NULL,
                        "空格键：跳跃。按住可以连续跳（跑酷常用）", "right", "round", 0.55, "");
        if (rc != SXCL_KEYMAP_OK) { return rc; }
        rc = add_button(layout, "mine", "挖", 0.60, 0.78, 0.16, 0.09, &B_ATTACK, NULL, NULL, NULL,
                        "左键：攻击 / 挖掘。按住就是一直挖，不用反复点", "right", "round", 0.55, "");
        if (rc != SXCL_KEYMAP_OK) { return rc; }
        rc = add_button(layout, "place", "放", 0.78, 0.78, 0.16, 0.09, &B_USE, NULL, NULL, NULL,
                        "右键：放置方块 / 使用物品 / 开门 / 喂动物", "right", "round", 0.55, "");
        if (rc != SXCL_KEYMAP_OK) { return rc; }
        return add_button(layout, "sneak", "潜行", 0.42, 0.78, 0.16, 0.09, &B_SNEAK, NULL, NULL,
                          &B_SNEAK_TOGGLE,
                          "Shift：潜行（不会掉下方块）。双击可以切换成常驻潜行", "right", "pill",
                          0.55, "");
    }
    rc = add_button(layout, "jump", "跳", 0.86, 0.62, 0.09, 0.16, &B_JUMP, NULL, NULL, NULL,
                    "空格键：跳跃。按住可以连续跳（跑酷常用）", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "mine", "挖", 0.74, 0.74, 0.10, 0.18, &B_ATTACK, NULL, NULL, NULL,
                    "左键：攻击 / 挖掘。按住就是一直挖，不用反复点", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "place", "放", 0.86, 0.74, 0.10, 0.18, &B_USE, NULL, NULL, NULL,
                    "右键：放置方块 / 使用物品 / 开门", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    return add_button(layout, "sneak", "潜行", 0.62, 0.80, 0.10, 0.10, &B_SNEAK, NULL, NULL,
                      &B_SNEAK_TOGGLE, "Shift：潜行。双击 = 常驻潜行（挂机搭桥很省手）", "right",
                      "pill", 0.55, "");
}

/* 快捷栏 1-9(横屏 9 个 / 竖屏 5 个)。 */
static int add_hotbar(sxcl_keymap_layout *layout, const char *screen)
{
    const int landscape = (strcmp(screen, "portrait") != 0);
    const int count = landscape ? 9 : 5;
    const double width = landscape ? 0.045 : 0.09;
    const double gap = 0.008;
    const double start_x = 0.5 - ((double)count * (width + gap) - gap) / 2.0;
    const double y = landscape ? 0.94 : 0.45;
    const double h = landscape ? 0.05 : 0.06;
    for (int i = 0; i < count; ++i) {
        char id[16];
        char label[8];
        char key[16];
        char hint[64];
        (void)snprintf(id, sizeof id, "slot%d", i + 1);
        (void)snprintf(label, sizeof label, "%d", i + 1);
        (void)snprintf(key, sizeof key, "KEY_%d", i + 1);
        (void)snprintf(hint, sizeof hint, "快捷栏第 %d 格（键盘 %d）", i + 1, i + 1);
        char action[SXCL_KEYMAP_ACTION_MAX];
        (void)snprintf(action, sizeof action, "hotbar_%d", i + 1);
        const bind_spec spec = { action, key, "hold" };
        const int rc = add_button(layout, id, label, start_x + (double)i * (width + gap), y, width, h,
                                  &spec, NULL, NULL, NULL, hint, "center", "square", 0.45, "");
        if (rc != SXCL_KEYMAP_OK) {
            return rc;
        }
    }
    return SXCL_KEYMAP_OK;
}

static int set_meta(sxcl_keymap_layout *layout, const char *recommended_for)
{
    char meta[192];
    (void)snprintf(meta, sizeof meta,
                   "{\n    \"builtin\": true,\n    \"recommended_for\": \"%s\"\n  }",
                   recommended_for);
    free(layout->meta_json);
    layout->meta_json = (char *)malloc(strlen(meta) + 1);
    if (!layout->meta_json) {
        return SXCL_KEYMAP_ERR_NOMEM;
    }
    memcpy(layout->meta_json, meta, strlen(meta) + 1);
    return SXCL_KEYMAP_OK;
}

static int build_minimal(sxcl_keymap_layout *layout, const char *screen)
{
    int rc = add_core_buttons(layout, screen);
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "inventory", "背包", 0.62, 0.62, 0.09, 0.14, &B_INVENTORY, NULL, NULL,
                    NULL, "E：背包", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = set_meta(layout, "newbie");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    return add_move(layout, screen);
}

static int build_survival(sxcl_keymap_layout *layout, const char *screen)
{
    int rc = add_core_buttons(layout, screen);
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "inventory", "背包", 0.62, 0.60, 0.09, 0.14, &B_INVENTORY, &B_CHAT, NULL,
                    NULL, "E：打开背包。长按 = 打开聊天（发消息）", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "drop", "丢弃", 0.50, 0.68, 0.09, 0.14, &B_DROP, NULL, NULL, NULL,
                    "Q：丢掉手上物品（小心别把钻石扔了）", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = set_meta(layout, "survival");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    return add_move(layout, screen);
}

static int build_building(sxcl_keymap_layout *layout, const char *screen)
{
    int rc = add_core_buttons(layout, screen);
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_hotbar(layout, screen);
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "inventory", "背包", 0.62, 0.60, 0.09, 0.14, &B_INVENTORY, NULL, NULL,
                    NULL, "E：背包；长按 = 聊天", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "fly_up", "升", 0.50, 0.44, 0.08, 0.10, &B_JUMP, NULL, NULL,
                    &B_JUMP_TOGGLE, "创造模式飞行上升（双击空格开始/停止飞行）", "center", "round",
                    0.55, "jump");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "fly_down", "降", 0.50, 0.56, 0.08, 0.10, &B_SNEAK, NULL, NULL, NULL,
                    "创造模式飞行下降", "center", "round", 0.55, "sneak");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "sprint", "疾跑", 0.30, 0.86, 0.10, 0.09, &B_SPRINT, NULL, NULL, NULL,
                    "Ctrl：疾跑（也可以双击前进）", "left", "pill", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = set_meta(layout, "building");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    return add_move(layout, screen);
}

static int build_pvp(sxcl_keymap_layout *layout, const char *screen)
{
    int rc = add_core_buttons(layout, screen);
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "sprint", "疾跑", 0.30, 0.86, 0.10, 0.09, &B_SPRINT, NULL, NULL, NULL,
                    "Ctrl 疾跑：追击/逃跑必备", "left", "pill", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "swap", "副手", 0.74, 0.62, 0.08, 0.10, &B_SWAP, NULL, NULL, NULL,
                    "F：把主手物品换到副手（盾牌/火把常用）", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "shield", "盾", 0.62, 0.72, 0.09, 0.14, &B_USE, NULL, NULL, NULL,
                    "右键举盾（副手放盾牌）", "right", "round", 0.55, "use");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "inventory", "背包", 0.62, 0.58, 0.09, 0.12, &B_INVENTORY, NULL, NULL,
                    NULL, "E：背包", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "perspective", "视角", 0.50, 0.90, 0.10, 0.07, &B_PERSPECTIVE, NULL, NULL,
                    NULL, "F5：切换第一/第三人称（PVP 看背后很有用）", "center", "pill", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = set_meta(layout, "pvp");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    return add_move(layout, screen);
}

/* 单手:全部塞到右下角,适合站着刷东西。它固定竖屏。 */
static int build_one_hand(sxcl_keymap_layout *layout)
{
    int rc = add_button(layout, "jump", "跳", 0.86, 0.84, 0.11, 0.12, &B_JUMP, NULL, NULL, NULL,
                        "空格：跳", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "mine", "挖", 0.72, 0.84, 0.11, 0.12, &B_ATTACK, NULL, NULL, NULL,
                    "左键：挖/打（按住连续）", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "place", "放", 0.86, 0.68, 0.11, 0.12, &B_USE, NULL, NULL, NULL,
                    "右键：放方块/使用", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "sneak", "潜行", 0.72, 0.68, 0.11, 0.12, &B_SNEAK, NULL, NULL, NULL,
                    "Shift：潜行", "right", "pill", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = add_button(layout, "inventory", "背包", 0.86, 0.52, 0.11, 0.12, &B_INVENTORY, NULL, NULL,
                    NULL, "E：背包", "right", "round", 0.55, "");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    rc = set_meta(layout, "one_hand");
    if (rc != SXCL_KEYMAP_OK) { return rc; }
    return add_move(layout, "portrait");
}

/* ── 对外 ── */

static const char *const k_keys[] = { "minimal", "survival", "building", "pvp", "one_hand" };
static const char *const k_labels[] = { "极简（推荐新手）", "生存", "建造", "对战", "单手" };
static const char *const k_names[] = { "极简", "生存", "建造", "对战", "单手" };
static const char *const k_descriptions[] = {
    "只留四个必用键，屏幕最干净（新手先用这套）",
    "探索/挖矿/打怪用的一套完整布局",
    "搭建筑/红石用：带快捷栏 1-9 与飞行上下",
    "PVP 向：疾跑、副手、盾牌、切视角都放手指边",
    "单手模式：所有按键集中在右下，走路用触摸板",
};

size_t sxcl_keymap_preset_count(void)
{
    return sizeof k_keys / sizeof k_keys[0];
}

const char *sxcl_keymap_preset_key(size_t index)
{
    return (index < sxcl_keymap_preset_count()) ? k_keys[index] : NULL;
}

const char *sxcl_keymap_preset_label(size_t index)
{
    return (index < sxcl_keymap_preset_count()) ? k_labels[index] : NULL;
}

const char *sxcl_keymap_preset_screen(const char *key, const char *screen)
{
    if (key && strcmp(key, "one_hand") == 0) {
        return "portrait";   /* 单手模式天生竖屏,请求别的也不改 */
    }
    return (screen && strcmp(screen, "portrait") == 0) ? "portrait" : "landscape";
}

const char *sxcl_keymap_recommend_preset(const char *mc_version, const char *screen)
{
    if (screen && strcmp(screen, "portrait") == 0) {
        return "one_hand";
    }
    return (mc_version && *mc_version) ? "survival" : "minimal";
}

int sxcl_keymap_build_preset(const char *key, const char *screen, const char *mc_version,
                             sxcl_keymap_layout *out)
{
    if (!out) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    size_t index = 0;
    int found = 0;
    for (size_t i = 0; i < sxcl_keymap_preset_count(); ++i) {
        if (key && strcmp(key, k_keys[i]) == 0) {
            index = i;
            found = 1;
        }
    }
    if (!found) {
        index = 0;   /* 认不出的名字回落到 minimal(presets.py 的行为) */
    }
    const char *use_screen = sxcl_keymap_preset_screen(k_keys[index], screen);

    sxcl_keymap_layout_init(out);
    sxcl_kp_copy(out->name, sizeof out->name, k_names[index]);
    sxcl_kp_copy(out->screen, sizeof out->screen, use_screen);
    sxcl_kp_copy(out->description, sizeof out->description, k_descriptions[index]);
    sxcl_kp_copy(out->mc_version, sizeof out->mc_version, mc_version ? mc_version : "");

    int rc;
    switch (index) {
    case 1: rc = build_survival(out, use_screen); break;
    case 2: rc = build_building(out, use_screen); break;
    case 3: rc = build_pvp(out, use_screen); break;
    case 4: rc = build_one_hand(out); break;
    case 0:
    default: rc = build_minimal(out, use_screen); break;
    }
    if (rc != SXCL_KEYMAP_OK) {
        sxcl_keymap_layout_free(out);
        return rc;
    }
    return SXCL_KEYMAP_OK;
}
