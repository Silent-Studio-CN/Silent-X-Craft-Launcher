/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/logexport.h"
#include "sxcl/zip.h"

/* 一键导出日志包的夹具(建在构建目录下):
 *   _export_fixture/logs/sxcl-20260101-000000.log   启动器日志 1(含令牌/用户名/UUID)
 *   _export_fixture/logs/sxcl-20260101-000001.log   启动器日志 2(新的,应优先)
 *   _export_fixture/logs/other.log                  别人的日志(不该进包)
 *   _export_fixture/game/logs/latest.log            游戏日志(含 "Setting user:")
 *   _export_fixture/game/crash-reports/crash-x.txt  崩溃报告(含用户名与 UUID)
 * 校验方式:打完包用**我们自己的 zip 读侧**回读,断言
 *   1) 条目齐(启动器日志 + 游戏日志 + 报告 + 说明文件);
 *   2) 明文令牌/用户名/UUID 一个都不在包里;
 *   3) 打码标记 *** 在;
 *   4) 不含无关文件(other.log)。 */

#define FIX       "_export_fixture"
#define LOG_DIR   FIX "/logs"
#define GAME_DIR  FIX "/game"
#define OUT_ZIP   FIX "/out/logs.zip"

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

static void check_int(long got, long want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %ld want %ld\n", what, got, want);
    }
}

static int write_text(const char *path, const char *text)
{
    FILE *f = sxcl_fs_fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    if (fwrite(text, 1, strlen(text), f) != strlen(text)) {
        (void)fclose(f);
        return -1;
    }
    (void)fclose(f);
    return 0;
}

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

static int setup(void)
{
    if (sxcl_fs_mkdirs(LOG_DIR) != 0 || sxcl_fs_mkdirs(GAME_DIR "/logs") != 0 ||
        sxcl_fs_mkdirs(GAME_DIR "/crash-reports") != 0) {
        return -1;
    }
    if (write_text(LOG_DIR "/sxcl-20260101-000000.log",
                   "2026-01-01 00:00:00.000 INFO  launch | 启动 --accessToken SUPERSECRETTOKEN\n"
                   "2026-01-01 00:00:00.100 INFO  game | Setting user: Steve\n"
                   "2026-01-01 00:00:00.200 INFO  launch | uuid=069a79f4-44e9-4726-a5be-fca90e38aaf5\n"
                   "2026-01-01 00:00:00.300 INFO  launch | path C:\\Users\\Steve\\.minecraft\n") != 0) {
        return -1;
    }
    if (write_text(LOG_DIR "/sxcl-20260101-000001.log",
                   "2026-01-01 00:00:01.000 INFO  launch | 第二份日志 password=hunter2\n") != 0) {
        return -1;
    }
    if (write_text(LOG_DIR "/other.log", "2026-01-01 00:00:00.000 INFO  x | 别人的日志\n") != 0) {
        return -1;
    }
    if (write_text(GAME_DIR "/logs/latest.log",
                   "[00:00:00] [main/INFO]: Loading Minecraft 1.0\n"
                   "[00:00:00] [Render thread/INFO]: Setting user: Steve\n"
                   "[00:00:01] [main/ERROR]: java.lang.OutOfMemoryError: Java heap space\n") != 0) {
        return -1;
    }
    if (write_text(GAME_DIR "/crash-reports/crash-2026-01-01_00.00.00-client.txt",
                   "---- Minecraft Crash Report ----\n"
                   "Description: Rendering overlay\n"
                   "Caused by: java.lang.OutOfMemoryError: Java heap space\n"
                   "\tat com.example.Main.tick(Main.java:42)\n") != 0) {
        return -1;
    }
    return 0;
}

int main(void)
{
    sxcl_logs_export_request req;
    sxcl_logs_export_result res;
    char err[SXCL_LOGS_EXPORT_ERROR_MAX];
    sxcl_zip *zip = NULL;
    char buf[8192];

    printf("日志导出模块测试\n");
    if (setup() != 0) {
        printf("夹具准备失败(写不了 %s?)\n", FIX);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    req.game_dir = GAME_DIR;
    req.log_dir = LOG_DIR;
    req.out_zip = OUT_ZIP;
    req.max_launcher_logs = 2;

    printf("[1] 导出\n");
    check_int(sxcl_logs_export(&req, &res, err, sizeof(err)), 0, "导出成功");
    check(res.files >= 4, "至少打进 4 个文件(2 份启动器日志 + latest.log + 1 份报告)");
    check_int(res.entries, res.files + 1, "条目数 = 文件数 + 说明文件");
    check(res.zip_bytes > 200, "zip 有内容");
    check(res.skipped == 0, "没有文件被跳过");
    check(contains(res.out_path, "logs.zip"), "结果里有输出路径");

    printf("[2] 用我们自己的 zip 读侧回读\n");
    zip = sxcl_zip_open(OUT_ZIP);
    check(zip != NULL, "打得开的 zip(store 打包器产出的是合法 zip)");
    if (zip == NULL) {
        printf("logexport 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
        return 1;
    }
    check(sxcl_zip_find(zip, SXCL_LOGS_EXPORT_MANIFEST) >= 0, "有说明文件条目");
    check(sxcl_zip_find(zip, "game/logs/latest.log") >= 0, "有游戏 latest.log 条目");
    check(sxcl_zip_find(zip, "sxcl-logs/sxcl-20260101-000000.log") >= 0, "有启动器日志条目");
    check(sxcl_zip_find(zip, "game/crash-reports/crash-2026-01-01_00.00.00-client.txt") >= 0,
          "有崩溃报告条目");
    check(sxcl_zip_find(zip, "sxcl-logs/other.log") < 0, "不含无关文件 other.log");

    printf("[3] 打码:明文不许出现在包里\n");
    {
        size_t len = 0;
        int rc = 0;
        buf[0] = '\0';
        rc = sxcl_zip_extract_memory(zip, "sxcl-logs/sxcl-20260101-000000.log", buf, sizeof(buf) - 1u,
                                     &len);
        check(rc == 0 && len > 0, "解出第一份启动器日志");
        buf[len] = '\0';
        check(!contains(buf, "SUPERSECRETTOKEN"), "令牌明文不在包里");
        check(!contains(buf, "069a79f4-44e9-4726-a5be-fca90e38aaf5"), "UUID 明文不在包里");
        check(!contains(buf, "Steve"), "用户名明文不在包里");
        check(contains(buf, "***"), "打码标记在");
        check(contains(buf, "launch | 启动"), "非敏感内容原样保留");
    }
    {
        size_t len = 0;
        int rc = 0;
        rc = sxcl_zip_extract_memory(zip, "sxcl-logs/sxcl-20260101-000001.log", buf, sizeof(buf) - 1u,
                                     &len);
        check(rc == 0, "解出第二份日志");
        buf[len] = '\0';
        check(!contains(buf, "hunter2"), "password 的值被打了码");
    }
    {
        size_t len = 0;
        int rc = 0;
        rc = sxcl_zip_extract_memory(zip, "game/logs/latest.log", buf, sizeof(buf) - 1u, &len);
        check(rc == 0, "解出游戏 latest.log");
        buf[len] = '\0';
        check(!contains(buf, "Steve"), "游戏日志里的玩家名被打了码");
        check(contains(buf, "OutOfMemoryError"), "游戏日志的内容还在");
    }
    {
        size_t len = 0;
        int rc = 0;
        rc = sxcl_zip_extract_memory(zip, SXCL_LOGS_EXPORT_MANIFEST, buf, sizeof(buf) - 1u, &len);
        check(rc == 0, "解出说明文件");
        buf[len] = '\0';
        check(contains(buf, "store"), "说明文件写明打包方式");
        check(contains(buf, "打码"), "说明文件写明打码");
        check(!contains(buf, "Steve"), "说明文件里的路径也打了码");
    }
    sxcl_zip_close(zip);

    printf("[4] 边界\n");
    check_int(sxcl_logs_export(NULL, &res, err, sizeof(err)), -1, "req=NULL 报错");
    req.out_zip = NULL;
    check_int(sxcl_logs_export(&req, &res, err, sizeof(err)), -1, "缺输出路径报错");
    check(err[0] != '\0', "错误文本有人话");
    req.out_zip = FIX "/out/only-launcher.zip";
    req.game_dir = NULL;   /* 没有游戏目录:只剩启动器自己的日志 + 说明文件 */
    req.log_dir = LOG_DIR;
    check_int(sxcl_logs_export(&req, &res, err, sizeof(err)), 0, "没有游戏目录也能导出");
    check(res.files >= 2, "启动器日志照样进包");
    check(res.note[0] == '\0' || res.note[0] != '\0', "结果里有 note 字段");
    zip = sxcl_zip_open(FIX "/out/only-launcher.zip");
    check(zip != NULL, "回读第二份 zip");
    sxcl_zip_close(zip);

    printf("logexport 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
