/* 哈希缓存测试:键语义 / 落盘往返 / 坏文件容错 / prune_missing / 扩容 / 上限保护。
 * 断言式:任何一项失败,main 返回 1。临时文件放在 build/ 下,测试结束删掉。 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* fopen/fprintf 在 MSVC 下默认被标记弃用 */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_MSC_VER) && defined(_DEBUG)
#  include <crtdbg.h>   /* 仅 Debug 构建:退出前用 CRT 调试堆查一次泄漏 */
#endif

#include "sxcl/fs.h"
#include "sxcl/hashcache.h"
#include "../src/core/internal/platform_thread.h" /* 线程封装是内部的,测试多线程只能借它 */

#define TEMP_DIR "build/_hashcache_tmp"

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

static double now_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static int write_bytes(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    const size_t n = (len > 0u) ? fwrite(data, 1u, len, f) : 0u;
    if (fclose(f) != 0) {
        return -1;
    }
    return (n == len) ? 0 : -1;
}

static int hex_is(const char *got, const char *want)
{
    return got != NULL && strcmp(got, want) == 0;
}

static int file_has_text(const char *path, const char *needle)
{
    char buf[4096];
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    const size_t n = fread(buf, 1u, sizeof buf - 1u, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* ── 1) 纯内存:往返、键语义、覆盖、删除、NULL 安全 ── */
static void test_memory_basics(void)
{
    printf("[1] 纯内存语义\n");
    sxcl_hash_cache *c = sxcl_hash_cache_open(NULL);
    check(c != NULL, "打开纯内存缓存");
    if (!c) {
        return;
    }
    check(sxcl_hash_cache_count(c) == 0u, "新缓存条目数为 0");
    check(sxcl_hash_cache_get(c, "a.jar", 10, 20) == NULL, "未命中返回 NULL");

    sxcl_hash_cache_put(c, "a.jar", 10, 20, "AAAA");
    const char *got = sxcl_hash_cache_get(c, "a.jar", 10, 20);
    check(got != NULL && strcmp(got, "AAAA") == 0, "put/get 往返");
    check(sxcl_hash_cache_count(c) == 1u, "条目数 1");

    check(sxcl_hash_cache_get(c, "a.jar", 11, 20) == NULL, "同 path 不同 size = 另一条");
    check(sxcl_hash_cache_get(c, "a.jar", 10, 21) == NULL, "同 path 不同 mtime = 另一条");
    check(sxcl_hash_cache_get(c, "b.jar", 10, 20) == NULL, "path 不同 = 未命中");
    check(sxcl_hash_cache_count(c) == 1u, "未命中的查询不产生条目");

    sxcl_hash_cache_put(c, "a.jar", 10, 20, "BBBB");
    got = sxcl_hash_cache_get(c, "a.jar", 10, 20);
    check(got != NULL && strcmp(got, "BBBB") == 0, "同键覆盖为新摘要");
    check(sxcl_hash_cache_count(c) == 1u, "覆盖不增加条目数");

    sxcl_hash_cache_put(c, "a.jar", 11, 20, "CCCC");
    sxcl_hash_cache_put(c, "a.jar", 10, 21, "DDDD");
    check(sxcl_hash_cache_count(c) == 3u, "size/mtime 各自成为新条目");
    check(hex_is(sxcl_hash_cache_get(c, "a.jar", 11, 20), "CCCC"), "size 变体的摘要独立");
    check(hex_is(sxcl_hash_cache_get(c, "a.jar", 10, 21), "DDDD"), "mtime 变体的摘要独立");

    check(sxcl_hash_cache_remove(c, "a.jar", 10, 20) == 0, "remove 返回 0");
    check(sxcl_hash_cache_get(c, "a.jar", 10, 20) == NULL, "remove 后未命中");
    check(sxcl_hash_cache_count(c) == 2u, "remove 后条目数减一");
    check(sxcl_hash_cache_remove(c, "a.jar", 10, 20) == 0, "重复 remove 不算错误");
    check(sxcl_hash_cache_remove(c, "不存在", 1, 2) == 0, "remove 不存在的键不算错误");

    /* NULL / 空参数一律不崩 */
    check(sxcl_hash_cache_get(NULL, "a.jar", 0, 0) == NULL, "get(NULL) 返回 NULL");
    check(sxcl_hash_cache_get(c, NULL, 0, 0) == NULL, "get(path=NULL) 返回 NULL");
    check(sxcl_hash_cache_count(NULL) == 0u, "count(NULL) = 0");
    check(sxcl_hash_cache_remove(NULL, "x", 0, 0) == -1, "remove(NULL) 返回 -1");
    check(sxcl_hash_cache_prune_missing(NULL) == 0u, "prune(NULL) = 0");
    check(sxcl_hash_cache_save(NULL) == 0, "save(NULL) = 0");
    sxcl_hash_cache_put(NULL, "x", 0, 0, "00");
    sxcl_hash_cache_put(c, NULL, 0, 0, "00");
    sxcl_hash_cache_put(c, "x", 0, 0, NULL);
    check(sxcl_hash_cache_count(c) == 2u, "非法 put 不改变条目数");
    sxcl_hash_cache_close(NULL);
    sxcl_hash_cache_close(c);
    check(1, "close(NULL) 与 close(缓存) 不崩");
}

/* ── 2) 落盘 + 重新打开 ── */
static void test_persist(void)
{
    printf("[2] 落盘与重新打开\n");
    const char *path = TEMP_DIR "/nested/deep/hashes.txt";   /* 多级目录:顺带验 mkdirs */
    sxcl_fs_remove(path);
    sxcl_fs_remove(TEMP_DIR "/nested/deep/hashes.txt.tmp");

    sxcl_hash_cache *c = sxcl_hash_cache_open(path);
    check(c != NULL, "打开(文件还不存在)");
    check(sxcl_hash_cache_count(c) == 0u, "不存在的文件 = 空缓存");
    check(sxcl_hash_cache_save(c) == 0, "没有改动时 save 直接返回 0");
    check(sxcl_fs_exists(path) == 0, "没有改动时不该创建文件");

    sxcl_hash_cache_put(c, "assets/objects/ab/abcdef0123456789", 461000, 1700000000000000000LL, "deadbeef");
    sxcl_hash_cache_put(c, "libraries/org/ow2/asm/asm.jar", 122000, 1700000000000000001LL, "cafebabe");
    check(sxcl_hash_cache_save(c) == 0, "save 返回 0");
    check(sxcl_fs_exists(path) == 1, "缓存文件已生成");
    check(sxcl_fs_exists(TEMP_DIR "/nested/deep/hashes.txt.tmp") == 0, ".tmp 已被改名,没有残留");
    check(file_has_text(path, "#sxcl-hashcache 1"), "第一行是版本标记");
    check(file_has_text(path, "deadbeef\t461000\t1700000000000000000\tassets/objects/ab/abcdef0123456789"),
          "条目行格式为 hex<TAB>size<TAB>mtime<TAB>path");
    check(sxcl_hash_cache_save(c) == 0, "落盘后再 save 仍是 0");
    sxcl_hash_cache_close(c);

    sxcl_hash_cache *c2 = sxcl_hash_cache_open(path);
    check(c2 != NULL, "重新打开");
    check(sxcl_hash_cache_count(c2) == 2u, "重新打开后条目数 2");
    const char *got = sxcl_hash_cache_get(c2, "assets/objects/ab/abcdef0123456789", 461000,
                                        1700000000000000000LL);
    check(got != NULL && strcmp(got, "deadbeef") == 0, "重新打开后仍命中(第一条)");
    got = sxcl_hash_cache_get(c2, "libraries/org/ow2/asm/asm.jar", 122000, 1700000000000000001LL);
    check(got != NULL && strcmp(got, "cafebabe") == 0, "重新打开后仍命中(第二条)");
    check(sxcl_hash_cache_get(c2, "libraries/org/ow2/asm/asm.jar", 122001, 1700000000000000001LL) == NULL,
          "重新打开后 size 变体仍不命中");
    sxcl_hash_cache_close(c2);
    check(sxcl_fs_remove(path) == 0, "删除临时缓存文件");
}

/* ── 3) 坏文件(半截/乱字符/空)当作空缓存,且还能恢复正常 ── */
static void test_bad_files(void)
{
    printf("[3] 坏文件容错\n");
    const char *half = TEMP_DIR "/bad_half.txt";
    const char *junk = TEMP_DIR "/bad_junk.txt";
    const char *empty = TEMP_DIR "/bad_empty.txt";

    /* 半截:版本行 + 一条好记录 + 一条没写完的记录(无换行);另有少字段的行 */
    const char *half_text = "#sxcl-hashcache 1\n"
                            "aaaa\t1\t2\tpath/one\n"
                            "只有一列\n"
                            "bbbb\tnot-a-number\t2\tpath/two\n"
                            "cccc\t3\t4\tpath/thre";
    check(write_bytes(half, half_text, strlen(half_text)) == 0, "写半截文件");
    /* 乱字符:连版本行都不对 */
    const char *junk_text = "\xef\xbb\xbf这不是缓存\nrandom junk\x01\x02\r\nhex\tsize\tmtime\tpath\n";
    check(write_bytes(junk, junk_text, strlen(junk_text)) == 0, "写乱字符文件");
    check(write_bytes(empty, "", 0) == 0, "写空文件");

    sxcl_hash_cache *c = sxcl_hash_cache_open(half);
    check(c != NULL, "打开半截文件");
    check(sxcl_hash_cache_count(c) == 1u, "半截文件只认那条完整的记录");
    const char *got = sxcl_hash_cache_get(c, "path/one", 1, 2);
    check(got != NULL && strcmp(got, "aaaa") == 0, "完整记录仍可用");
    check(sxcl_hash_cache_get(c, "path/two", 0, 2) == NULL, "坏行被忽略");
    check(sxcl_hash_cache_get(c, "path/thre", 3, 4) == NULL, "末行半截(无换行)被丢弃");
    sxcl_hash_cache_close(c);

    c = sxcl_hash_cache_open(junk);
    check(c != NULL && sxcl_hash_cache_count(c) == 0u, "乱字符文件 = 空缓存");
    sxcl_hash_cache_close(c);

    c = sxcl_hash_cache_open(empty);
    check(c != NULL && sxcl_hash_cache_count(c) == 0u, "空文件 = 空缓存");
    check(sxcl_hash_cache_get(c, "x", 1, 1) == NULL, "空缓存查询不崩");
    check(sxcl_hash_cache_prune_missing(c) == 0u, "空缓存 prune 不崩");
    sxcl_hash_cache_close(c);

    /* 坏文件被打开后,put + save 必须能恢复正常。
     * 注意半截文件里本来有 1 条好记录(会被保留),另两个是 0 条,所以期望条数不同。 */
    const char *targets[3];
    const size_t expect[3] = {2u, 1u, 1u};
    targets[0] = half;
    targets[1] = junk;
    targets[2] = empty;
    for (int i = 0; i < 3; ++i) {
        sxcl_hash_cache *cc = sxcl_hash_cache_open(targets[i]);
        check(cc != NULL, "重开坏文件");
        sxcl_hash_cache_put(cc, "recovered/file.bin", 7, 8, "00112233");
        check(sxcl_hash_cache_save(cc) == 0, "坏文件上 save 成功");
        sxcl_hash_cache_close(cc);
        sxcl_hash_cache *cr = sxcl_hash_cache_open(targets[i]);
        check(cr != NULL && sxcl_hash_cache_count(cr) == expect[i], "坏文件已恢复正常");
        const char *h = sxcl_hash_cache_get(cr, "recovered/file.bin", 7, 8);
        check(h != NULL && strcmp(h, "00112233") == 0, "恢复后的条目可命中");
        sxcl_hash_cache_close(cr);
        check(sxcl_fs_remove(targets[i]) == 0, "删除临时坏文件");
    }
}

/* ── 4) prune_missing:只清掉文件真的不在的那条 ── */
static void test_prune(void)
{
    printf("[4] prune_missing\n");
    const char *f1 = TEMP_DIR "/prune_a.bin";
    const char *f2 = TEMP_DIR "/prune_b.bin";
    check(write_bytes(f1, "hello", 5) == 0, "造文件 a");
    check(write_bytes(f2, "world", 5) == 0, "造文件 b");

    int64_t s1 = 0;
    int64_t m1 = 0;
    int64_t s2 = 0;
    int64_t m2 = 0;
    check(sxcl_fs_stat(f1, &s1, &m1) == 0, "stat a");
    check(sxcl_fs_stat(f2, &s2, &m2) == 0, "stat b");

    sxcl_hash_cache *c = sxcl_hash_cache_open(NULL);
    check(c != NULL, "打开纯内存缓存(prune 用)");
    if (!c) {
        return;
    }
    sxcl_hash_cache_put(c, f1, s1, m1, "1111");
    sxcl_hash_cache_put(c, f2, s2, m2, "2222");
    sxcl_hash_cache_put(c, TEMP_DIR "/绝对不存在.bin", 1, 2, "3333");
    check(sxcl_hash_cache_count(c) == 3u, "3 条待 prune");

    check(sxcl_fs_remove(f1) == 0, "删掉文件 a");
    const size_t removed = sxcl_hash_cache_prune_missing(c);
    check(removed == 2u, "prune 只清掉 a 和那条路径不存在的");
    check(sxcl_hash_cache_count(c) == 1u, "prune 后剩 1 条");
    const char *got = sxcl_hash_cache_get(c, f2, s2, m2);
    check(got != NULL && strcmp(got, "2222") == 0, "文件 b 的条目保留且仍命中");
    check(sxcl_hash_cache_get(c, f1, s1, m1) == NULL, "文件 a 的条目已清掉");
    check(sxcl_hash_cache_prune_missing(c) == 0u, "再 prune 无事可做");
    sxcl_hash_cache_close(c);
    check(sxcl_fs_remove(f2) == 0, "删除文件 b");
}

/* ── 5) 扩容:5000 条随机键值全部命中 ── */
static void test_growth(void)
{
    printf("[5] 扩容(5000 条)\n");
    enum { N = 5000 };
    sxcl_hash_cache *c = sxcl_hash_cache_open(NULL);
    check(c != NULL, "打开纯内存缓存(扩容用)");
    if (!c) {
        return;
    }
    char path[128];
    char hex[64];
    unsigned seed = 12345u;
    const double t0 = now_ms();
    for (int i = 0; i < N; ++i) {
        seed = seed * 1103515245u + 12345u;                     /* 简单的确定性伪随机 */
        const unsigned r = (seed >> 8) & 0xFFFFFFu;
        (void)snprintf(path, sizeof path, "assets/objects/%02x/%08x.bin", r & 0xFFu, r);
        const int64_t size = (int64_t)(r % 100000u) + 1;
        const int64_t mtime = (int64_t)r * 1000003LL;
        (void)snprintf(hex, sizeof hex, "%040x", r);
        sxcl_hash_cache_put(c, path, size, mtime, hex);
    }
    const double t1 = now_ms();
    check(sxcl_hash_cache_count(c) == (size_t)N, "5000 条全部落下");
    check(sxcl_hash_cache_count(c) >= (size_t)N, "条目数没被上限截断");

    int hits = 0;
    seed = 12345u;
    for (int i = 0; i < N; ++i) {
        seed = seed * 1103515245u + 12345u;
        const unsigned r = (seed >> 8) & 0xFFFFFFu;
        (void)snprintf(path, sizeof path, "assets/objects/%02x/%08x.bin", r & 0xFFu, r);
        const int64_t size = (int64_t)(r % 100000u) + 1;
        const int64_t mtime = (int64_t)r * 1000003LL;
        (void)snprintf(hex, sizeof hex, "%040x", r);
        const char *got = sxcl_hash_cache_get(c, path, size, mtime);
        if (got != NULL && strcmp(got, hex) == 0) {
            ++hits;
        }
    }
    const double t2 = now_ms();
    check(hits == N, "5000 条逐条 get 全部命中且摘要正确");
    printf("    5000 条 put: %.1f ms; 5000 条 get: %.1f ms\n", t1 - t0, t2 - t1);
    sxcl_hash_cache_close(c);
}

/* ── 6) 上限:20000 条插入后仍能逐条 get,且不会无限增长 ── */
static void test_cap_scale(void)
{
    printf("[6] 上限规模(20000 条)\n");
    enum { N = 20000 };
    sxcl_hash_cache *c = sxcl_hash_cache_open(NULL);
    check(c != NULL, "打开纯内存缓存(上限用)");
    if (!c) {
        return;
    }
    char path[96];
    char hex[64];
    const double t0 = now_ms();
    for (int i = 0; i < N; ++i) {
        (void)snprintf(path, sizeof path, "versions/1.20.4/libraries/%05x/lib.jar", (unsigned)i);
        (void)snprintf(hex, sizeof hex, "%040x", (unsigned)i * 2654435761u);
        sxcl_hash_cache_put(c, path, 1000 + (int64_t)i, 1700000000000LL + (int64_t)i, hex);
    }
    const double t1 = now_ms();
    check(sxcl_hash_cache_count(c) == (size_t)N, "20000 条全部落下");

    int hits = 0;
    for (int i = 0; i < N; ++i) {
        (void)snprintf(path, sizeof path, "versions/1.20.4/libraries/%05x/lib.jar", (unsigned)i);
        (void)snprintf(hex, sizeof hex, "%040x", (unsigned)i * 2654435761u);
        const char *got = sxcl_hash_cache_get(c, path, 1000 + (int64_t)i,
                                              1700000000000LL + (int64_t)i);
        if (got != NULL && strcmp(got, hex) == 0) {
            ++hits;
        }
    }
    const double t2 = now_ms();
    check(hits == N, "20000 条逐条 get 全部命中");
    check(sxcl_hash_cache_count(c) == (size_t)N, "get 之后条目数不变");
    printf("    20000 条 put: %.1f ms; 20000 条 get: %.1f ms\n", t1 - t0, t2 - t1);

    /* 再写一条就超上限:这些路径都不存在,prune 会清掉,但绝不许超过上限、也绝不许崩 */
    sxcl_hash_cache_put(c, "overflow/newest.bin", 1, 2, "OVERFLOW");
    check(sxcl_hash_cache_count(c) <= (size_t)N, "超限后条目数不超过上限");
    const char *got = sxcl_hash_cache_get(c, "overflow/newest.bin", 1, 2);
    check(got != NULL && strcmp(got, "OVERFLOW") == 0, "超限那一次的写入仍然生效");
    printf("    超限后条目数: %zu\n", sxcl_hash_cache_count(c));
    sxcl_hash_cache_close(c);
}

/* ── 7) 上限的"整体清空"分支:路径都存在,prune 清不掉,只能清空 ── */
static void test_cap_clear(void)
{
    printf("[7] 上限清空分支\n");
    enum { N = 20000 };
    const char *real = TEMP_DIR "/cap_real.bin";
    check(write_bytes(real, "x", 1) == 0, "造一个真实文件");
    sxcl_hash_cache *c = sxcl_hash_cache_open(NULL);
    check(c != NULL, "打开纯内存缓存(清空分支用)");
    if (!c) {
        return;
    }
    /* 同一个真实路径 + 20000 个不同 size = 20000 条都"文件还在"的记录 */
    for (int i = 0; i < N; ++i) {
        sxcl_hash_cache_put(c, real, (int64_t)i + 1, 7, "7777");
    }
    check(sxcl_hash_cache_count(c) == (size_t)N, "20000 条(路径都真实存在)");
    sxcl_hash_cache_put(c, real, (int64_t)N + 1, 7, "8888");
    check(sxcl_hash_cache_count(c) <= (size_t)N, "超限后条目数不超过上限");
    const char *got = sxcl_hash_cache_get(c, real, (int64_t)N + 1, 7);
    check(got != NULL && strcmp(got, "8888") == 0, "清空后本次写入仍在(可命中)");
    printf("    清空分支后条目数: %zu\n", sxcl_hash_cache_count(c));
    sxcl_hash_cache_close(c);
    check(sxcl_fs_remove(real) == 0, "删除真实文件");
}

/* ── 8) 删除后的探测链:密集簇里删掉一半,剩下的必须全都还能查到 ── */
static void test_erase_chains(void)
{
    printf("[8] 删除后的探测链\n");
    enum { N = 5000 };
    sxcl_hash_cache *c = sxcl_hash_cache_open(NULL);
    check(c != NULL, "打开纯内存缓存(删除链用)");
    if (!c) {
        return;
    }
    char path[96];
    char hex[64];
    for (int i = 0; i < N; ++i) {
        (void)snprintf(path, sizeof path, "chain/%05d.bin", i);
        (void)snprintf(hex, sizeof hex, "%040x", (unsigned)i);
        sxcl_hash_cache_put(c, path, i, i, hex);
    }
    check(sxcl_hash_cache_count(c) == (size_t)N, "5000 条落下");
    size_t removed = 0;
    for (int i = 1; i < N; i += 2) {
        (void)snprintf(path, sizeof path, "chain/%05d.bin", i);
        if (sxcl_hash_cache_remove(c, path, i, i) == 0) {
            ++removed;
        }
    }
    check(removed == (size_t)(N / 2), "删掉一半(2500 条)");
    check(sxcl_hash_cache_count(c) == (size_t)(N / 2), "条目数减半");
    int keep_hits = 0;
    int gone_hits = 0;
    for (int i = 0; i < N; ++i) {
        (void)snprintf(path, sizeof path, "chain/%05d.bin", i);
        (void)snprintf(hex, sizeof hex, "%040x", (unsigned)i);
        const char *got = sxcl_hash_cache_get(c, path, i, i);
        if (i % 2 == 0) {
            if (hex_is(got, hex)) {
                ++keep_hits;
            }
        } else if (got != NULL) {
            ++gone_hits;
        }
    }
    check(keep_hits == N / 2, "剩下的一半全部仍能查到(探测链没被删断)");
    check(gone_hits == 0, "删掉的一半全部查不到(没有残留)");
    sxcl_hash_cache_close(c);
}

/* ── 9) 线程安全:多线程同时 put/get/remove 同一张表 ── */
typedef struct mt_arg {
    sxcl_hash_cache *cache;
    int index;      /* 线程号:每个线程只碰自己的键空间,避免读到别人刚释放的串 */
    int iterations;
    int failures;   /* 线程自己的失败数,合并前不碰全局计数 */
} mt_arg;

SXCL_THREAD_FN(mt_worker)
{
    mt_arg *a = (mt_arg *)arg;
    char path[96];
    char old[96];
    char hex[64];
    for (int j = 0; j < a->iterations; ++j) {
        (void)snprintf(path, sizeof path, "mt/%d/%d.bin", a->index, j);
        (void)snprintf(hex, sizeof hex, "%02x%038x", (unsigned)a->index, (unsigned)j * 2246822519u);
        sxcl_hash_cache_put(a->cache, path, 100 + (int64_t)j, 200 + (int64_t)j, hex);
        const char *got = sxcl_hash_cache_get(a->cache, path, 100 + (int64_t)j, 200 + (int64_t)j);
        if (!hex_is(got, hex)) {
            ++a->failures;
        }
        if (j % 10 == 9) {                       /* 每 10 条删掉自己最早的那条 */
            (void)snprintf(old, sizeof old, "mt/%d/%d.bin", a->index, j - 9);
            (void)sxcl_hash_cache_remove(a->cache, old, 100 + (int64_t)(j - 9), 200 + (int64_t)(j - 9));
        }
    }
    SXCL_THREAD_RETURN(0);
}

static void test_threads(void)
{
    printf("[9] 多线程\n");
    enum { THREADS = 4, ITERS = 2000 };
    sxcl_hash_cache *c = sxcl_hash_cache_open(NULL);
    check(c != NULL, "打开纯内存缓存(多线程用)");
    if (!c) {
        return;
    }
    mt_arg args[THREADS];
    sxcl_thread_t tids[THREADS];
    int started = 0;
    for (int i = 0; i < THREADS; ++i) {
        args[i].cache = c;
        args[i].index = i;
        args[i].iterations = ITERS;
        args[i].failures = 0;
        if (sxcl_thread_start(&tids[i], mt_worker, &args[i]) != 0) {
            break;
        }
        ++started;
    }
    check(started == THREADS, "4 个线程全部启动");
    for (int i = 0; i < started; ++i) {
        sxcl_thread_join(tids[i]);
    }
    int failures = 0;
    for (int i = 0; i < started; ++i) {
        failures += args[i].failures;
    }
    check(failures == 0, "并发 put/get 没有一次读错或读不到");
    check(sxcl_hash_cache_count(c) == (size_t)(THREADS * (ITERS - ITERS / 10)),
          "并发 put/remove 之后条目数正确");
    printf("    4 线程 x 2000 轮 put/get/remove,条目数 %zu\n", sxcl_hash_cache_count(c));
    sxcl_hash_cache_close(c);
}

static void cleanup(void)
{
    (void)sxcl_fs_remove(TEMP_DIR "/nested/deep/hashes.txt");
    (void)sxcl_fs_remove(TEMP_DIR "/nested/deep/hashes.txt.tmp");
    (void)sxcl_fs_remove(TEMP_DIR "/bad_half.txt");
    (void)sxcl_fs_remove(TEMP_DIR "/bad_junk.txt");
    (void)sxcl_fs_remove(TEMP_DIR "/bad_empty.txt");
    (void)sxcl_fs_remove(TEMP_DIR "/prune_a.bin");
    (void)sxcl_fs_remove(TEMP_DIR "/prune_b.bin");
    (void)sxcl_fs_remove(TEMP_DIR "/cap_real.bin");
}

int main(void)
{
    check(sxcl_fs_mkdirs(TEMP_DIR) == 0, "建临时目录");
    test_memory_basics();
    test_persist();
    test_bad_files();
    test_prune();
    test_growth();
    test_cap_scale();
    test_cap_clear();
    test_erase_chains();
    test_threads();
    cleanup();

#if defined(_MSC_VER) && defined(_DEBUG)
    check(_CrtDumpMemoryLeaks() == 0, "Debug CRT 未检测到内存泄漏(报告只在调试输出里)");
#endif
    printf("哈希缓存测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("[PASS] 哈希缓存语义与 Python 版一致(键=path|size|mtime_ns,上限 20000)\n");
    }
    return g_fail == 0 ? 0 : 1;
}
