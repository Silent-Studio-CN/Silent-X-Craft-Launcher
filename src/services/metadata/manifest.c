/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/manifest.h"

#include "sxcl/engine.h"  /* sxcl_version_plan_fetch:启动前"补全文件"要跑下载引擎 */
#include "sxcl/fs.h"
#include "sxcl/natives.h" /* sxcl_natives_classifier_of:老版本 natives 键里的 arch 占位符 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <sys/sysctl.h>
#  include <sys/utsname.h>
#else
#  include <sys/utsname.h>
#endif

/* ── 平台判定 ── */

const char *sxcl_platform_os_name(void)
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "osx";
#else
    return "linux";
#endif
}

const char *sxcl_platform_arch_name(void)
{
#if defined(_WIN32)
#  if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#  elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#  else
    return "x64";
#  endif
#elif defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__i386__)
    return "x86";
#else
    return "x64";
#endif
}

int sxcl_rules_allow(const sxcl_json_value *rules, const char *os_name, const char *arch_name)
{
    if (!rules || sxcl_json_type_of(rules) != SXCL_JSON_ARRAY) {
        return 1; /* 没有 rules = 一律允许 */
    }
    const size_t n = sxcl_json_size(rules);
    if (n == 0) {
        return 1;
    }
    int allowed = 0; /* 有 rules 时默认不允许,由命中的规则决定 */
    for (size_t i = 0; i < n; ++i) {
        const sxcl_json_value *rule = sxcl_json_at(rules, i);
        if (!rule || sxcl_json_type_of(rule) != SXCL_JSON_OBJECT) {
            continue;
        }
        if (sxcl_json_get(rule, "features")) {
            continue; /* demo/quickPlay 这类特性规则一律不匹配 */
        }
        const sxcl_json_value *os = sxcl_json_get(rule, "os");
        if (os) {
            const char *name = sxcl_json_get_string(os, "name", NULL);
            if (name && strcmp(name, os_name) != 0) {
                continue;
            }
            const char *arch = sxcl_json_get_string(os, "arch", NULL);
            if (arch && arch_name && strcmp(arch, arch_name) != 0) {
                continue;
            }
            if (sxcl_json_get_string(os, "version", NULL)) {
                continue; /* 系统版本正则太复杂,启动器的通行做法是不匹配 */
            }
        }
        const char *action = sxcl_json_get_string(rule, "action", "allow");
        allowed = (strcmp(action, "disallow") == 0) ? 0 : 1;
    }
    return allowed;
}

/* ── 镜像映射 ── */

/** 前缀匹配(不区分大小写);匹配上返回剩余部分(指向 url 内部),否则 NULL。 */
static const char *prefix_after(const char *url, const char *prefix)
{
    const size_t n = strlen(prefix);
    size_t i = 0;
    if (url == NULL || prefix == NULL) {
        return NULL;
    }
    for (i = 0; i < n; ++i) {
        char a = url[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return NULL;
        }
    }
    return url + n;
}

static const char *mirror_join(const char *base, const char *sub, const char *rest, char *out,
                               size_t out_len)
{
    const int n = snprintf(out, out_len, "%s%s%s", base, sub ? sub : "", rest ? rest : "");
    if (n < 0 || (size_t)n >= out_len) {
        out[0] = '\0';
        return NULL;
    }
    return out;
}

int sxcl_manifest_mirror_url(const char *url, const char *mirror_base, char *out, size_t out_len)
{
    const char *base = (mirror_base && *mirror_base) ? mirror_base : SXCL_MIRROR_BMCLAPI_BASE;
    const char *rest = NULL;
    if (out == NULL || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    if (url == NULL || url[0] == '\0') {
        return -1;
    }
    /* 先长后短:launchermeta 与 piston-meta 是并列的两条前缀,不互相包含,顺序无所谓;
     * 但 libraries / resources 必须排在它们后面判(前缀不重叠,这里只是写明意图)。 */
    if ((rest = prefix_after(url, "https://launchermeta.mojang.com")) != NULL ||
        (rest = prefix_after(url, "https://piston-meta.mojang.com")) != NULL ||
        (rest = prefix_after(url, "https://piston-data.mojang.com")) != NULL ||
        (rest = prefix_after(url, "http://launchermeta.mojang.com")) != NULL ||
        (rest = prefix_after(url, "http://piston-meta.mojang.com")) != NULL ||
        (rest = prefix_after(url, "http://piston-data.mojang.com")) != NULL) {
        return mirror_join(base, NULL, rest, out, out_len) ? 0 : -1;
    }
    if ((rest = prefix_after(url, "https://libraries.minecraft.net")) != NULL ||
        (rest = prefix_after(url, "http://libraries.minecraft.net")) != NULL) {
        return mirror_join(base, "/maven", rest, out, out_len) ? 0 : -1;
    }
    if ((rest = prefix_after(url, "https://resources.download.minecraft.net")) != NULL ||
        (rest = prefix_after(url, "http://resources.download.minecraft.net")) != NULL) {
        return mirror_join(base, "/assets", rest, out, out_len) ? 0 : -1;
    }
    /* 三个**加载器 maven**（docs/22 的 A4「镜像三前缀」）：BMCLAPI 都在 /maven/ 下镜像它。
     * 实测（2026-09-22 晚逐个 HEAD）：forge 安装器 / fabric-loader / neoforge 安装器 /
     * Maven Central 的 gson 全是 200 —— 以前这三家**一条镜像都没有**，
     * 于是"方式 A 的安装器自己下几百 MB"这条路完全绕开镜像。
     * Quilt 的 maven **故意不映射**:BMCLAPI 对它 404,映射了只会多一条必死的候选。 */
    if ((rest = prefix_after(url, "https://maven.minecraftforge.net")) != NULL ||
        (rest = prefix_after(url, "http://maven.minecraftforge.net")) != NULL ||
        (rest = prefix_after(url, "https://maven.neoforged.net/releases")) != NULL ||
        (rest = prefix_after(url, "http://maven.neoforged.net/releases")) != NULL ||
        (rest = prefix_after(url, "https://maven.fabricmc.net")) != NULL ||
        (rest = prefix_after(url, "http://maven.fabricmc.net")) != NULL) {
        return mirror_join(base, "/maven", rest, out, out_len) ? 0 : -1;
    }
    return -1;
}

/* ── 版本清单 ── */

struct sxcl_version_list {
    sxcl_version_entry *entries;
    size_t count;
    char latest_release[64];
    char latest_snapshot[64];
};

static char *dup_str(const char *s)
{
    if (!s) {
        return NULL;
    }
    const size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) {
        memcpy(p, s, n + 1);
    }
    return p;
}

sxcl_version_list *sxcl_version_list_build(const sxcl_json *doc)
{
    const sxcl_json_value *root = sxcl_json_root(doc);
    const sxcl_json_value *versions = sxcl_json_get(root, "versions");
    if (!versions || sxcl_json_type_of(versions) != SXCL_JSON_ARRAY) {
        return NULL;
    }
    sxcl_version_list *list = (sxcl_version_list *)calloc(1, sizeof(sxcl_version_list));
    if (!list) {
        return NULL;
    }
    const size_t n = sxcl_json_size(versions);
    list->entries = (sxcl_version_entry *)calloc(n ? n : 1, sizeof(sxcl_version_entry));
    if (!list->entries) {
        free(list);
        return NULL;
    }
    list->count = n;
    for (size_t i = 0; i < n; ++i) {
        const sxcl_json_value *src = sxcl_json_at(versions, i);
        sxcl_version_entry *dst = &list->entries[i];
        dst->id = dup_str(sxcl_json_get_string(src, "id", ""));
        dst->type = dup_str(sxcl_json_get_string(src, "type", "release"));
        dst->url = dup_str(sxcl_json_get_string(src, "url", ""));
        dst->sha1 = dup_str(sxcl_json_get_string(src, "sha1", ""));
        dst->size = sxcl_json_get_int64(src, "size", 0);
    }
    const sxcl_json_value *latest = sxcl_json_get(root, "latest");
    const char *rel = sxcl_json_get_string(latest, "release", "");
    const char *snap = sxcl_json_get_string(latest, "snapshot", "");
    snprintf(list->latest_release, sizeof(list->latest_release), "%s", rel ? rel : "");
    snprintf(list->latest_snapshot, sizeof(list->latest_snapshot), "%s", snap ? snap : "");
    return list;
}

void sxcl_version_list_free(sxcl_version_list *list)
{
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; ++i) {
        free((void *)list->entries[i].id);
        free((void *)list->entries[i].type);
        free((void *)list->entries[i].url);
        free((void *)list->entries[i].sha1);
    }
    free(list->entries);
    free(list);
}

const char *sxcl_version_list_latest_release(const sxcl_version_list *list)
{
    return list ? list->latest_release : "";
}

const char *sxcl_version_list_latest_snapshot(const sxcl_version_list *list)
{
    return list ? list->latest_snapshot : "";
}

size_t sxcl_version_list_count(const sxcl_version_list *list)
{
    return list ? list->count : 0;
}

const sxcl_version_entry *sxcl_version_list_at(const sxcl_version_list *list, size_t index)
{
    if (!list || index >= list->count) {
        return NULL;
    }
    return &list->entries[index];
}

const sxcl_version_entry *sxcl_version_list_find(const sxcl_version_list *list, const char *id)
{
    if (!list || !id) {
        return NULL;
    }
    for (size_t i = 0; i < list->count; ++i) {
        if (list->entries[i].id && strcmp(list->entries[i].id, id) == 0) {
            return &list->entries[i];
        }
    }
    return NULL;
}

/* ── 下载计划 ── */

/* 任务逐个 malloc:计划是"边下边扩"的(先版本 JSON,后资源索引展开 5000+ 条),
 * 而引擎提交后持有的是任务指针 —— 若用连续数组 + realloc,扩容会把地址搬走,
 * 引擎手里的指针立刻变野指针(实测崩在 0xC0000005)。地址稳定是硬要求。 */
static uint64_t fnv1a64(const char *s); /* 定义在资源对象那一节,这里先声明 */

struct sxcl_version_plan {
    sxcl_task **tasks;
    size_t count;
    size_t capacity;
    char **owned; /* 每个任务持有的字符串(路径/URL/摘要/label),释放时统一 free */
    size_t owned_count;
    /* 目标路径去重集(开放寻址,键是指向 tasks[i]->dest 的指针数组)。
     * 为什么必须去重:官方版本 JSON 里**同一个文件可以被列两次**(实测 1.0 的
     * net.java.jinput:jinput-platform:2.0.5 就出现了两遍),于是计划里出现两个 target 完全相同
     * 的任务,两个工作线程同时开同一个 <dest>.part —— 第二个拿到的是共享冲突,
     * 报"无法写入 ...part",于是"13 个成功 1 个失败"(实测就是这个)。
     * 装不下(理论不会)时退化成不去重:行为与老版本一致,不会错,只是可能重复下载。 */
    char **dest_keys;
    size_t dest_cap;
    size_t dest_used;
    /* 「关闭文件校验」档(见 manifest.h 的 sxcl_version_plan_set_skip_existing):
     * 非 0 = 目标文件已经躺在磁盘上的任务一个字节都不下,也不比对大小/哈希。 */
    int skip_existing;
};

/* 目标路径集合:FNV-1a + 开放寻址。返回 1 = 新插入,0 = 已存在,-1 = 装不下。 */
static int plan_dest_seen(sxcl_version_plan *plan, const char *dest)
{
    if (plan->dest_keys == NULL) {
        /* 16384 槽:一次全量安装的普通文件(jar/库/版本 JSON)远小于它;资源对象走另一条路
         * (按哈希去重),所以这个尺寸够用。 */
        plan->dest_cap = 16384;
        plan->dest_keys = (char **)calloc(plan->dest_cap, sizeof(char *));
        plan->dest_used = 0;
        if (plan->dest_keys == NULL) {
            plan->dest_cap = 0;
            return -1;
        }
    }
    if (plan->dest_cap == 0 || plan->dest_used * 10 >= plan->dest_cap * 7) {
        return -1;
    }
    uint64_t hash = fnv1a64(dest);
    size_t i = (size_t)(hash & (uint64_t)(plan->dest_cap - 1));
    while (plan->dest_keys[i] != NULL) {
        if (strcmp(plan->dest_keys[i], dest) == 0) {
            return 0;
        }
        i = (i + 1) & (plan->dest_cap - 1);
    }
    plan->dest_keys[i] = (char *)dest;
    ++plan->dest_used;
    return 1;
}

static char *plan_intern(sxcl_version_plan *plan, const char *s)
{
    char *copy = dup_str(s ? s : "");
    if (!copy) {
        return NULL;
    }
    char **grown = (char **)realloc(plan->owned, (plan->owned_count + 1) * sizeof(char *));
    if (!grown) {
        free(copy);
        return NULL;
    }
    plan->owned = grown;
    plan->owned[plan->owned_count++] = copy;
    return copy;
}

static int plan_add(sxcl_version_plan *plan, const char *url, const char *dest, const char *sha1,
                    int64_t size, int priority, const char *label, const char *mirror_url)
{
    if (!url || !*url || !dest) {
        return 0; /* 缺 URL 的条目直接跳过(元数据里偶有空条目) */
    }
    if (plan_dest_seen(plan, dest) == 0) {
        return 0; /* 同一个目标路径已经排过队:再排一次只会让两个线程抢同一个 .part */
    }
    if (plan->count == plan->capacity) {
        const size_t next = plan->capacity ? plan->capacity * 2 : 64;
        sxcl_task **grown = (sxcl_task **)realloc(plan->tasks, next * sizeof(sxcl_task *));
        if (!grown) {
            return -1;
        }
        plan->tasks = grown;
        plan->capacity = next;
    }
    sxcl_task *t = (sxcl_task *)calloc(1, sizeof(sxcl_task));
    if (!t) {
        return -1;
    }
    plan->tasks[plan->count] = t;
    t->dest = plan_intern(plan, dest);
    t->urls[0] = plan_intern(plan, url);
    t->urls[1] = mirror_url ? plan_intern(plan, mirror_url) : NULL;
    t->urls[2] = NULL;
    t->sha1 = (sha1 && *sha1) ? plan_intern(plan, sha1) : NULL;
    t->algo = SXCL_HASH_SHA1;
    t->size = size;
    t->priority = priority;
    t->label = plan_intern(plan, label ? label : dest);
    if (!t->dest || !t->urls[0] || !t->label) {
        return -1;
    }
    ++plan->count;
    return 0;
}

static char *join_path(const char *dir, const char *rel)
{
    const size_t a = strlen(dir);
    const size_t b = strlen(rel);
    char *p = (char *)malloc(a + b + 2);
    if (!p) {
        return NULL;
    }
    memcpy(p, dir, a);
    size_t n = a;
    if (n > 0 && p[n - 1] != '/' && p[n - 1] != '\\') {
        p[n++] = '/';
    }
    memcpy(p + n, rel, b + 1);
    return p;
}

sxcl_version_plan *sxcl_version_plan_build(const sxcl_json *version_json, const char *game_dir,
                                           const char *version_id, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!version_json || !game_dir || !version_id) {
        if (err) {
            snprintf(err, err_len, "参数不完整");
        }
        return NULL;
    }
    const sxcl_json_value *root = sxcl_json_root(version_json);
    const char *os_name = sxcl_platform_os_name();
    const char *arch_name = sxcl_platform_arch_name();
    sxcl_version_plan *plan = (sxcl_version_plan *)calloc(1, sizeof(sxcl_version_plan));
    if (!plan) {
        return NULL;
    }

    /* 1) 客户端 jar + 资源索引:优先级 0(先下,后面的资源展开要靠索引) */
    const sxcl_json_value *downloads = sxcl_json_get(root, "downloads");
    const sxcl_json_value *client = sxcl_json_get(downloads, "client");
    if (client) {
        char rel[256];
        snprintf(rel, sizeof(rel), "versions/%s/%s.jar", version_id, version_id);
        char *dest = join_path(game_dir, rel);
        if (dest) {
            plan_add(plan, sxcl_json_get_string(client, "url", NULL), dest,
                     sxcl_json_get_string(client, "sha1", NULL), sxcl_json_get_int64(client, "size", 0),
                     0, rel, NULL);
            free(dest);
        }
    }
    const sxcl_json_value *asset_index = sxcl_json_get(root, "assetIndex");
    if (asset_index) {
        const char *index_id = sxcl_json_get_string(asset_index, "id", "assets");
        char rel[256];
        snprintf(rel, sizeof(rel), "assets/indexes/%s.json", index_id);
        char *dest = join_path(game_dir, rel);
        if (dest) {
            plan_add(plan, sxcl_json_get_string(asset_index, "url", NULL), dest,
                     sxcl_json_get_string(asset_index, "sha1", NULL),
                     sxcl_json_get_int64(asset_index, "size", 0), 0, rel, NULL);
            free(dest);
        }
    }

    /* 2) 依赖库:优先级 10;带 rules 的先过滤,natives 分类器按平台键取 */
    const sxcl_json_value *libraries = sxcl_json_get(root, "libraries");
    const size_t lib_count = sxcl_json_size(libraries);
    for (size_t i = 0; i < lib_count; ++i) {
        const sxcl_json_value *lib = sxcl_json_at(libraries, i);
        if (!lib || sxcl_json_type_of(lib) != SXCL_JSON_OBJECT) {
            continue;
        }
        if (!sxcl_rules_allow(sxcl_json_get(lib, "rules"), os_name, arch_name)) {
            continue;
        }
        const sxcl_json_value *lib_dl = sxcl_json_get(lib, "downloads");
        const char *name = sxcl_json_get_string(lib, "name", "library");
        const sxcl_json_value *artifact = sxcl_json_get(lib_dl, "artifact");
        const char *path = sxcl_json_get_string(artifact, "path", NULL);
        if (path) {
            char rel[512];
            snprintf(rel, sizeof(rel), "libraries/%s", path);
            char *dest = join_path(game_dir, rel);
            if (dest) {
                plan_add(plan, sxcl_json_get_string(artifact, "url", NULL), dest,
                         sxcl_json_get_string(artifact, "sha1", NULL),
                         sxcl_json_get_int64(artifact, "size", 0), 10, name, NULL);
                free(dest);
            }
        }
        /* natives 分类器:老版本(LWJGL 2)靠这个拿平台相关的 so/dll */
        const sxcl_json_value *natives = sxcl_json_get(lib, "natives");
        const char *native_key = sxcl_json_get_string(natives, os_name, NULL);
        if (native_key && *native_key) {
            /* 键里可能有 ${arch} 占位符(1.8.x/1.9.x):交给 natives.h 那个函数统一处理,
             * 它顺带把"展开后的键"回填 —— 之前这里按原样查,老版本的原生库根本进不了计划。 */
            char resolved_key[160];
            const sxcl_json_value *cls = sxcl_natives_classifier_of(lib, os_name, resolved_key,
                                                                   sizeof(resolved_key));
            const char *cls_path = sxcl_json_get_string(cls, "path", NULL);
            if (cls_path) {
                char rel[512];
                snprintf(rel, sizeof(rel), "libraries/%s", cls_path);
                char *dest = join_path(game_dir, rel);
                if (dest) {
                    plan_add(plan, sxcl_json_get_string(cls, "url", NULL), dest,
                             sxcl_json_get_string(cls, "sha1", NULL),
                             sxcl_json_get_int64(cls, "size", 0), 10, name, NULL);
                    free(dest);
                }
            }
        }
    }

    if (plan->count == 0) {
        if (err) {
            snprintf(err, err_len, "版本 JSON 里没有可下载的条目");
        }
        sxcl_version_plan_free(plan);
        return NULL;
    }
    return plan;
}

int sxcl_version_plan_add_mirror(sxcl_version_plan *plan, const char *mirror_base, char *err,
                                 size_t err_len)
{
    int added = 0;
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!plan) {
        if (err) {
            snprintf(err, err_len, "计划为空");
        }
        return -1;
    }
    for (size_t i = 0; i < plan->count; ++i) {
        sxcl_task *t = plan->tasks[i];
        char mirror[1024];
        if (!t || !t->urls[0] || t->urls[1] != NULL) {
            continue; /* 已经有第二候选(例如资源对象展开时给过)就不动它 */
        }
        if (sxcl_manifest_mirror_url(t->urls[0], mirror_base, mirror, sizeof(mirror)) != 0) {
            continue; /* 认不出的 URL:没有对应的镜像路,跳过(不是错误) */
        }
        char *owned = plan_intern(plan, mirror);
        if (!owned) {
            if (err) {
                snprintf(err, err_len, "内存不足(镜像 URL 太长或候选太多)");
            }
            return -1;
        }
        t->urls[1] = owned;
        ++added;
    }
    return added;
}

int sxcl_version_plan_prefer_mirror(sxcl_version_plan *plan)
{
    if (!plan) {
        return -1;
    }
    int swapped = 0;
    for (size_t i = 0; i < plan->count; ++i) {
        sxcl_task *t = plan->tasks[i];
        if (!t || !t->urls[0] || !t->urls[1]) {
            continue; /* 没有第二候选:官方仍是唯一的一条路,不动它 */
        }
        /* plan 接管了这些字符串的生命周期(plan->owned),这里只换指针,不复制也不释放 */
        const char *first = t->urls[0];
        t->urls[0] = t->urls[1];
        t->urls[1] = first;
        ++swapped;
    }
    return swapped;
}

/* ── 资源对象展开(assets/objects) ── */

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    return h;
}

/* 5,147 条规模:定容开放寻址集合做"同哈希只入队一次"。装不下就退化为不去重(不会错,只是可能重复下载) */
typedef struct asset_seen {
    uint64_t *slots;
    size_t cap;
    size_t used;
} asset_seen;

static int asset_seen_init(asset_seen *set, size_t cap)
{
    set->slots = (uint64_t *)calloc(cap, sizeof(uint64_t));
    set->cap = set->slots ? cap : 0;
    set->used = 0;
    return set->slots ? 0 : -1;
}

static void asset_seen_free(asset_seen *set)
{
    free(set->slots);
    set->slots = NULL;
}

/** 返回 1 = 新插入,0 = 已存在,-1 = 装不下 */
static int asset_seen_add(asset_seen *set, uint64_t key)
{
    if (!set->slots || set->used * 10 >= set->cap * 7) {
        return -1;
    }
    size_t i = (size_t)(key & (uint64_t)(set->cap - 1));
    while (set->slots[i] != 0) {
        if (set->slots[i] == key) {
            return 0;
        }
        i = (i + 1) & (set->cap - 1);
    }
    set->slots[i] = key;
    ++set->used;
    return 1;
}

int sxcl_version_plan_add_asset_objects(sxcl_version_plan *plan, const sxcl_json *asset_index,
                                        const char *game_dir, const char *base_url,
                                        const char *mirror_base, char *err, size_t err_len)
{
    /* 老入口 = 强校验(装的时候用的就是它) */
    return sxcl_version_plan_add_asset_objects_ex(plan, asset_index, game_dir, base_url, mirror_base,
                                                 1, err, err_len);
}

int sxcl_version_plan_add_asset_objects_ex(sxcl_version_plan *plan, const sxcl_json *asset_index,
                                           const char *game_dir, const char *base_url,
                                           const char *mirror_base, int verify_hash, char *err,
                                           size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!plan || !asset_index || !game_dir) {
        if (err) {
            snprintf(err, err_len, "参数不完整");
        }
        return -1;
    }
    const sxcl_json_value *objects = sxcl_json_get(sxcl_json_root(asset_index), "objects");
    if (!objects || sxcl_json_type_of(objects) != SXCL_JSON_OBJECT) {
        if (err) {
            snprintf(err, err_len, "资源索引里没有 objects 对象");
        }
        return -1;
    }
    const size_t n = sxcl_json_member_count(objects);
    if (n == 0) {
        if (err) {
            snprintf(err, err_len, "objects 为空");
        }
        return -1;
    }
    const char *base = (base_url && *base_url) ? base_url : SXCL_ASSET_OBJECTS_BASE;
    asset_seen seen;
    if (asset_seen_init(&seen, 16384) != 0) {
        seen.slots = NULL;
        seen.cap = 0;
        seen.used = 0;
    }

    int added = 0;
    for (size_t i = 0; i < n; ++i) {
        const char *name = sxcl_json_member_key(objects, i);
        const sxcl_json_value *entry = sxcl_json_member_value(objects, i);
        const char *hash = sxcl_json_get_string(entry, "hash", NULL);
        const int64_t size = sxcl_json_get_int64(entry, "size", 0);
        if (!hash || strlen(hash) < 4 || size <= 0) {
            continue; /* 索引里的坏条目:跳过而不是整体失败 */
        }
        if (seen.slots && asset_seen_add(&seen, fnv1a64(hash) | 1ULL) == 0) {
            continue; /* 同一份内容已被别的名字引用过 */
        }
        char rel[160];
        snprintf(rel, sizeof(rel), "assets/objects/%.2s/%s", hash, hash);
        char *dest = join_path(game_dir, rel);
        if (!dest) {
            continue;
        }
        char url[512];
        snprintf(url, sizeof(url), "%s/%.2s/%s", base, hash, hash);
        char mirror[512];
        const char *mirror_url = NULL;
        if (mirror_base && *mirror_base) {
            snprintf(mirror, sizeof(mirror), "%s/%.2s/%s", mirror_base, hash, hash);
            mirror_url = mirror;
        }
        /* verify_hash=0 -> sha1 传 NULL:引擎按"只比大小"判"已存在",不读文件(启动前那一遍用) */
        plan_add(plan, url, dest, verify_hash ? hash : NULL, size, SXCL_ASSET_OBJECTS_PRIORITY, name,
                 mirror_url);
        ++added;
        free(dest);
    }
    asset_seen_free(&seen);
    return added;
}

void sxcl_version_plan_free(sxcl_version_plan *plan)
{
    if (!plan) {
        return;
    }
    for (size_t i = 0; i < plan->owned_count; ++i) {
        free(plan->owned[i]);
    }
    for (size_t i = 0; i < plan->count; ++i) {
        free(plan->tasks[i]);
    }
    free(plan->owned);
    free(plan->dest_keys);
    free(plan->tasks);
    free(plan);
}

void sxcl_version_plan_set_skip_existing(sxcl_version_plan *plan, int on)
{
    if (plan != NULL) {
        plan->skip_existing = on ? 1 : 0;
    }
}

size_t sxcl_version_plan_count(const sxcl_version_plan *plan)
{
    return plan ? plan->count : 0;
}

sxcl_task *sxcl_version_plan_task(sxcl_version_plan *plan, size_t index)
{
    if (!plan || index >= plan->count) {
        return NULL;
    }
    return plan->tasks[index];
}

int64_t sxcl_version_plan_total_bytes(const sxcl_version_plan *plan)
{
    int64_t total = 0;
    for (size_t i = 0; plan && i < plan->count; ++i) {
        total += plan->tasks[i]->size;
    }
    return total;
}

/* ══════════════════════ 把计划跑一遍(启动前"补全文件") ══════════════════════ */

/** 进度/取消的桥:调用方的 on_progress 要转发(它靠这个显示进度),取消要从这里叫停引擎。 */
typedef struct fetch_bridge {
    const sxcl_engine_opts *opts; /**< 调用方原始 opts:进度回调从这儿转发 */
    sxcl_engine *engine;
    int (*is_cancelled)(void *ud);
    void *cancel_ud;
    int stop;
} fetch_bridge;

/** 传输后端工厂的**转发**:引擎会用 opts->userdata 去调工厂(见 engine.h 的说明),
 *  而我们为了进度桥必须把 userdata 换成 fetch_bridge —— 那样工厂就拿不到调用方自己的
 *  userdata 了(调用方可能在里面放了服务器/连接池;本次实测就是"假后端把 bridge 当自己
 *  的结构体写 → 直接崩")。所以这里显式转发回**调用方那一份**。 */
static sxcl_transport *fetch_transport_factory(void *userdata)
{
    fetch_bridge *b = (fetch_bridge *)userdata;
    return b->opts->transport_factory(b->opts->userdata);
}

static void fetch_on_progress(void *userdata, const sxcl_task *task)
{
    fetch_bridge *b = (fetch_bridge *)userdata;
    if (b->opts->on_progress) {
        b->opts->on_progress(b->opts->userdata, task); /**< 原样转发 */
    }
    if (b->stop) {
        return;
    }
    if (b->is_cancelled && b->is_cancelled(b->cancel_ud)) {
        b->stop = 1;
        sxcl_engine_cancel(b->engine);
    }
}

int sxcl_version_plan_fetch(sxcl_version_plan *plan, const sxcl_engine_opts *opts,
                            int (*is_cancelled)(void *ud), void *cancel_ud, sxcl_fetch_stats *stats,
                            char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
    if (!plan || !opts || !opts->transport_factory) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "没有配置传输后端(engine_opts.transport_factory 为空)");
        }
        return SXCL_FETCH_ERR_ARG;
    }

    const size_t count = sxcl_version_plan_count(plan);
    if (stats) {
        stats->total = (int)count;
    }
    if (count == 0) {
        return SXCL_FETCH_OK;
    }

    fetch_bridge bridge;
    memset(&bridge, 0, sizeof(bridge));
    bridge.opts = opts;
    bridge.is_cancelled = is_cancelled;
    bridge.cancel_ud = cancel_ud;

    sxcl_engine_opts local = *opts;
    local.on_progress = fetch_on_progress;
    local.transport_factory = fetch_transport_factory; /* 见上面的转发理由 */
    local.userdata = &bridge;

    sxcl_engine *engine = sxcl_engine_create(&local);
    if (!engine) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "创建下载引擎失败");
        }
        return SXCL_FETCH_ERR_IO;
    }
    bridge.engine = engine;
    for (size_t i = 0; i < count; ++i) {
        sxcl_task *task = sxcl_version_plan_task(plan, i);
        /* 「关闭文件校验」:存在就算过 —— 直接不入队,也**不做任何比对**
         * (PCL 的 ShouldIgnoreFileCheck 就是把已存在的从列表里全剔掉,见 docs/24 §2.1)。
         * 记成 skipped_existing,让统计与「齐了几个」的口径跟别的路径一致。 */
        if (task != NULL && plan->skip_existing && task->dest != NULL && sxcl_fs_exists(task->dest)) {
            task->skipped_existing = 1;
            task->state = SXCL_TASK_DONE;
            task->bytes_done = 0;
            (void)snprintf(task->error, sizeof(task->error), "%s", "已存在(关闭文件校验,不比对)");
            continue;
        }
        if (task && sxcl_engine_submit(engine, task) != 0) {
            sxcl_engine_destroy(engine);
            if (err && err_len) {
                (void)snprintf(err, err_len, "任务入队失败(第 %d 个)", (int)i);
            }
            return SXCL_FETCH_ERR_IO;
        }
    }
    const int run_failed = sxcl_engine_run(engine);
    const int stopped = bridge.stop;
    sxcl_engine_destroy(engine); /* 任务归计划所有,引擎必须在返回前放掉 */

    /* 统计与第一句人话原因:逐个任务读**结构化字段**(skipped_existing / state / error),
     * 不去比 error 里的中文文案 —— 文案一改统计就会静默归零。 */
    int have_error = 0;
    for (size_t i = 0; i < count; ++i) {
        const sxcl_task *task = sxcl_version_plan_task(plan, i);
        if (!task || !stats) {
            continue;
        }
        if (task->skipped_existing) {
            ++stats->skipped;
            continue;
        }
        if (task->state == SXCL_TASK_DONE) {
            ++stats->downloaded;
            stats->bytes_done += task->bytes_done;
        } else if (task->state == SXCL_TASK_FAILED) {
            ++stats->failed;
            if (!have_error && task->error[0] && err && err_len) {
                (void)snprintf(err, err_len, "%s", task->error);
                have_error = 1;
            }
        }
    }

    if (stopped) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "已取消(补全到一半)");
        }
        return SXCL_FETCH_CANCELLED;
    }
    if ((stats && stats->failed > 0) || run_failed > 0) {
        if (!have_error && err && err_len) {
            (void)snprintf(err, err_len, "有 %d 个文件没补上", stats ? stats->failed : run_failed);
        }
        return SXCL_FETCH_PARTIAL;
    }
    return SXCL_FETCH_OK;
}

