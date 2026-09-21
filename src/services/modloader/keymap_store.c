/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include "keymap_internal.h"
#include "sxcl/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#endif

#define SXCL_KP_ACTIVE_FILE "active.json"
#define SXCL_KP_PRESET_PREFIX "preset-"
#define SXCL_KP_STORE_FILE_MAX (8u * 1024u * 1024u)   /* 单份布局的读取上限(防呆) */

/* ── 路径小工具 ── */

static char path_sep(void)
{
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
}

/* dir + 分隔符 + leaf;装不下返回 -1。dir 末尾已经有分隔符就不重复加。 */
static int join_path(char *out, size_t out_len, const char *dir, const char *leaf)
{
    if (!out || !dir || !leaf) {
        return -1;
    }
    const size_t dlen = strlen(dir);
    const size_t llen = strlen(leaf);
    const int need_sep = (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\')) ? 0 : 1;
    if (dlen + (size_t)need_sep + llen + 1 > out_len) {
        return -1;
    }
    memcpy(out, dir, dlen);
    size_t n = dlen;
    if (need_sep) {
        out[n++] = path_sep();
    }
    memcpy(out + n, leaf, llen + 1);
    return 0;
}

/* 拼接并检查长度:a + b 写进 out。 */
static int append2(char *out, size_t out_len, const char *a, const char *b)
{
    if (!out || !a || !b) {
        return -1;
    }
    const int written = snprintf(out, out_len, "%s%s", a, b);
    return (written > 0 && (size_t)written < out_len) ? 0 : -1;
}

/* 平台默认配置目录(不含 "keymaps" 这一层)。与 platform.py 的分支一一对应。 */
static int default_root(char *out, size_t out_len)
{
#if defined(_WIN32)
    const char *appdata = getenv("APPDATA");
    if (appdata && *appdata) {
        return append2(out, out_len, appdata, "\\SilentXCraftLauncher");
    }
    /* APPDATA 没有(服务账号/受限沙箱):platform.py 的兜底是 Path.home()/AppData/Roaming */
    const char *home = getenv("USERPROFILE");
    if (home && *home) {
        return append2(out, out_len, home, "\\AppData\\Roaming\\SilentXCraftLauncher");
    }
    return -1;
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (home && *home) {
        return append2(out, out_len, home, "/Library/Application Support/SilentXCraftLauncher");
    }
    return -1;
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return append2(out, out_len, xdg, "/silentxcraftlauncher");
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        return append2(out, out_len, home, "/.config/silentxcraftlauncher");
    }
    return -1;
#endif
}

/* dir 空 -> 平台默认 <配置目录>/keymaps;非空 -> 原样用。返回 0 / -1(装不下或拼不出来)。 */
static int resolve_dir(const char *dir, char *out, size_t out_len)
{
    if (dir && *dir) {
        return sxcl_kp_copy(out, out_len, dir);
    }
    char root[SXCL_KEYMAP_STORE_PATH_MAX];
    if (default_root(root, sizeof root) != 0) {
        return -1;
    }
    return join_path(out, out_len, root, "keymaps");
}

/* ── 文件名映射(store.py 的规则) ── */

/* key 里不许出现路径分隔符(与 store.py 的有意差异:不让输入写到 keymaps/ 外面去)。 */
static int key_has_separator(const char *key)
{
    for (const char *p = key; p && *p; ++p) {
        if (*p == '/' || *p == '\\') {
            return 1;
        }
    }
    return 0;
}

static int has_json_suffix(const char *name)
{
    const size_t len = name ? strlen(name) : 0;
    return len > 5 && strcmp(name + len - 5, ".json") == 0;
}

/* Python 的 str.isalnum() 在 C 里的近似:按码点判断,覆盖会出现在布局名里的常用文字
 * (ASCII/拉丁扩展/希腊/西里尔/假名/CJK/谚文/全角字母数字)。冷门文字(如藏文)可能被丢掉,
 * 但标点(含全角括号)与符号的行为与 Python 一致 —— 那正是布局名里最常见的"非字母数字"。 */
static int py_alnum_cp(uint32_t cp)
{
    static const uint32_t ranges[][2] = {
        { 0x00AA, 0x00AA }, { 0x00B5, 0x00B5 }, { 0x00BA, 0x00BA },
        { 0x00C0, 0x00D6 }, { 0x00D8, 0x00F6 }, { 0x00F8, 0x02FF },   /* 拉丁扩展 */
        { 0x0370, 0x03FF },                                          /* 希腊 */
        { 0x0400, 0x052F },                                          /* 西里尔 */
        { 0x0531, 0x0588 }, { 0x05D0, 0x05EA }, { 0x0620, 0x064A },
        { 0x0660, 0x0669 }, { 0x06F0, 0x06F9 },                      /* 阿拉伯文字与数字 */
        { 0x0E01, 0x0E3A }, { 0x0E40, 0x0E5B },                      /* 泰文 */
        { 0x1E00, 0x1FFF },                                          /* 拉丁/希腊扩展附加 */
        { 0x3040, 0x30FF },                                          /* 平假名 / 片假名 */
        { 0x3131, 0x318E }, { 0xAC00, 0xD7A3 },                      /* 谚文 */
        { 0x31F0, 0x31FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF }, { 0xF900, 0xFAFF }, /* CJK */
        { 0xFF10, 0xFF19 }, { 0xFF21, 0xFF3A }, { 0xFF41, 0xFF5A },  /* 全角字母数字 */
        { 0xFF66, 0xFFDC },                                          /* 半角片假名 */
        { 0x20000, 0x2FA1F },                                        /* CJK 扩展 B 及以后 */
    };
    if ((cp >= (uint32_t)'0' && cp <= (uint32_t)'9') || (cp >= (uint32_t)'A' && cp <= (uint32_t)'Z') ||
        (cp >= (uint32_t)'a' && cp <= (uint32_t)'z')) {
        return 1;
    }
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
        if (cp >= ranges[i][0] && cp <= ranges[i][1]) {
            return 1;
        }
    }
    return 0;
}

/* 解一个 UTF-8 码点。返回消耗的字节数;0 = 这个字节不是合法的 UTF-8 起始。 */
static size_t utf8_decode(const unsigned char *p, size_t avail, uint32_t *out_cp)
{
    const unsigned char c0 = p[0];
    size_t need = 0;
    uint32_t cp = 0;
    if (c0 < 0x80) {
        *out_cp = c0;
        return 1;
    }
    if ((c0 & 0xE0) == 0xC0) { need = 1; cp = c0 & 0x1Fu; }
    else if ((c0 & 0xF0) == 0xE0) { need = 2; cp = c0 & 0x0Fu; }
    else if ((c0 & 0xF8) == 0xF0) { need = 3; cp = c0 & 0x07u; }
    else { return 0; }
    if (avail < need + 1) {
        return 0;
    }
    for (size_t i = 1; i <= need; ++i) {
        if ((p[i] & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3Fu);
    }
    *out_cp = cp;
    return need + 1;
}

/* store.py 的 save():安全名字 = 只留 str.isalnum() 为真的字符与 -_ (非 ASCII 按码点判)。 */
static void safe_key(char *out, size_t out_len, const char *text)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    for (size_t left = strlen((const char *)p); left > 0;) {
        if (*p == '-' || *p == '_') {
            if (n + 1 < out_len) {
                out[n++] = (char)*p;
            }
            ++p;
            --left;
            continue;
        }
        uint32_t cp = 0;
        const size_t used = utf8_decode(p, left, &cp);
        if (used == 0) {
            ++p;   /* 非法字节:丢掉(不能拼进文件名) */
            --left;
            continue;
        }
        if (py_alnum_cp(cp)) {
            if (n + used < out_len) {
                memcpy(out + n, p, used);
                n += used;
            }
        }
        p += used;
        left -= used;
    }
    out[n] = '\0';
}

/* "my-pvp" / "my-pvp.json" -> "my-pvp.json"(store.py 拼路径的规则) */
static int leaf_of_key(const char *key, char *out, size_t out_len)
{
    const size_t len = strlen(key);
    if (len > 5 && strcmp(key + len - 5, ".json") == 0) {
        return sxcl_kp_copy(out, out_len, key);
    }
    const int written = snprintf(out, out_len, "%s.json", key);
    return (written > 0 && (size_t)written < out_len) ? 0 : -1;
}

static int is_preset_key(const char *key)
{
    for (size_t i = 0; i < sxcl_keymap_preset_count(); ++i) {
        if (strcmp(sxcl_keymap_preset_key(i), key) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *preset_label_for(const char *key)
{
    for (size_t i = 0; i < sxcl_keymap_preset_count(); ++i) {
        if (strcmp(sxcl_keymap_preset_key(i), key) == 0) {
            return sxcl_keymap_preset_label(i);
        }
    }
    return NULL;
}

/* ── 文件小工具 ── */

/* 整份读进来(malloc 给调用方;失败返回 -1)。 */
static int read_text_file(const char *path, char **out_text)
{
    *out_text = NULL;
    FILE *fh = sxcl_fs_fopen(path, "rb");
    if (!fh) {
        return -1;
    }
    if (fseek(fh, 0, SEEK_END) != 0) {
        fclose(fh);
        return -1;
    }
    const long size = ftell(fh);
    if (size < 0 || (unsigned long)size > SXCL_KP_STORE_FILE_MAX) {
        fclose(fh);
        return -1;
    }
    rewind(fh);
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        fclose(fh);
        return -1;
    }
    const size_t got = (size > 0) ? fread(text, 1, (size_t)size, fh) : 0;
    fclose(fh);
    if (got != (size_t)size) {
        free(text);
        return -1;
    }
    text[got] = '\0';
    *out_text = text;
    return 0;
}

/* 原子写文本(先写 .tmp 再改名);active.json 用,布局本身走 sxcl_keymap_save_file。 */
static int write_text_atomic(const char *path, const char *text)
{
    if (sxcl_fs_mkdirs_for_file(path) != 0) {
        return -1;
    }
    char tmp[SXCL_KEYMAP_STORE_PATH_MAX + 8];
    if (append2(tmp, sizeof tmp, path, ".tmp") != 0) {
        return -1;
    }
    FILE *fh = sxcl_fs_fopen(tmp, "wb");
    if (!fh) {
        return -1;
    }
    const size_t len = strlen(text);
    const size_t written = (len > 0) ? fwrite(text, 1, len, fh) : 0;
    fclose(fh);
    if (written != len) {
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    if (sxcl_fs_rename_replace(tmp, path) != 0) {
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    return 0;
}

/* ── 目录列举 ── */

static int compare_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* dir 下的 *.json 文件名(不递归),按文件名排序(store.py 是 sorted(glob))。返回 0 成功。 */
static int list_json_names(const char *dir, char (*names)[SXCL_KEYMAP_STORE_FILE_MAX], size_t cap,
                           size_t *count, size_t *dropped)
{
    *count = 0;
    *dropped = 0;
#if defined(_WIN32)
    char pattern[SXCL_KEYMAP_STORE_PATH_MAX];
    if (join_path(pattern, sizeof pattern, dir, "*") != 0) {
        return -1;
    }
    wchar_t wpattern[SXCL_KEYMAP_STORE_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern,
                            (int)(sizeof wpattern / sizeof wpattern[0])) <= 0) {
        return -1;
    }
    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(wpattern, &found);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;   /* 目录不在(调用方一般已经建过):当空目录 */
    }
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        char name[SXCL_KEYMAP_STORE_FILE_MAX];
        if (WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, name, (int)sizeof name, NULL, NULL) <= 0) {
            continue;
        }
        if (!has_json_suffix(name)) {
            continue;
        }
        if (*count < cap) {
            sxcl_kp_copy(names[*count], SXCL_KEYMAP_STORE_FILE_MAX, name);
            ++*count;
        } else {
            ++*dropped;
        }
    } while (FindNextFileW(handle, &found));
    FindClose(handle);
#else
    DIR *handle = opendir(dir);
    if (!handle) {
        return 0;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(handle)) != NULL) {
        if (!has_json_suffix(entry->d_name)) {
            continue;
        }
        char full[SXCL_KEYMAP_STORE_PATH_MAX];
        if (join_path(full, sizeof full, dir, entry->d_name) != 0 || sxcl_fs_is_dir(full)) {
            continue;
        }
        if (*count < cap) {
            sxcl_kp_copy(names[*count], SXCL_KEYMAP_STORE_FILE_MAX, entry->d_name);
            ++*count;
        } else {
            ++*dropped;
        }
    }
    closedir(handle);
#endif
    if (*count > 1) {
        qsort(names, *count, SXCL_KEYMAP_STORE_FILE_MAX, compare_names);
    }
    return 0;
}

/* ── 对外:目录 ── */

int sxcl_keymap_store_dir(const char *dir, char *out, size_t out_len, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!out || out_len == 0) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    out[0] = '\0';
    char path[SXCL_KEYMAP_STORE_PATH_MAX];
    if (resolve_dir(dir, path, sizeof path) != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "拼不出 keymaps 目录(APPDATA/HOME 没有?路径太长?)");
        }
        return SXCL_KEYMAP_ERR_IO;
    }
    if (sxcl_fs_mkdirs(path) != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "建不了目录: %s", path);
        }
        return SXCL_KEYMAP_ERR_IO;
    }
    if (sxcl_kp_copy(out, out_len, path) != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "目录路径太长: %s", path);
        }
        return SXCL_KEYMAP_ERR_SPACE;
    }
    return SXCL_KEYMAP_OK;
}

/* ── 对外:列布局(list_layouts) ── */

/* 从 JSON 里取 list_layouts() 用到的四个字段(name/screen/buttons/directions)。
 * 与 store.py 一样**读原始字段**(不做模型的默认值/钳制),读不出来就跳过这份文件。 */
static int fill_entry(sxcl_keymap_store_entry *item, const char *file, const char *text, size_t len)
{
    char json_err[120];
    json_err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(text, len, json_err, sizeof json_err);
    if (!doc) {
        return -1;
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    if (!root || sxcl_json_type_of(root) != SXCL_JSON_OBJECT) {
        sxcl_json_free(doc);
        return -1;
    }
    memset(item, 0, sizeof *item);
    sxcl_kp_copy(item->file, sizeof item->file, file);
    const size_t file_len = strlen(file);
    char stem[SXCL_KEYMAP_STORE_KEY_MAX];
    if (file_len > 5 && file_len - 5 < sizeof stem) {
        sxcl_kp_copy_range(stem, sizeof stem, file, file_len - 5);
    } else {
        sxcl_kp_copy(stem, sizeof stem, file);
    }
    const size_t prefix = strlen(SXCL_KP_PRESET_PREFIX);
    const char *key = (strncmp(stem, SXCL_KP_PRESET_PREFIX, prefix) == 0) ? stem + prefix : stem;
    sxcl_kp_copy(item->key, sizeof item->key, key);
    const char *name = sxcl_kp_string(root, "name", NULL);
    sxcl_kp_copy(item->name, sizeof item->name, (name && *name) ? name : stem);
    const char *screen = sxcl_kp_string(root, "screen", NULL);
    sxcl_kp_copy(item->screen, sizeof item->screen, (screen && *screen) ? screen : "landscape");
    const sxcl_json_value *buttons = sxcl_json_get(root, "buttons");
    item->buttons = (buttons && sxcl_json_type_of(buttons) == SXCL_JSON_ARRAY) ? sxcl_json_size(buttons) : 0;
    const sxcl_json_value *directions = sxcl_json_get(root, "directions");
    item->directions =
        (directions && sxcl_json_type_of(directions) == SXCL_JSON_ARRAY) ? sxcl_json_size(directions) : 0;
    item->builtin = (strncmp(file, SXCL_KP_PRESET_PREFIX, prefix) == 0) ? 1 : 0;
    const char *label = preset_label_for(item->key);
    sxcl_kp_copy(item->label, sizeof item->label, label ? label : item->name);
    sxcl_json_free(doc);
    return 0;
}

int sxcl_keymap_store_list(const char *dir, sxcl_keymap_store_catalog *out, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!out) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    char path[SXCL_KEYMAP_STORE_PATH_MAX];
    int rc = sxcl_keymap_store_dir(dir, path, sizeof path, err, err_len);
    if (rc != SXCL_KEYMAP_OK) {
        return rc;
    }
    char names[SXCL_KEYMAP_STORE_LIST_MAX][SXCL_KEYMAP_STORE_FILE_MAX];
    size_t found = 0;
    size_t dropped = 0;
    if (list_json_names(path, names, SXCL_KEYMAP_STORE_LIST_MAX, &found, &dropped) != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "读不了目录: %s", path);
        }
        return SXCL_KEYMAP_ERR_IO;
    }
    out->dropped = dropped;
    for (size_t i = 0; i < found; ++i) {
        if (strcmp(names[i], SXCL_KP_ACTIVE_FILE) == 0) {
            continue;   /* active.json 不是布局(store.py 也跳过它) */
        }
        char full[SXCL_KEYMAP_STORE_PATH_MAX];
        if (join_path(full, sizeof full, path, names[i]) != 0) {
            continue;
        }
        char *text = NULL;
        if (read_text_file(full, &text) != 0) {
            if (err && err_len) {
                (void)snprintf(err, err_len, "布局读取失败 %s", names[i]);
            }
            continue;
        }
        sxcl_keymap_store_entry item;
        if (fill_entry(&item, names[i], text, strlen(text)) != 0) {
            /* store.py: json 坏了就 log.warning 跳过,不整份失败 */
            if (err && err_len) {
                (void)snprintf(err, err_len, "布局读取失败 %s: 不是有效的 JSON", names[i]);
            }
            free(text);
            continue;
        }
        free(text);
        if (out->count < SXCL_KEYMAP_STORE_LIST_MAX) {
            out->items[out->count++] = item;
        } else {
            ++out->dropped;
        }
    }
    return SXCL_KEYMAP_OK;
}

/* ── 对外:读/存/删 ── */

int sxcl_keymap_store_load(const char *dir, const char *key, sxcl_keymap_layout *out,
                           sxcl_keymap_issues *issues, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!out) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    if (!key || !*key) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "没有给 key(布局名)");
        }
        return SXCL_KEYMAP_ERR_ARG;
    }
    if (key_has_separator(key)) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "key 里不能有路径分隔符: %s", key);
        }
        return SXCL_KEYMAP_ERR_ARG;
    }
    char base[SXCL_KEYMAP_STORE_PATH_MAX];
    if (sxcl_keymap_store_dir(dir, base, sizeof base, err, err_len) != SXCL_KEYMAP_OK) {
        return SXCL_KEYMAP_ERR_IO;
    }
    char leaf[SXCL_KEYMAP_STORE_FILE_MAX];
    char path[SXCL_KEYMAP_STORE_PATH_MAX];
    if (leaf_of_key(key, leaf, sizeof leaf) != 0 || join_path(path, sizeof path, base, leaf) != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "路径太长: %s", key);
        }
        return SXCL_KEYMAP_ERR_SPACE;
    }
    if (!sxcl_fs_exists(path) && is_preset_key(key)) {
        /* store.py:预设文件不在时先 ensure_presets() 再找一次 */
        const int written = snprintf(leaf, sizeof leaf, "%s%s.json", SXCL_KP_PRESET_PREFIX, key);
        if (written <= 0 || (size_t)written >= sizeof leaf ||
            join_path(path, sizeof path, base, leaf) != 0) {
            return SXCL_KEYMAP_ERR_SPACE;
        }
        if (!sxcl_fs_exists(path)) {
            (void)sxcl_keymap_store_ensure_presets(dir, 0, NULL, err, err_len);
        }
    }
    if (!sxcl_fs_exists(path)) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "没有这份布局: %s", path);
        }
        return SXCL_KEYMAP_ERR_IO;
    }
    return sxcl_keymap_load_file(path, out, issues, err, err_len);
}

int sxcl_keymap_store_save(const char *dir, const sxcl_keymap_layout *layout, const char *key,
                           char *out_path, size_t out_len, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (out_path && out_len) {
        out_path[0] = '\0';
    }
    if (!layout) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    const char *source = (key && *key) ? key : ((layout->name && *layout->name) ? layout->name : "layout");
    char safe[SXCL_KEYMAP_STORE_KEY_MAX];
    safe_key(safe, sizeof safe, source);
    if (!safe[0]) {
        sxcl_kp_copy(safe, sizeof safe, "layout");
    }
    char leaf[SXCL_KEYMAP_STORE_FILE_MAX];
    const int written = snprintf(leaf, sizeof leaf, "%s.json", safe);
    if (written <= 0 || (size_t)written >= sizeof leaf) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "布局名太长: %s", source);
        }
        return SXCL_KEYMAP_ERR_SPACE;
    }
    char base[SXCL_KEYMAP_STORE_PATH_MAX];
    if (sxcl_keymap_store_dir(dir, base, sizeof base, err, err_len) != SXCL_KEYMAP_OK) {
        return SXCL_KEYMAP_ERR_IO;
    }
    char path[SXCL_KEYMAP_STORE_PATH_MAX];
    if (join_path(path, sizeof path, base, leaf) != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "路径太长: %s", leaf);
        }
        return SXCL_KEYMAP_ERR_SPACE;
    }
    const int rc = sxcl_keymap_save_file(layout, path, err, err_len);
    if (rc != SXCL_KEYMAP_OK) {
        return rc;
    }
    if (out_path && out_len && sxcl_kp_copy(out_path, out_len, path) != 0) {
        return SXCL_KEYMAP_ERR_SPACE;
    }
    return SXCL_KEYMAP_OK;
}

int sxcl_keymap_store_delete(const char *dir, const char *key)
{
    if (!key || !*key || key_has_separator(key)) {
        return 0;
    }
    char base[SXCL_KEYMAP_STORE_PATH_MAX];
    if (sxcl_keymap_store_dir(dir, base, sizeof base, NULL, 0) != SXCL_KEYMAP_OK) {
        return 0;
    }
    char leaf[SXCL_KEYMAP_STORE_FILE_MAX];
    char path[SXCL_KEYMAP_STORE_PATH_MAX];
    if (leaf_of_key(key, leaf, sizeof leaf) != 0 || join_path(path, sizeof path, base, leaf) != 0) {
        return 0;
    }
    if (strncmp(leaf, SXCL_KP_PRESET_PREFIX, strlen(SXCL_KP_PRESET_PREFIX)) == 0) {
        return 0;   /* 内置预设不许删(store.py 的规矩) */
    }
    if (!sxcl_fs_exists(path)) {
        return 0;
    }
    return sxcl_fs_remove(path) == 0 ? 1 : 0;
}

/* ── 对外:active.json ── */

int sxcl_keymap_store_active(const char *dir, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return SXCL_KEYMAP_ERR_ARG;
    }
    out[0] = '\0';
    char base[SXCL_KEYMAP_STORE_PATH_MAX];
    if (sxcl_keymap_store_dir(dir, base, sizeof base, NULL, 0) != SXCL_KEYMAP_OK) {
        return SXCL_KEYMAP_ERR_IO;
    }
    char path[SXCL_KEYMAP_STORE_PATH_MAX];
    if (join_path(path, sizeof path, base, SXCL_KP_ACTIVE_FILE) != 0) {
        return SXCL_KEYMAP_ERR_SPACE;
    }
    if (!sxcl_fs_exists(path)) {
        sxcl_kp_copy(out, out_len, "preset-minimal");   /* store.py:没有 active.json 时的默认值 */
        return SXCL_KEYMAP_OK;
    }
    char *text = NULL;
    if (read_text_file(path, &text) != 0) {
        return SXCL_KEYMAP_OK;   /* 读不了当空(store.py 的 except 分支) */
    }
    char json_err[64];
    json_err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(text, strlen(text), json_err, sizeof json_err);
    free(text);
    if (!doc) {
        return SXCL_KEYMAP_OK;   /* 坏了返回 "",与 store.py 的 except 分支一致 */
    }
    const sxcl_json_value *root = sxcl_json_root(doc);
    const sxcl_json_value *value = (root && sxcl_json_type_of(root) == SXCL_JSON_OBJECT)
                                       ? sxcl_json_get(root, "active")
                                       : NULL;
    if (value && sxcl_json_type_of(value) == SXCL_JSON_STRING) {
        sxcl_kp_copy(out, out_len, sxcl_json_string(value));
    }
    sxcl_json_free(doc);
    return SXCL_KEYMAP_OK;
}

int sxcl_keymap_store_set_active(const char *dir, const char *key, char *out_path, size_t out_len,
                                 char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (out_path && out_len) {
        out_path[0] = '\0';
    }
    const char *name = key ? key : "";
    const size_t prefix = strlen(SXCL_KP_PRESET_PREFIX);
    if (strncmp(name, SXCL_KP_PRESET_PREFIX, prefix) == 0) {
        name += prefix;   /* store.py: active.json 里存的是去掉 preset- 的 key */
    }
    char base[SXCL_KEYMAP_STORE_PATH_MAX];
    if (sxcl_keymap_store_dir(dir, base, sizeof base, err, err_len) != SXCL_KEYMAP_OK) {
        return SXCL_KEYMAP_ERR_IO;
    }
    char path[SXCL_KEYMAP_STORE_PATH_MAX];
    if (join_path(path, sizeof path, base, SXCL_KP_ACTIVE_FILE) != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "路径太长: %s", SXCL_KP_ACTIVE_FILE);
        }
        return SXCL_KEYMAP_ERR_SPACE;
    }
    /* json.dumps({"active": key}, ensure_ascii=False, indent=2) 的逐字节写法 */
    sxcl_kp_buf buf;
    sxcl_kp_buf_init(&buf);
    int rc = sxcl_kp_buf_puts(&buf, "{\n  \"active\": ");
    if (rc == SXCL_KEYMAP_OK) {
        rc = sxcl_kp_buf_json_string(&buf, name);
    }
    if (rc == SXCL_KEYMAP_OK) {
        rc = sxcl_kp_buf_puts(&buf, "\n}");
    }
    if (rc != SXCL_KEYMAP_OK) {
        sxcl_kp_buf_free(&buf);
        if (err && err_len) {
            (void)snprintf(err, err_len, "内存不足(拼 active.json)");
        }
        return rc;
    }
    const int write_rc = write_text_atomic(path, buf.data);
    sxcl_kp_buf_free(&buf);
    if (write_rc != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "写不了: %s", path);
        }
        return SXCL_KEYMAP_ERR_IO;
    }
    if (out_path && out_len && sxcl_kp_copy(out_path, out_len, path) != 0) {
        return SXCL_KEYMAP_ERR_SPACE;
    }
    return SXCL_KEYMAP_OK;
}

/* ── 对外:落盘内置预设 ── */

size_t sxcl_keymap_store_ensure_presets(const char *dir, int overwrite, sxcl_keymap_store_catalog *written,
                                        char *err, size_t err_len)
{
    char first_err[160];
    first_err[0] = '\0';
    if (err && err_len) {
        err[0] = '\0';
    }
    if (written) {
        memset(written, 0, sizeof *written);
    }
    char base[SXCL_KEYMAP_STORE_PATH_MAX];
    if (sxcl_keymap_store_dir(dir, base, sizeof base, first_err, sizeof first_err) != SXCL_KEYMAP_OK) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "%s", first_err);
        }
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < sxcl_keymap_preset_count(); ++i) {
        const char *key = sxcl_keymap_preset_key(i);
        char leaf[SXCL_KEYMAP_STORE_FILE_MAX];
        char path[SXCL_KEYMAP_STORE_PATH_MAX];
        const int written_len = snprintf(leaf, sizeof leaf, "%s%s.json", SXCL_KP_PRESET_PREFIX, key);
        if (written_len <= 0 || (size_t)written_len >= sizeof leaf ||
            join_path(path, sizeof path, base, leaf) != 0) {
            continue;
        }
        if (sxcl_fs_exists(path) && !overwrite) {
            continue;   /* 已经落过盘的不动(store.py 的 overwrite=False) */
        }
        sxcl_keymap_layout layout;
        if (sxcl_keymap_build_preset(key, NULL, NULL, &layout) != SXCL_KEYMAP_OK) {
            continue;
        }
        char save_err[160];
        save_err[0] = '\0';
        const int rc = sxcl_keymap_save_file(&layout, path, save_err, sizeof save_err);
        if (rc != SXCL_KEYMAP_OK) {
            if (!first_err[0]) {
                (void)snprintf(first_err, sizeof first_err, "预设 %s 写不了: %s", key, save_err);
            }
            sxcl_keymap_layout_free(&layout);
            continue;
        }
        ++count;
        if (written) {
            if (written->count < SXCL_KEYMAP_STORE_LIST_MAX) {
                sxcl_keymap_store_entry *item = &written->items[written->count++];
                memset(item, 0, sizeof *item);
                sxcl_kp_copy(item->file, sizeof item->file, leaf);
                sxcl_kp_copy(item->key, sizeof item->key, key);
                sxcl_kp_copy(item->name, sizeof item->name, layout.name);
                sxcl_kp_copy(item->label, sizeof item->label, preset_label_for(key) ? preset_label_for(key)
                                                                                   : layout.name);
                sxcl_kp_copy(item->screen, sizeof item->screen, layout.screen);
                item->buttons = layout.button_count;
                item->directions = layout.direction_count;
                item->builtin = 1;
            } else {
                ++written->dropped;
            }
        }
        sxcl_keymap_layout_free(&layout);
    }
    if (err && err_len && first_err[0]) {
        (void)snprintf(err, err_len, "%s", first_err);
    }
    return count;
}
