#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/manifest.h"

#include "sxcl/fs.h"

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
struct sxcl_version_plan {
    sxcl_task **tasks;
    size_t count;
    size_t capacity;
    char **owned; /* 每个任务持有的字符串(路径/URL/摘要/label),释放时统一 free */
    size_t owned_count;
};

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
            const sxcl_json_value *classifiers = sxcl_json_get(lib_dl, "classifiers");
            const sxcl_json_value *cls = sxcl_json_get(classifiers, native_key);
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
        plan_add(plan, url, dest, hash, size, SXCL_ASSET_OBJECTS_PRIORITY, name, mirror_url);
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
    free(plan->tasks);
    free(plan);
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
