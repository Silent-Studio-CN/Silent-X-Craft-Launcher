/* 校验层测试:大小/哈希/缺失/大小写/分块一致性 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* sprintf/fopen 在 MSVC 下默认被标记弃用 */
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sxcl/fs.h"
#include "sxcl/hash.h"
#include "sxcl/verify.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

static void check_status(sxcl_verify_status got, sxcl_verify_status want, const char *what) {
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %s want %s\n", what, sxcl_verify_status_name(got),
               sxcl_verify_status_name(want));
    }
}

static int write_file(const char *path, const void *data, size_t len) {
    FILE *fh = fopen(path, "wb");
    if (!fh) {
        return -1;
    }
    const size_t n = len > 0 ? fwrite(data, 1, len, fh) : 0;
    fclose(fh);
    return n == len ? 0 : -1;
}

int main(void) {
    const char *dir = "build/_verify_tmp";
    check(sxcl_fs_mkdirs(dir) == 0, "建临时目录");

    const char *abc = "abc";
    const char *abc_sha1 = "a9993e364706816aba3e25717850c26c9cd0d89d";
    char path[256];
    sprintf(path, "%s/abc.bin", dir);
    check(write_file(path, abc, 3) == 0, "写 abc.bin");

    /* 1) 正确哈希 + 正确大小 */
    sxcl_verify_result res;
    check_status(sxcl_verify_file(path, 3, abc_sha1, SXCL_HASH_SHA1, &res), SXCL_VERIFY_OK,
                 "正确 SHA-1 + 大小");
    check(res.size == 3, "结果里带实际大小");
    check(strcmp(res.actual_hex, abc_sha1) == 0, "结果里带实际摘要");

    /* 2) 大写十六进制也应通过(比较不区分大小写) */
    char upper[41];
    for (int i = 0; i < 40; ++i) {
        upper[i] = (char)((abc_sha1[i] >= 'a' && abc_sha1[i] <= 'f') ? abc_sha1[i] - 32 : abc_sha1[i]);
    }
    upper[40] = '\0';
    check_status(sxcl_verify_file(path, 3, upper, SXCL_HASH_SHA1, NULL), SXCL_VERIFY_OK,
                 "大写哈希同样通过");

    /* 3) 大小不符 -> SIZE(不该白算哈希) */
    check_status(sxcl_verify_file(path, 4, abc_sha1, SXCL_HASH_SHA1, NULL), SXCL_VERIFY_SIZE,
                 "大小不符");

    /* 4) 哈希不符 */
    check_status(sxcl_verify_file(path, 3, "da39a3ee5e6b4b0d3255bfef95601890afd80709", SXCL_HASH_SHA1, NULL),
                 SXCL_VERIFY_HASH, "哈希不符");

    /* 5) 只校验大小(不给哈希) */
    check_status(sxcl_verify_file(path, 3, NULL, SXCL_HASH_SHA1, NULL), SXCL_VERIFY_OK, "仅校验大小");
    check_status(sxcl_verify_file(path, 3, "", SXCL_HASH_SHA1, NULL), SXCL_VERIFY_OK, "空哈希=仅大小");

    /* 6) 不校验大小也不校验哈希 */
    check_status(sxcl_verify_file(path, 0, NULL, SXCL_HASH_SHA1, NULL), SXCL_VERIFY_OK, "两者都不校验");

    /* 7) 文件不存在 */
    check_status(sxcl_verify_file("build/_verify_tmp/不存在.bin", 0, NULL, SXCL_HASH_SHA1, NULL),
                 SXCL_VERIFY_MISSING, "文件不存在");

    /* 8) 分块一致性:5MiB 文件(跨 4MiB 分块)与一次性摘要必须一致 */
    const size_t big = 5u * 1024u * 1024u;
    unsigned char *buf = (unsigned char *)malloc(big);
    if (!buf) {
        check(0, "分配 5MiB 缓冲");
    } else {
        for (size_t i = 0; i < big; ++i) {
            buf[i] = (unsigned char)((i * 31u + 7u) & 0xFFu);
        }
        sprintf(path, "%s/big.bin", dir);
        check(write_file(path, buf, big) == 0, "写 5MiB 文件");

        char hex_file[65];
        char hex_mem[65];
        check(sxcl_hash_file(path, SXCL_HASH_SHA1, hex_file, sizeof(hex_file)) == 0, "文件哈希(分块)");
        check(sxcl_hash_digest(SXCL_HASH_SHA1, buf, big, hex_mem, sizeof(hex_mem)) == 0, "内存摘要(一次性)");
        check(strcmp(hex_file, hex_mem) == 0, "跨分块结果一致");

        check_status(sxcl_verify_file(path, (int64_t)big, hex_mem, SXCL_HASH_SHA1, NULL), SXCL_VERIFY_OK,
                     "5MiB 文件整链校验");
        check_status(sxcl_verify_file(path, (int64_t)big + 1, hex_mem, SXCL_HASH_SHA1, NULL),
                     SXCL_VERIFY_SIZE, "5MiB 大小差 1 字节即不通过");
        free(buf);
    }

    /* 9) SHA-256 路径也要能走通:拿回 abc.bin 的路径(上面 path 已指向 5MiB 文件) */
    sprintf(path, "%s/abc.bin", dir);

    char hex256[65];
    check(sxcl_hash_digest(SXCL_HASH_SHA256, abc, 3, hex256, sizeof(hex256)) == 0, "SHA-256(abc)");
    check(strcmp(hex256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "SHA-256(abc) 摘要正确");
    check_status(sxcl_verify_file(path, 3, hex256, SXCL_HASH_SHA256, NULL), SXCL_VERIFY_OK,
                 "SHA-256 正确摘要通过");
    /* 拿 SHA-1 的摘要去比 SHA-256:长度都不同,必须报哈希不符而不是"通过" */
    check_status(sxcl_verify_file(path, 3, abc_sha1, SXCL_HASH_SHA256, NULL), SXCL_VERIFY_HASH,
                 "算法用错(SHA-1 摘要配 SHA-256)必须报哈希不符");
    check_status(sxcl_verify_file(path, 3, hex256, SXCL_HASH_SHA1, NULL), SXCL_VERIFY_HASH,
                 "算法用错(SHA-256 摘要配 SHA-1)必须报哈希不符");

    printf("校验层测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("[PASS] 校验层语义与 Python 版一致\n");
    }
    return g_fail == 0 ? 0 : 1;
}
