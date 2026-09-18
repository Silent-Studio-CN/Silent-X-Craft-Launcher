/* 版本 JSON 落盘回归测试 —— **不联网**。
 *
 * 背景(实测踩出来的产品级缺陷):
 *   sxcl-dl version 把版本 JSON 只留在缓存目录(sxcl-cache/<id>.json),**不写**
 *   <游戏目录>/versions/<id>/<id>.json —— 而启动层读的正是后者。
 *   症状:用户"下载完成了"却启动不了,报"找不到版本 JSON"(D:\mc-test 上真实复现过)。
 *
 * 这个用例守住四件事:
 *   1) 写到 **<游戏目录>/versions/<id>/<id>.json**(路径就是启动层要的那一个);
 *   2) 内容能被启动层解析 —— 不只是"JSON 合法",而是真让 sxcl_launch_run 以 dry_run 读它一次;
 *   3) 重复落盘**不会损坏已有文件**(原子改名),也不留 .tmp 垃圾;
 *   4) 版本名带 "../" 之类会被拒(不让它写到 versions/ 外面去)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/install.h"
#include "sxcl/json.h"
#include "sxcl/launch.h"

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
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n       期望 <%s> 实际 <%s>\n", what, want ? want : "(null)",
               got ? got : "(null)");
    }
}

/* 一份**最小但完整**的版本 JSON:启动层要的字段都在(id/type/mainClass/libraries/
 * assetIndex/assets/downloads.client),这样 dry_run 能一路走到拼 argv。 */
static const char *k_version_json =
    "{\n"
    "  \"id\": \"1.21.4-test\",\n"
    "  \"type\": \"release\",\n"
    "  \"mainClass\": \"net.minecraft.client.main.Main\",\n"
    "  \"assets\": \"19\",\n"
    "  \"assetIndex\": { \"id\": \"19\", \"url\": \"https://example.invalid/19.json\",\n"
    "                   \"sha1\": \"0000000000000000000000000000000000000000\", \"size\": 123 },\n"
    "  \"downloads\": { \"client\": { \"url\": \"https://example.invalid/client.jar\",\n"
    "                               \"sha1\": \"0000000000000000000000000000000000000000\",\n"
    "                               \"size\": 11 } },\n"
    "  \"libraries\": [],\n"
    "  \"arguments\": { \"game\": [\"--username\", \"${auth_player_name}\", \"--uuid\", \"${auth_uuid}\",\n"
    "                            \"--accessToken\", \"${auth_access_token}\", \"--userType\", \"${user_type}\",\n"
    "                            \"--version\", \"${version_name}\", \"--gameDir\", \"${game_directory}\"] }\n"
    "}\n";

static int write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return -1;
    }
    const size_t n = strlen(text);
    const int ok = (fwrite(text, 1, n, fp) == n) ? 0 : -1;
    fclose(fp);
    return ok;
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = 0;
    if (out_len != NULL) {
        *out_len = got;
    }
    return buf;
}

int main(void)
{
    /* 工作目录:构建目录下(ctest 的 WORKING_DIRECTORY),不留垃圾到仓库里 */
    const char *base = getenv("SXCL_VERSION_JSON_TEST_DIR");
    char root[SXCL_INSTALL_PATH_MAX];
    if (base != NULL && base[0] != 0) {
        (void)snprintf(root, sizeof(root), "%s", base);
    } else {
        (void)snprintf(root, sizeof(root), ".");
    }
    char game[SXCL_INSTALL_PATH_MAX];
    (void)snprintf(game, sizeof(game), "%s/_version_json_tmp/game", root);
    char src[SXCL_INSTALL_PATH_MAX];
    (void)snprintf(src, sizeof(src), "%s/src_version.json", root);
    if (sxcl_fs_mkdirs(game) != 0) {
        printf("  [!!] 建不了临时目录: %s\n", game);
        return 1;
    }
    if (write_file(src, k_version_json) != 0) {
        printf("  [!!] 写不了源 JSON: %s\n", src);
        return 1;
    }

    const char *id = "1.21.4-test";
    char expect[SXCL_INSTALL_PATH_MAX];
    (void)snprintf(expect, sizeof(expect), "%s/versions/%s/%s.json", game, id, id);
    (void)sxcl_fs_remove(expect);

    char err[256];
    err[0] = 0;
    /* 1) 首次落盘 */
    check(sxcl_install_write_version_json(game, id, src, err, sizeof(err)) == 0,
          "版本 JSON 落盘成功");
    if (err[0] != 0) {
        printf("       err: %s\n", err);
    }
    check(sxcl_fs_exists(expect) == 1,
          "文件就在 <游戏目录>/versions/<id>/<id>.json(启动层读的那一个)");

    /* 2) 内容逐字节一致 */
    size_t got_len = 0;
    char *got = read_file(expect, &got_len);
    check(got != NULL && got_len == strlen(k_version_json), "落盘内容长度与源一致");
    check(got != NULL && strcmp(got, k_version_json) == 0, "落盘内容逐字节一致(不加工、不改写)");

    /* 3) 能被 JSON 解析,且 id 对得上 */
    char jerr[128];
    jerr[0] = 0;
    sxcl_json *doc = sxcl_json_parse_file(expect, jerr, sizeof(jerr));
    check(doc != NULL, "启动层能解析这个版本 JSON");
    if (doc != NULL) {
        check_str(sxcl_json_get_string(sxcl_json_root(doc), "id", ""), id, "解析出来的 id 正确");
        sxcl_json_free(doc);
    }
    free(got);

    /* 4) **真让启动层读一次**(dry_run:只准备不起进程)——
     *    这才是"文件都下完了却启动不了"那个缺陷的直接回归断言。 */
    {
        char jar[SXCL_INSTALL_PATH_MAX];
        (void)snprintf(jar, sizeof(jar), "%s/versions/%s/%s.jar", game, id, id);
        (void)write_file(jar, "dummy-jar"); /* dry_run 也会检查 jar 在不在 */
        sxcl_launch_request req;
        memset(&req, 0, sizeof(req));
        req.game_dir = game;
        req.version_name = id;
        req.dry_run = 1;
        sxcl_launch_result res;
        char lerr[256];
        lerr[0] = 0;
        const int rc = sxcl_launch_run(&req, &res, lerr, sizeof(lerr));
        check(rc == 0, "启动层 dry_run 能读到这个版本(不再报找不到版本 JSON)");
        if (rc != 0) {
            printf("       launch err: %s\n", lerr);
        }
    }

    /* 5) 重复落盘:不许损坏已有文件,也不许留 .tmp */
    check(sxcl_install_write_version_json(game, id, src, err, sizeof(err)) == 0, "第二次落盘也成功");
    check(sxcl_fs_exists(expect) == 1, "第二次之后文件仍在");
    got = read_file(expect, &got_len);
    check(got != NULL && strcmp(got, k_version_json) == 0, "第二次之后内容仍然正确(原子改名)");
    free(got);
    {
        char tmp[SXCL_INSTALL_PATH_MAX + 8];
        (void)snprintf(tmp, sizeof(tmp), "%s.tmp", expect);
        check(sxcl_fs_exists(tmp) == 0, "没有留下 .tmp 垃圾文件");
    }

    /* 6) 版本名不安全 -> 拒绝,而且不能写到 versions/ 外面 */
    err[0] = 0;
    check(sxcl_install_write_version_json(game, "../escape", src, err, sizeof(err)) != 0,
          "版本名含 .. 被拒(不能逃出 versions/)");
    check(sxcl_install_write_version_json(game, "a/b", src, err, sizeof(err)) != 0,
          "版本名含 / 被拒");
    check(sxcl_install_write_version_json(game, "a b", src, err, sizeof(err)) != 0,
          "版本名含空格被拒");
    check(sxcl_fs_exists(expect) == 1, "被拒的调用没有破坏已有文件");

    /* 7) 源文件不存在 -> 报错(而不是写出一个空文件) */
    err[0] = 0;
    check(sxcl_install_write_version_json(game, "no-src", "不存在的路径.json", err, sizeof(err)) != 0,
          "源文件不存在时报错");
    check(sxcl_fs_is_dir(game) == 1, "报错也不影响已有目录(fs_exists 是看文件,目录要用 fs_is_dir)");

    /* 清理(测试自己的临时件) */
    (void)sxcl_fs_remove(src);
    char tdir[SXCL_INSTALL_PATH_MAX];
    (void)snprintf(tdir, sizeof(tdir), "%s/_version_json_tmp", root);
    (void)sxcl_fs_remove_tree(tdir);

    printf("version_json_test: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
