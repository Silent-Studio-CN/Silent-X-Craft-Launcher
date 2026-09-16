/* options.txt 读写测试:格式、顺序、bare 行、原子写、边界 */
#include <stdio.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/options.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *what) {
    if (ok) { ++g_pass; } else { ++g_fail; printf("  [!!] %s\n", what); }
}
static void check_str(const char *got, const char *want, const char *what) {
    if (got && want && strcmp(got, want) == 0) { ++g_pass; }
    else { ++g_fail; printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)"); }
}

static int write_file(const char *path, const char *text) {
    FILE *fh = fopen(path, "wb");
    if (!fh) { return -1; }
    fputs(text, fh);
    fclose(fh);
    return 0;
}
static char *read_file(const char *path, char *buf, size_t n) {
    FILE *fh = fopen(path, "rb");
    if (!fh) { return NULL; }
    const size_t got = fread(buf, 1, n - 1, fh);
    buf[got] = '\0';
    fclose(fh);
    return buf;
}

int main(void) {
    const char *path = "build/_options_tmp/options.txt";
    char buf[2048];
    check(sxcl_fs_mkdirs("build/_options_tmp") == 0, "建临时目录(否则后面写文件全失败)");
    /* 1) 不存在 = 空表 */
    {
        sxcl_options *o = sxcl_options_load("build/_options_tmp/不存在.txt");
        check(o != NULL, "文件不存在也返回可用句柄");
        check(sxcl_options_count(o) == 0, "空表 count=0");
        check(sxcl_options_get(o, "graphicsApi") == NULL, "取不存在的键返回 NULL");
        check(sxcl_options_set(o, "graphicsApi", "vulkan") == 0, "空表也能设值");
        check_str(sxcl_options_get(o, "graphicsApi"), "vulkan", "设完能取到");
        sxcl_options_free(o);
    }
    /* 2) 读真实格式(含 CRLF、bare 行、值里带冒号) */
    check(write_file(path, "version:26.2\r\nfov:0.5\r\ngraphicsApi:default\r\n"
                           "resourcePacks:[\"vanilla\"]\r\nkey_key.hotbar:key.hotbar.1\r\nbareline\r\n") == 0,
          "写测试文件");
    {
        sxcl_options *o = sxcl_options_load(path);
        check(o != NULL, "加载");
        check(sxcl_options_count(o) == 6, "6 条(含 bare 行)");
        check_str(sxcl_options_get(o, "version"), "26.2", "CRLF 被剥掉");
        check_str(sxcl_options_get(o, "graphicsApi"), "default", "读到 graphicsApi");
        check_str(sxcl_options_get(o, "key_key.hotbar"), "key.hotbar.1", "值里带冒号只按第一个冒号切");
        check_str(sxcl_options_get(o, "resourcePacks"), "[\"vanilla\"]", "值里的引号原样");
        check_str(sxcl_options_get(o, "bareline"), "", "bare 行的值是空串");

        /* 3) 改值:原地改,顺序不变 */
        check(sxcl_options_set(o, "graphicsApi", "vulkan") == 0, "改 graphicsApi");
        check_str(sxcl_options_key_at(o, 2), "graphicsApi", "改值不改变位置");
        /* 4) 新增:追加到末尾 */
        check(sxcl_options_set(o, "newKey", "1") == 0, "新增键");
        check_str(sxcl_options_key_at(o, sxcl_options_count(o) - 1), "newKey", "新增键在末尾");
        /* 5) 删除 */
        check(sxcl_options_remove(o, "fov") == 0, "删除 fov");
        check(sxcl_options_get(o, "fov") == NULL, "删掉后取不到");
        check(sxcl_options_remove(o, "不存在") == 0, "删不存在的键不算错");
        /* 6) 原子写 + 回读 */
        check(sxcl_options_save(o, path) == 0, "保存");
        sxcl_options_free(o);

        sxcl_options *r = sxcl_options_load(path);
        check_str(sxcl_options_get(r, "graphicsApi"), "vulkan", "保存后回读 graphicsApi");
        check_str(sxcl_options_get(r, "bareline"), "", "bare 行被保留");
        check_str(sxcl_options_get(r, "newKey"), "1", "新增键被保留");
        check(sxcl_options_get(r, "fov") == NULL, "删除生效");
        check(sxcl_options_count(r) == 6, "重载后条数正确(6-1+1)");
        sxcl_options_free(r);

        check(read_file(path, buf, sizeof(buf)) != NULL, "回读文件");
        check(strstr(buf, "graphicsApi:vulkan") != NULL, "文件里能看到新值");
        check(strstr(buf, "\r\n") == NULL, "写出的是 LF(不带 CR)");
        check(strstr(buf, "bareline") != NULL, "bare 行原样写出");
    }
    printf("options 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
