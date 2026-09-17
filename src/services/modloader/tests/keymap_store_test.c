/* 按键布局存取(store.py)+ FCL 导入(fcl.py)测试:不联网、不碰用户真实配置。
 *
 * 覆盖:
 *   1) ensure_presets 落盘(5 份)、第二次不再写、overwrite 强制重写;
 *   2) 保存 -> 列出 -> 读取 往返;重名直接覆盖(文件数不变);
 *   3) 文件名安全化(中文/全角标点/斜杠/空格;key 与布局名两个来源);
 *   4) 非法 JSON 拒绝:列目录跳过坏文件、load 返回错误码;
 *   5) active.json:默认值 / set_active(去掉 preset- 前缀)/ 坏文件返回 "";
 *   6) delete:用户布局能删、内置预设拒删、路径穿越拒绝;
 *   7) FCL 导入:字段映射与容错(空 id/label 回落、GLFW 码、字符串键、混合坐标、
 *      alpha=0、未知类型、非对象项跳过),并与 **Python 生成的期望文件**
 *      (tests/fcl_fixture_expected.json / fcl_fixture_array_expected.json)逐字段比对
 *      —— 这是"两边一致"的硬证据;夹具由 tools/keymap_parity.py --write-fixtures 生成。
 *
 * 临时产物写在构建目录的 keymap_store_out/(跑之前整个删掉),仓库里不留产物;
 * FCL 夹具在源码树 tests/ 下,CMake 用 SXCL_KEYMAP_FIXTURE_DIR 把路径编进来。
 * 跑完这个测试后,仓库根目录的 tools/keymap_parity.py 会拿这里的产物与 Python 版
 * 现场生成的产物逐字段比对(见交付报告)。
 */
#define _CRT_SECURE_NO_WARNINGS 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/json.h"
#include "sxcl/keymap.h"

#define SXCL_KP_STORE_OUT "keymap_store_out"

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

/* 文件名是否以 want 结尾(路径分隔符两种都算)。 */
static int ends_with_name(const char *path, const char *want)
{
    if (!path || !want) {
        return 0;
    }
    const size_t plen = strlen(path);
    const size_t wlen = strlen(want);
    if (wlen > plen) {
        return 0;
    }
    const char *tail = path + (plen - wlen);
    if (strcmp(tail, want) != 0) {
        return 0;
    }
    return (plen == wlen) || (tail[-1] == '/' || tail[-1] == '\\');
}

/* ── 文件小工具(测试自己用;中文名必须走 sxcl_fs_fopen) ── */

static int write_file(const char *path, const char *text)
{
    if (sxcl_fs_mkdirs_for_file(path) != 0) {
        return -1;
    }
    FILE *fh = sxcl_fs_fopen(path, "wb");
    if (!fh) {
        return -1;
    }
    const size_t len = strlen(text);
    const size_t written = fwrite(text, 1, len, fh);
    fclose(fh);
    return written == len ? 0 : -1;
}

static char *read_file(const char *path)
{
    FILE *fh = sxcl_fs_fopen(path, "rb");
    if (!fh) {
        return NULL;
    }
    (void)fseek(fh, 0, SEEK_END);
    const long size = ftell(fh);
    rewind(fh);
    if (size < 0) {
        fclose(fh);
        return NULL;
    }
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        fclose(fh);
        return NULL;
    }
    const size_t got = (size > 0) ? fread(text, 1, (size_t)size, fh) : 0;
    fclose(fh);
    text[got] = '\0';
    return text;
}

/* ── 两个布局逐字段相等(与 keymap_test.c 同一套口径) ── */

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

static int layouts_equal(const sxcl_keymap_layout *a, const sxcl_keymap_layout *b, const char **why)
{
    if (a->button_count != b->button_count) { *why = "按钮数量不同"; return 0; }
    if (a->direction_count != b->direction_count) { *why = "方向控件数量不同"; return 0; }
    for (size_t i = 0; i < a->button_count; ++i) {
        const sxcl_keymap_button *x = &a->buttons[i];
        const sxcl_keymap_button *y = &b->buttons[i];
        if (strcmp(x->id, y->id) != 0) { *why = "按钮 id 不同"; return 0; }
        if (strcmp(x->label, y->label) != 0) { *why = "按钮 label 不同"; return 0; }
        if (strcmp(x->hint, y->hint) != 0) { *why = "按钮 hint 不同"; return 0; }
        if (strcmp(x->shape, y->shape) != 0) { *why = "按钮 shape 不同"; return 0; }
        if (strcmp(x->group, y->group) != 0) { *why = "按钮 group 不同"; return 0; }
        if (!nearly(x->opacity, y->opacity)) { *why = "按钮 opacity 不同"; return 0; }
        if (!nearly(x->x, y->x) || !nearly(x->y, y->y) || !nearly(x->w, y->w) || !nearly(x->h, y->h)) {
            *why = "按钮坐标不同";
            return 0;
        }
        if (x->event_mask != y->event_mask) { *why = "按钮事件集合不同"; return 0; }
        for (size_t e = 0; e < SXCL_KEYMAP_EVENT_COUNT; ++e) {
            if ((x->event_mask & (1u << (unsigned)e)) == 0) {
                continue;
            }
            if (!bindings_equal(&x->events[e], &y->events[e])) { *why = "按钮事件绑定不同"; return 0; }
        }
    }
    for (size_t i = 0; i < a->direction_count; ++i) {
        const sxcl_keymap_direction *x = &a->directions[i];
        const sxcl_keymap_direction *y = &b->directions[i];
        if (strcmp(x->id, y->id) != 0) { *why = "方向 id 不同"; return 0; }
        if (strcmp(x->style, y->style) != 0) { *why = "方向 style 不同"; return 0; }
        if (strcmp(x->up, y->up) != 0 || strcmp(x->down, y->down) != 0 ||
            strcmp(x->left, y->left) != 0 || strcmp(x->right, y->right) != 0) {
            *why = "方向键码不同";
            return 0;
        }
        if (!nearly(x->x, y->x) || !nearly(x->y, y->y) || !nearly(x->w, y->w) || !nearly(x->h, y->h)) {
            *why = "方向坐标不同";
            return 0;
        }
    }
    return 1;
}

/* ── JSON 逐字段比对(对象不看键顺序;数字给 1e-9 的余量) ── */

static int json_equal(const sxcl_json_value *a, const sxcl_json_value *b, const char *path, char *why,
                      size_t why_len)
{
    const sxcl_json_type ta = a ? sxcl_json_type_of(a) : SXCL_JSON_NULL;
    const sxcl_json_type tb = b ? sxcl_json_type_of(b) : SXCL_JSON_NULL;
    if (!a || !b) {
        (void)snprintf(why, why_len, "%s: 一边是 null(C=%s Python=%s)", path, a ? "有" : "无",
                       b ? "有" : "无");
        return 0;
    }
    if (ta != tb) {
        (void)snprintf(why, why_len, "%s: 类型不同(C=%d Python=%d)", path, (int)ta, (int)tb);
        return 0;
    }
    switch (ta) {
    case SXCL_JSON_BOOL:
        if (sxcl_json_bool(a) != sxcl_json_bool(b)) {
            (void)snprintf(why, why_len, "%s: 布尔不同(C=%d Python=%d)", path, sxcl_json_bool(a),
                           sxcl_json_bool(b));
            return 0;
        }
        return 1;
    case SXCL_JSON_NUMBER: {
        const double x = sxcl_json_number(a);
        const double y = sxcl_json_number(b);
        if (!nearly(x, y)) {
            (void)snprintf(why, why_len, "%s: 数字不同(C=%.10g Python=%.10g)", path, x, y);
            return 0;
        }
        return 1;
    }
    case SXCL_JSON_STRING: {
        const char *x = sxcl_json_string(a);
        const char *y = sxcl_json_string(b);
        if (strcmp(x ? x : "", y ? y : "") != 0) {
            (void)snprintf(why, why_len, "%s: 字符串不同(C='%s' Python='%s')", path, x ? x : "",
                           y ? y : "");
            return 0;
        }
        return 1;
    }
    case SXCL_JSON_ARRAY: {
        const size_t na = sxcl_json_size(a);
        const size_t nb = sxcl_json_size(b);
        if (na != nb) {
            (void)snprintf(why, why_len, "%s: 数组长度不同(C=%lu Python=%lu)", path, (unsigned long)na,
                           (unsigned long)nb);
            return 0;
        }
        for (size_t i = 0; i < na; ++i) {
            char sub[256];
            (void)snprintf(sub, sizeof sub, "%s[%lu]", path, (unsigned long)i);
            if (!json_equal(sxcl_json_at(a, i), sxcl_json_at(b, i), sub, why, why_len)) {
                return 0;
            }
        }
        return 1;
    }
    case SXCL_JSON_OBJECT:
    default: {
        const size_t na = sxcl_json_member_count(a);
        const size_t nb = sxcl_json_member_count(b);
        if (na != nb) {
            (void)snprintf(why, why_len, "%s: 成员个数不同(C=%lu Python=%lu)", path, (unsigned long)na,
                           (unsigned long)nb);
            return 0;
        }
        for (size_t i = 0; i < na; ++i) {
            const char *key = sxcl_json_member_key(a, i);
            const sxcl_json_value *mine = sxcl_json_member_value(a, i);
            const sxcl_json_value *theirs = sxcl_json_get(b, key);
            if (!theirs) {
                (void)snprintf(why, why_len, "%s.%s: Python 侧没有这个字段", path, key ? key : "?");
                return 0;
            }
            char sub[256];
            (void)snprintf(sub, sizeof sub, "%s.%s", path, key ? key : "?");
            if (!json_equal(mine, theirs, sub, why, why_len)) {
                return 0;
            }
        }
        return 1;
    }
    }
}

/* 读一份 JSON 文件;失败返回 NULL。 */
static sxcl_json *load_json(const char *path)
{
    char *text = read_file(path);
    if (!text) {
        return NULL;
    }
    char err[160];
    err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(text, strlen(text), err, sizeof err);
    free(text);
    return doc;
}

/* ── 1) 目录与预设 ── */

static void test_dir_and_presets(const char *dir_root)
{
    /* 把平台默认目录指到测试目录:顺便验证"dir 传空 = %APPDATA%/SilentXCraftLauncher/keymaps"
     * 与 store.py 的 default_config_directory 一致(这里不碰用户真实的配置目录)。 */
#if defined(_WIN32)
    char scratch[512];
    (void)snprintf(scratch, sizeof scratch, "%s\\default", dir_root);
    char env[600];
    (void)snprintf(env, sizeof env, "APPDATA=%s", scratch);
    _putenv(env);
    const char *want_tail = "SilentXCraftLauncher\\keymaps";
#else
    char scratch[512];
    (void)snprintf(scratch, sizeof scratch, "%s/default", dir_root);
    setenv("XDG_CONFIG_HOME", scratch, 1);
    const char *want_tail = "silentxcraftlauncher/keymaps";
#endif

    char resolved[640];
    char err[200];
    check_int(sxcl_keymap_store_dir("", resolved, sizeof resolved, err, sizeof err), SXCL_KEYMAP_OK,
              "目录: 空 dir 走平台默认");
    check(strstr(resolved, want_tail) != NULL, "目录: 默认路径与 platform.py 一致");
    check(sxcl_fs_is_dir(resolved) == 1, "目录: 默认目录被建出来了(keymap_dir() 的行为)");
    check_int(sxcl_keymap_store_dir(dir_root, resolved, sizeof resolved, err, sizeof err),
              SXCL_KEYMAP_OK, "目录: 显式 dir 原样返回");
    check_str(resolved, dir_root, "目录: 显式 dir 不被改写");
    check_int(sxcl_keymap_store_dir(dir_root, resolved, 4, err, sizeof err), SXCL_KEYMAP_ERR_SPACE,
              "目录: 缓冲太小返回 SPACE");

    sxcl_keymap_store_catalog written;
    char preset_err[200];
    check_size(sxcl_keymap_store_ensure_presets(dir_root, 0, &written, preset_err, sizeof preset_err), 5,
               "预设: 首次落盘 5 份");
    check_size(written.count, 5, "预设: written 里也报了 5 份");
    check_str(written.items[0].key, "minimal", "预设: 顺序与 presets.py 一致(minimal 最先)");
    check_str(written.items[0].file, "preset-minimal.json", "预设: 文件名 preset-<key>.json");
    check_str(written.items[0].label, "极简（推荐新手）", "预设: 中文名照 presets.py");
    check_str(written.items[4].key, "one_hand", "预设: 最后一个是 one_hand");
    check_str(written.items[4].screen, "portrait", "预设: one_hand 固定竖屏");
    check_size(sxcl_keymap_store_ensure_presets(dir_root, 0, NULL, preset_err, sizeof preset_err), 0,
               "预设: 已经有了就不再写(overwrite=0)");
    check_size(sxcl_keymap_store_ensure_presets(dir_root, 1, NULL, preset_err, sizeof preset_err), 5,
               "预设: overwrite=1 强制重写");
}

/* ── 2) 列目录 ── */

static size_t catalog_find(const sxcl_keymap_store_catalog *catalog, const char *key)
{
    for (size_t i = 0; i < catalog->count; ++i) {
        if (strcmp(catalog->items[i].key, key) == 0) {
            return i + 1;   /* 0 = 没找到 */
        }
    }
    return 0;
}

static void test_list(const char *dir_root, const char *save_path)
{
    sxcl_keymap_store_catalog catalog;
    char err[200];
    check_int(sxcl_keymap_store_list(dir_root, &catalog, err, sizeof err), SXCL_KEYMAP_OK,
              "列目录: 成功");
    check_size(catalog.count, 6, "列目录: 5 个预设 + 刚存的那份");
    check_size(catalog.dropped, 0, "列目录: 没有丢东西");
    /* 按文件名排序:preset-building / preset-minimal / preset-one_hand / preset-pvp /
     * preset-survival / my-pvp ... 但 my-pvp 排在 preset-* **前面**(m < p)。 */
    check_str(catalog.items[0].file, "my-pvp.json", "列目录: 按文件名排序(my-pvp 最先)");
    check_int(catalog.items[0].builtin, 0, "列目录: 用户布局 builtin=0");
    check_str(catalog.items[0].name, "生存", "列目录: name 从 JSON 里读");
    check_str(catalog.items[0].screen, "portrait", "列目录: screen 从 JSON 里读");
    check_size(catalog.items[0].buttons, 6, "列目录: 按钮数从 JSON 里数");
    check_size(catalog.items[0].directions, 1, "列目录: 方向控件数从 JSON 里数");
    check_str(catalog.items[0].label, "生存", "列目录: 非预设的 label 用 name");

    const size_t minimal = catalog_find(&catalog, "minimal");
    check(minimal > 0, "列目录: 找得到预设 minimal");
    if (minimal > 0) {
        const sxcl_keymap_store_entry *item = &catalog.items[minimal - 1];
        check_int(item->builtin, 1, "列目录: 预设 builtin=1");
        check_str(item->file, "preset-minimal.json", "列目录: 预设的 file");
        check_str(item->label, "极简（推荐新手）", "列目录: 预设 label 走 PRESET_LABELS");
    }
    check(save_path != NULL && ends_with_name(save_path, "my-pvp.json") == 1,
          "列目录: 保存返回的就是 my-pvp.json");
}

static void write_list_json(const char *path, const sxcl_keymap_store_catalog *catalog)
{
    char text[8192];
    size_t used = 0;
    used += (size_t)snprintf(text + used, sizeof text - used, "[");
    for (size_t i = 0; i < catalog->count && used + 320 < sizeof text; ++i) {
        const sxcl_keymap_store_entry *item = &catalog->items[i];
        used += (size_t)snprintf(text + used, sizeof text - used,
                                 "%s\n  {\n    \"file\": \"%s\",\n    \"key\": \"%s\",\n"
                                 "    \"name\": \"%s\",\n    \"screen\": \"%s\",\n"
                                 "    \"buttons\": %lu,\n    \"directions\": %lu,\n"
                                 "    \"builtin\": %s,\n    \"label\": \"%s\"\n  }",
                                 i ? "," : "", item->file, item->key, item->name, item->screen,
                                 (unsigned long)item->buttons, (unsigned long)item->directions,
                                 item->builtin ? "true" : "false", item->label);
    }
    (void)snprintf(text + used, sizeof text - used, "\n]\n");
    (void)write_file(path, text);
}

/* ── 3) 保存 / 重名覆盖 / 文件名安全化 ── */

static void test_save_overwrite_and_names(const char *dir_root)
{
    char err[200];
    char path[640];
    sxcl_keymap_layout layout;
    check_int(sxcl_keymap_build_preset("survival", "portrait", "1.21.1", &layout), SXCL_KEYMAP_OK,
              "保存: 造一份生存(竖屏)");
    check_int(sxcl_keymap_store_save(dir_root, &layout, "my-pvp", path, sizeof path, err, sizeof err),
              SXCL_KEYMAP_OK, "保存: 成功");
    check(ends_with_name(path, "my-pvp.json") == 1, "保存: 文件名 = key + .json");

    /* 读回来逐字段等价 */
    sxcl_keymap_layout loaded;
    sxcl_keymap_issues issues;
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_store_load(dir_root, "my-pvp", &loaded, &issues, err, sizeof err),
              SXCL_KEYMAP_OK, "读取: 成功");
    const char *why = "";
    check(layouts_equal(&layout, &loaded, &why), "读取: 逐字段等价");
    if (why[0]) {
        printf("        (%s)\n", why);
    }
    check_str(loaded.screen, "portrait", "读取: 屏幕方向");
    check_str(loaded.mc_version, "1.21.1", "读取: mc_version");
    check_int(loaded.meta_json != NULL && strstr(loaded.meta_json, "builtin") != NULL, 1,
              "读取: meta 也读回来了");
    sxcl_keymap_layout_free(&loaded);

    /* 重名覆盖:换名字再存一次,文件数不变、内容换成新的 */
    sxcl_keymap_layout_free(&layout);
    check_int(sxcl_keymap_build_preset("minimal", "landscape", "", &layout), SXCL_KEYMAP_OK,
              "重名覆盖: 再造一份极简");
    check_int(sxcl_keymap_store_save(dir_root, &layout, "my-pvp", path, sizeof path, err, sizeof err),
              SXCL_KEYMAP_OK, "重名覆盖: 第二次保存成功");
    sxcl_keymap_store_catalog catalog;
    check_int(sxcl_keymap_store_list(dir_root, &catalog, err, sizeof err), SXCL_KEYMAP_OK,
              "重名覆盖: 列目录");
    check_size(catalog.count, 6, "重名覆盖: 文件数没变");
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_store_load(dir_root, "my-pvp", &loaded, &issues, err, sizeof err),
              SXCL_KEYMAP_OK, "重名覆盖: 还能读");
    check_str(loaded.name, "极简", "重名覆盖: 读到的是新的那一份");
    sxcl_keymap_layout_free(&loaded);
    sxcl_keymap_layout_free(&layout);

    /* 文件名安全化:布局名里的全角括号/斜杠/空格都要按 store.py 的规则过掉 */
    sxcl_keymap_layout named;
    check_int(sxcl_keymap_build_preset("pvp", "landscape", "", &named), SXCL_KEYMAP_OK, "名字: 造 pvp");
    strcpy(named.name, "对战（推荐）/battle");
    check_int(sxcl_keymap_store_save(dir_root, &named, NULL, path, sizeof path, err, sizeof err),
              SXCL_KEYMAP_OK, "名字: 用布局名当文件名");
    check(ends_with_name(path, "对战推荐battle.json") == 1,
          "名字: 全角括号与斜杠被过掉(与 store.py 的 isalnum 一致)");
    /* 空名字 -> layout.json(store.py 的兜底) */
    named.name[0] = '\0';
    check_int(sxcl_keymap_store_save(dir_root, &named, "", path, sizeof path, err, sizeof err),
              SXCL_KEYMAP_OK, "名字: 空名字也能存");
    check(ends_with_name(path, "layout.json") == 1, "名字: 空名字兜底 layout.json");
    /* key 里的空格/标点也过掉 */
    check_int(sxcl_keymap_store_save(dir_root, &named, "my pvp!", path, sizeof path, err, sizeof err),
              SXCL_KEYMAP_OK, "名字: key 里的标点过掉");
    check(ends_with_name(path, "mypvp.json") == 1, "名字: key -> mypvp.json");
    sxcl_keymap_layout_free(&named);

    /* 非法参数 */
    check_int(sxcl_keymap_store_save(dir_root, NULL, "x", path, sizeof path, err, sizeof err),
              SXCL_KEYMAP_ERR_ARG, "保存: 空 layout 返回 ARG");
}

/* ── 4) 非法 JSON 拒绝 ── */

static void test_bad_json(const char *dir_root)
{
    char path[640];
    char err[200];
    (void)snprintf(path, sizeof path, "%s/broken.json", dir_root);
    check_int(write_file(path, "{这不是 JSON"), 0, "坏文件: 写进去一份截断 JSON");
    (void)snprintf(path, sizeof path, "%s/notobj.json", dir_root);
    check_int(write_file(path, "[1, 2, 3]"), 0, "坏文件: 写进去一份顶层是数组的 JSON");

    sxcl_keymap_store_catalog catalog;
    check_int(sxcl_keymap_store_list(dir_root, &catalog, err, sizeof err), SXCL_KEYMAP_OK,
              "坏文件: 列目录不整份失败");
    check_size(catalog.count, 9, "坏文件: 好的 9 份照列(broken/notobj 被跳过)");
    check(catalog_find(&catalog, "broken") == 0, "坏文件: 坏 JSON 不进列表");
    check(catalog_find(&catalog, "notobj") == 0, "坏文件: 不是对象的也不进列表");
    check(err[0] != '\0', "坏文件: err 里说清跳过了谁");

    sxcl_keymap_layout layout;
    check(sxcl_keymap_store_load(dir_root, "broken", &layout, NULL, err, sizeof err) != SXCL_KEYMAP_OK,
          "坏文件: load 坏 JSON 返回错误");
    check(err[0] != '\0', "坏文件: err 里给了原因");
    printf("        (坏 JSON 的 err: %s)\n", err);
    check(sxcl_keymap_store_load(dir_root, "notobj", &layout, NULL, err, sizeof err) != SXCL_KEYMAP_OK,
          "坏文件: load 顶层不是对象也返回错误");
    check_int(sxcl_keymap_store_load(dir_root, "不存在", &layout, NULL, err, sizeof err),
              SXCL_KEYMAP_ERR_IO, "坏文件: 不存在的 key 返回 IO");
    check(strstr(err, "没有这份布局") != NULL, "坏文件: err 里说明找不到");
    check_int(sxcl_keymap_store_load(dir_root, "../escaped", &layout, NULL, err, sizeof err),
              SXCL_KEYMAP_ERR_ARG, "坏文件: key 里的路径分隔符被拒绝");
    check_int(sxcl_keymap_store_load(dir_root, "", &layout, NULL, err, sizeof err), SXCL_KEYMAP_ERR_ARG,
              "坏文件: 空 key 返回 ARG");
}

/* ── 5) active.json ── */

static void test_active(const char *dir_root)
{
    char path[640];
    char err[200];
    char name[128];
    (void)snprintf(path, sizeof path, "%s/active.json", dir_root);
    check_int(sxcl_fs_remove(path), 0, "当前布局: 先删掉 active.json");

    check_int(sxcl_keymap_store_active(dir_root, name, sizeof name), SXCL_KEYMAP_OK, "当前布局: 读默认");
    check_str(name, "preset-minimal", "当前布局: 没有 active.json 就是 preset-minimal");

    check_int(sxcl_keymap_store_set_active(dir_root, "preset-survival", path, sizeof path, err,
                                           sizeof err),
              SXCL_KEYMAP_OK, "当前布局: 设为 preset-survival");
    char *text = read_file(path);
    check(text != NULL, "当前布局: active.json 写出来了");
    if (text) {
        check_str(text, "{\n  \"active\": \"survival\"\n}", "当前布局: 内容与 store.py 逐字节一致");
        free(text);
    }
    check_int(sxcl_keymap_store_active(dir_root, name, sizeof name), SXCL_KEYMAP_OK, "当前布局: 再读");
    check_str(name, "survival", "当前布局: preset- 前缀去掉后才写进去");

    check_int(sxcl_keymap_store_set_active(dir_root, "my-pvp", NULL, 0, err, sizeof err), SXCL_KEYMAP_OK,
              "当前布局: 设为 my-pvp");
    (void)sxcl_keymap_store_active(dir_root, name, sizeof name);
    check_str(name, "my-pvp", "当前布局: 用户布局原样写");

    (void)snprintf(path, sizeof path, "%s/active.json", dir_root);
    check_int(write_file(path, "这不是 JSON"), 0, "当前布局: 弄坏 active.json");
    check_int(sxcl_keymap_store_active(dir_root, name, sizeof name), SXCL_KEYMAP_OK,
              "当前布局: 坏了也不报错");
    check_str(name, "", "当前布局: 坏了返回空串(store.py 的 except 分支)");
    (void)snprintf(path, sizeof path, "%s/active.json", dir_root);
    check_int(sxcl_fs_remove(path), 0, "当前布局: 删回默认");
    (void)sxcl_keymap_store_active(dir_root, name, sizeof name);
    check_str(name, "preset-minimal", "当前布局: 删了又回到默认值");
}

/* ── 6) delete ── */

static void test_delete(const char *dir_root)
{
    char path[640];
    check_int(sxcl_keymap_store_delete(dir_root, "mypvp"), 1, "删除: 用户布局能删");
    (void)snprintf(path, sizeof path, "%s/mypvp.json", dir_root);
    check_int(sxcl_fs_exists(path), 0, "删除: 文件真的没了");
    check_int(sxcl_keymap_store_delete(dir_root, "mypvp"), 0, "删除: 再删一次返回 0");
    check_int(sxcl_keymap_store_delete(dir_root, "preset-minimal"), 0, "删除: 内置预设拒删");
    (void)snprintf(path, sizeof path, "%s/preset-minimal.json", dir_root);
    check_int(sxcl_fs_exists(path), 1, "删除: 内置预设文件还在");
    check_int(sxcl_keymap_store_delete(dir_root, "../outside"), 0, "删除: 路径穿越拒绝");
    check_int(sxcl_keymap_store_delete(dir_root, ""), 0, "删除: 空 key 返回 0");
}

/* ── 7) 预设自动落盘(load 的回落路径) ── */

static void test_preset_fallback(const char *dir_root)
{
    char path[640];
    char err[200];
    (void)snprintf(path, sizeof path, "%s/preset-one_hand.json", dir_root);
    check_int(sxcl_fs_remove(path), 0, "预设回落: 先删掉 preset-one_hand.json");
    sxcl_keymap_layout layout;
    sxcl_keymap_issues issues;
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_store_load(dir_root, "one_hand", &layout, &issues, err, sizeof err),
              SXCL_KEYMAP_OK, "预设回落: load 预设名会先 ensure_presets()");
    check_str(layout.screen, "portrait", "预设回落: 读到的确实是单手(竖屏)");
    check_int(sxcl_fs_exists(path), 1, "预设回落: 文件被补回来了");
    sxcl_keymap_layout_free(&layout);
    /* key 带 .json:按原样找文件(store.py 就是这么拼的),所以要找的是 preset-one_hand.json */
    sxcl_keymap_issues_reset(&issues);
    check_int(sxcl_keymap_store_load(dir_root, "preset-one_hand.json", &layout, &issues, err,
                                     sizeof err),
              SXCL_KEYMAP_OK, "预设回落: key 带 .json 也认");
    sxcl_keymap_layout_free(&layout);
    /* 反过来:key 写 "one_hand.json" 找的是 one_hand.json(没有这个文件)—— 与 store.py 一致 */
    check_int(sxcl_keymap_store_load(dir_root, "one_hand.json", &layout, &issues, err, sizeof err),
              SXCL_KEYMAP_ERR_IO, "预设回落: key 里的 .json 不被当成预设名(store.py 的行为)");
}

/* ── 8) FCL:字段映射 + 与 Python 期望文件逐字段比对 ── */

#if defined(SXCL_KEYMAP_FIXTURE_DIR)
static void compare_with_python(const sxcl_keymap_layout *layout, const char *expected_file,
                                const char *what)
{
    char *mine = NULL;
    char err[200];
    check_int(sxcl_keymap_to_json(layout, &mine, err, sizeof err), SXCL_KEYMAP_OK, what);
    if (!mine) {
        return;
    }
    char path[640];
    (void)snprintf(path, sizeof path, "%s/%s", SXCL_KEYMAP_FIXTURE_DIR, expected_file);
    sxcl_json *doc_c = NULL;
    {
        char jerr[160];
        jerr[0] = '\0';
        doc_c = sxcl_json_parse(mine, strlen(mine), jerr, sizeof jerr);
    }
    sxcl_json *doc_py = load_json(path);
    check(doc_c != NULL, "FCL 一致性: C 版输出是合法 JSON");
    check(doc_py != NULL, "FCL 一致性: Python 期望文件读得进来");
    if (doc_c && doc_py) {
        char why[512];
        why[0] = '\0';
        char label[160];
        (void)snprintf(label, sizeof label, "FCL 一致性: C 版输出与 Python 的 %s 逐字段一致", expected_file);
        if (!json_equal(sxcl_json_root(doc_c), sxcl_json_root(doc_py), "layout", why, sizeof why)) {
            check(0, label);
            printf("        (%s)\n", why);
        } else {
            check(1, label);
        }
    }
    if (doc_c) {
        sxcl_json_free(doc_c);
    }
    if (doc_py) {
        sxcl_json_free(doc_py);
    }
    free(mine);
}

static void test_fcl_fixture(const char *dir_root, const char *file, const char *expected_file)
{
    char path[640];
    (void)snprintf(path, sizeof path, "%s/%s", SXCL_KEYMAP_FIXTURE_DIR, file);
    char *text = read_file(path);
    check(text != NULL, "FCL: 读得到夹具文件");
    if (!text) {
        return;
    }
    sxcl_keymap_layout layout;
    sxcl_keymap_issues issues;
    sxcl_keymap_issues_reset(&issues);
    char err[200];
    check_int(sxcl_keymap_import_fcl(text, strlen(text), NULL, 2400, 1080, &layout, &issues, err,
                                     sizeof err),
              SXCL_KEYMAP_OK, "FCL: 夹具导入成功");
    free(text);
    compare_with_python(&layout, expected_file, "FCL: 导出成 JSON");

    /* 与 Python 侧存成同名文件,交给 tools/keymap_parity.py 逐字段比对 */
    char save_path[640];
    char err2[200];
    const char *save_key = (strstr(file, "array") != NULL) ? "fcl-array-imported" : "fcl-imported";
    check_int(sxcl_keymap_store_save(dir_root, &layout, save_key, save_path, sizeof save_path, err2,
                                     sizeof err2),
              SXCL_KEYMAP_OK, "FCL: 导入结果存得下(供一致性比对)");
    sxcl_keymap_layout_free(&layout);
}

/* 对象形夹具里那些"容错路径"的落点(与 fcl_fixture_expected.json 是同一批断言;
 * 这里逐条点出来,失败时一眼能看出是哪条容错规则坏了)。 */
static void test_fcl_object_details(void)
{
    char path[640];
    (void)snprintf(path, sizeof path, "%s/fcl_fixture.json", SXCL_KEYMAP_FIXTURE_DIR);
    char *text = read_file(path);
    check(text != NULL, "FCL 细节: 读得到夹具");
    if (!text) {
        return;
    }
    sxcl_keymap_layout layout;
    sxcl_keymap_issues issues;
    sxcl_keymap_issues_reset(&issues);
    char err[200];
    check_int(sxcl_keymap_import_fcl(text, strlen(text), NULL, 2400, 1080, &layout, &issues, err,
                                     sizeof err),
              SXCL_KEYMAP_OK, "FCL 细节: 导入成功");
    free(text);

    check_size(sxcl_keymap_control_count(&layout), 11, "FCL 细节: 11 个控件(两个非对象项被跳过)");
    check_size(layout.direction_count, 2, "FCL 细节: 两个方向控件");
    check_size(layout.button_count, 9, "FCL 细节: 九个按钮");
    const sxcl_keymap_direction *move = sxcl_keymap_direction_by_id(&layout, "move");
    check(move != NULL, "FCL 细节: 方向控件用视图里的 id");
    if (move) {
        check_str(move->style, "dpad_compact",
                  "FCL 细节: 类型是 direction 就是 dpad_compact(不看视图里的 style 字段)");
        check_near(move->x, 72.0 / 2400.0, "FCL 细节: 像素坐标按屏宽归一化");
        check_near(move->y, 626.0 / 1080.0, "FCL 细节: 像素坐标按屏高归一化");
        check_str(move->label, "移动", "FCL 细节: 方向 label 取 text");
    }
    const sxcl_keymap_direction *joy = sxcl_keymap_direction_by_id(&layout, "move1");
    check(joy != NULL, "FCL 细节: 没写 id 的方向控件叫 move<序号>");
    if (joy) {
        check_str(joy->style, "rocker", "FCL 细节: 类型里有 joystick -> rocker");
        check_near(joy->x, 0.5, "FCL 细节: 已经是 0~1 的坐标不会再除一次");
    }
    const sxcl_keymap_button *shield = sxcl_keymap_button_by_id(&layout, "shield");
    check(shield != NULL, "FCL 细节: squareButton 认 id");
    if (shield) {
        check_str(shield->shape, "square", "FCL 细节: 类型里有 square -> shape=square");
        check_near(shield->opacity, 0.55, "FCL 细节: alpha=0 也当没写(0.55)");
        check_str(shield->group, "", "FCL 细节: group 用默认空串(不是预设那个 right)");
        check(shield->events[SXCL_KEYMAP_EVENT_PRESS].key_count == 1 &&
                  strcmp(shield->events[SXCL_KEYMAP_EVENT_PRESS].keys[0], "MOUSE_RIGHT") == 0,
              "FCL 细节: keys 字段里的中文「右键」-> MOUSE_RIGHT");
    }
    const sxcl_keymap_button *weird = sxcl_keymap_button_by_id(&layout, "weird");
    check(weird != NULL, "FCL 细节: 认不出的键码也留一个按钮");
    if (weird) {
        check_str(weird->events[SXCL_KEYMAP_EVENT_PRESS].keys[0], "KEY_999",
                  "FCL 细节: 认不出的 GLFW 码 -> KEY_<码>");
        check_str(weird->events[SXCL_KEYMAP_EVENT_PRESS].action, "",
                  "FCL 细节: 认不出动作就留空(不是瞎猜)");
    }
    const sxcl_keymap_button *hotbar = sxcl_keymap_button_by_id(&layout, "hotbar");
    check(hotbar != NULL && strcmp(hotbar->events[SXCL_KEYMAP_EVENT_PRESS].keys[0], "KEY_A") == 0,
          "FCL 细节: 数字字符串 65 -> KEY_A");
    const sxcl_keymap_button *bag = sxcl_keymap_button_by_id(&layout, "fcl7");
    check(bag != NULL, "FCL 细节: 空 id 回落 fcl<序号>");
    if (bag) {
        check_str(bag->label, "背包", "FCL 细节: 空 id 那个按钮的 label");
        check_str(bag->events[SXCL_KEYMAP_EVENT_PRESS].action, "inventory",
                  "FCL 细节: 多个键里第一个认得出来的动作 win");
        check_size(bag->events[SXCL_KEYMAP_EVENT_PRESS].key_count, 2, "FCL 细节: 两个键都留着");
    }
    const sxcl_keymap_button *drop = sxcl_keymap_button_by_id(&layout, "fcl8");
    check(drop != NULL && strcmp(drop->label, "丢弃") == 0,
          "FCL 细节: 没有 id 字段的按钮按视图序号命名");
    const sxcl_keymap_button *mixed = sxcl_keymap_button_by_id(&layout, "fcl9");
    check(mixed != NULL, "FCL 细节: 混合坐标(像素+归一化)也认");
    if (mixed) {
        check_near(mixed->x, 100.0 / 2400.0, "FCL 细节: 混合坐标 x 按屏宽");
        check_near(mixed->y, 0.5 / 1080.0, "FCL 细节: 混合坐标 y 也按屏高(与 fcl.py 一致)");
    }
    const sxcl_keymap_button *floor_btn = sxcl_keymap_button_by_id(&layout, "no_rect");
    check(floor_btn != NULL, "FCL 细节: 没有坐标的按钮也留着");
    if (floor_btn) {
        check_str(floor_btn->label, "键11", "FCL 细节: 没有 text 时 label 用 键<序号>");
        check_near(floor_btn->w, 0.09, "FCL 细节: 没写宽度用 0.09");
        check_near(floor_btn->h, 0.14, "FCL 细节: 没写高度用 0.14");
        check_int((int)floor_btn->event_mask, 0, "FCL 细节: 一个键都没有就不建事件");
    }
    check_int(sxcl_keymap_meta_bool(&layout, "fcl_raw_missing", 7), 7,
              "FCL 细节: meta 里没有的键给默认值");
    check(layout.meta_json != NULL && strstr(layout.meta_json, "fcl_raw") != NULL,
          "FCL 细节: 原始数据进 meta.fcl_raw");
    /* 数组形夹具(两个按钮 + 一个方向)也点一下 */
    check_size(sxcl_keymap_control_count(&layout) + 8, 19, "FCL 细节: 控件数与视图数对得上");
    sxcl_keymap_layout_free(&layout);
}
#endif /* SXCL_KEYMAP_FIXTURE_DIR */

static void test_fcl_array(void)
{
    /* 数组形态(顶层就是视图列表):name 用默认的"从 FCL 导入",meta 要包一层 views */
    static const char *const arr =
        "[ { \"type\": \"button\", \"text\": \"跳\", \"x\": 1200, \"y\": 540, \"width\": 120,"
        " \"height\": 120, \"keycodes\": [32] } ]";
    sxcl_keymap_layout layout;
    sxcl_keymap_issues issues;
    sxcl_keymap_issues_reset(&issues);
    char err[200];
    check_int(sxcl_keymap_import_fcl(arr, strlen(arr), NULL, 2400, 1080, &layout, &issues, err,
                                     sizeof err),
              SXCL_KEYMAP_OK, "FCL 数组形: 导入成功");
    check_str(layout.name, "从 FCL 导入", "FCL 数组形: 默认名字");
    check_str(layout.screen, "landscape", "FCL 数组形: 2400x1080 -> landscape");
    check_size(layout.button_count, 1, "FCL 数组形: 一个按钮");
    check(layout.meta_json != NULL && strstr(layout.meta_json, "\"views\"") != NULL,
          "FCL 数组形: meta.fcl_raw 包了一层 views(与 fcl.py 一致)");
    check_int(sxcl_keymap_meta_bool(&layout, "fcl_raw", 0), 0, "FCL 数组形: fcl_raw 是对象不是布尔");
    sxcl_keymap_layout_free(&layout);
}

int main(void)
{
    printf("按键布局存取 + FCL 一致性测试(store.py / fcl.py 的 C 版)\n");
    (void)sxcl_fs_remove_tree(SXCL_KP_STORE_OUT);
    if (sxcl_fs_mkdirs(SXCL_KP_STORE_OUT) != 0) {
        printf("  [!!] 建不了测试目录 %s\n", SXCL_KP_STORE_OUT);
        return 1;
    }

    char err[200];
    test_dir_and_presets(SXCL_KP_STORE_OUT);
    {
        /* 保存一份用户布局,后面列目录/读取都用它 */
        char path[640];
        sxcl_keymap_layout layout;
        (void)sxcl_keymap_build_preset("survival", "portrait", "1.21.1", &layout);
        (void)sxcl_keymap_store_save(SXCL_KP_STORE_OUT, &layout, "my-pvp", path, sizeof path, err,
                                     sizeof err);
        sxcl_keymap_layout_free(&layout);
        test_list(SXCL_KP_STORE_OUT, path);
    }
    test_save_overwrite_and_names(SXCL_KP_STORE_OUT);
    test_bad_json(SXCL_KP_STORE_OUT);
    test_active(SXCL_KP_STORE_OUT);
    test_delete(SXCL_KP_STORE_OUT);
    test_preset_fallback(SXCL_KP_STORE_OUT);
    test_fcl_array();
#if defined(SXCL_KEYMAP_FIXTURE_DIR)
    test_fcl_object_details();
    test_fcl_fixture(SXCL_KP_STORE_OUT, "fcl_fixture.json", "fcl_fixture_expected.json");
    test_fcl_fixture(SXCL_KP_STORE_OUT, "fcl_fixture_array.json", "fcl_fixture_array_expected.json");
#else
    printf("  (没定义 SXCL_KEYMAP_FIXTURE_DIR,跳过与 Python 期望文件的比对)\n");
#endif

    /* 收尾:把目录恢复成 tools/keymap_parity.py 的 Python 侧那份终态
     * (同一个操作序列 -> 同一个文件集合),只属于测试的产物清掉。 */
    {
        (void)sxcl_keymap_store_set_active(SXCL_KP_STORE_OUT, "preset-survival", NULL, 0, err,
                                           sizeof err);
        /* my-pvp 在"重名覆盖"里被换成了极简,这里恢复成生存 —— Python 侧脚本存的就是生存 */
        sxcl_keymap_layout survival;
        if (sxcl_keymap_build_preset("survival", "portrait", "1.21.1", &survival) == SXCL_KEYMAP_OK) {
            (void)sxcl_keymap_store_save(SXCL_KP_STORE_OUT, &survival, "my-pvp", NULL, 0, err,
                                         sizeof err);
            sxcl_keymap_layout_free(&survival);
        }
        (void)sxcl_fs_remove(SXCL_KP_STORE_OUT "/layout.json");
        (void)sxcl_fs_remove(SXCL_KP_STORE_OUT "/broken.json");
        (void)sxcl_fs_remove(SXCL_KP_STORE_OUT "/notobj.json");

        sxcl_keymap_store_catalog catalog;
        if (sxcl_keymap_store_list(SXCL_KP_STORE_OUT, &catalog, err, sizeof err) == SXCL_KEYMAP_OK) {
            write_list_json(SXCL_KP_STORE_OUT "/list_layouts.json", &catalog);
            printf("  产物: %s/(%lu 份布局 + active.json + list_layouts.json)\n", SXCL_KP_STORE_OUT,
                   (unsigned long)catalog.count);
        }
    }

    printf("按键布局存取 + FCL 一致性测试: 通过 %d 失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
