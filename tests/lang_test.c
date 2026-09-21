/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#else
#  define _POSIX_C_SOURCE 200809L  /* setenv/unsetenv(环境变量优先那几条断言) */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/lang.h"

static int g_pass = 0, g_fail = 0;

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
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)",
               want ? want : "(null)");
    }
}

static int write_file(const char *path, const char *text)
{
    FILE *fh = fopen(path, "wb");
    if (!fh) {
        return -1;
    }
    fputs(text, fh);
    fclose(fh);
    return 0;
}

/* Python 版 .lang 的键数(两份都是 76;改动它就得同时改内置表) */
#define PY_LANG_KEY_COUNT 76

static void test_normalize(void)
{
    printf("-- 语言代码归一化\n");
    char out[SXCL_LANG_CODE_MAX];
    struct { const char *in; const char *want; } cases[] = {
        { "zh-cn", "zh-cn" }, { "zh-CN", "zh-cn" }, { "zh_CN", "zh-cn" }, { "ZH-CN", "zh-cn" },
        { "zh", "zh-cn" },    { "zh-Hans-CN", "zh-cn" }, { "en-us", "en-us" },
        { "en-US", "en-us" }, { "en_US", "en-us" }, { "en", "en-us" }, { "en-GB", "en-us" },
        { "", "zh-cn" },      { "fr", "zh-cn" },    { "ja-JP", "zh-cn" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        check(sxcl_lang_normalize_code(cases[i].in, out, sizeof(out)) == SXCL_LANG_OK,
              "归一化返回 OK");
        check_str(out, cases[i].want, cases[i].in);
    }
    check(sxcl_lang_normalize_code("en-us", out, 4) == SXCL_LANG_ERR_SPACE, "缓冲太小 = ERR_SPACE");
    check(sxcl_lang_is_supported("zh-CN") == 1, "zh-CN 认得出");
    check(sxcl_lang_is_supported("en_US") == 1, "en_US 认得出");
    check(sxcl_lang_is_supported("fr") == 0, "fr 不认");
    check(sxcl_lang_supported_count() == 2, "支持两种语言(Python _SUPPORTED)");
    check_str(sxcl_lang_supported_code(0), "zh-cn", "第 0 种是中文");
    check_str(sxcl_lang_supported_code(1), "en-us", "第 1 种是英文");
    check_str(sxcl_lang_supported_name(0), "简体中文", "中文显示名");
    check_str(sxcl_lang_supported_name(1), "English", "英文显示名");
    check(sxcl_lang_supported_code(2) == NULL, "越界返回 NULL");
}

static void test_builtin(void)
{
    printf("-- 内置默认表(不联网也有完整两份)\n");
    char err[SXCL_LANG_ERR_MAX];
    const char *nolang_dir = "build/_lang_empty"; /* 一个不存在的目录 -> 只吃内置表 */
    sxcl_lang *zh = sxcl_lang_open("zh-CN", nolang_dir, err, sizeof(err));
    check(zh != NULL, "打开中文表");
    check_str(sxcl_lang_code(zh), "zh-cn", "代码归一化成 zh-cn");
    check(sxcl_lang_count(zh) == PY_LANG_KEY_COUNT, "中文内置表 76 条(Python 那份的键数)");
    check_str(sxcl_lang_get(zh, "app.name", NULL), "Silent X Craft Launcher", "app.name 中英一致");
    check_str(sxcl_lang_get(zh, "page.versions.title", NULL), "游戏版本", "中文 page.versions.title");
    check_str(sxcl_lang_get(zh, "page.settings.title", NULL), "设置", "中文 page.settings.title");
    check_str(sxcl_lang_get(zh, "java.not_found", ""), "未检测到 Java 运行时，请前往设置页面手动选择或下载",
              "中文 java.not_found(界面文案)");

    sxcl_lang *en = sxcl_lang_open("en-US", nolang_dir, err, sizeof(err));
    check(en != NULL, "打开英文表");
    check(sxcl_lang_count(en) == PY_LANG_KEY_COUNT, "英文内置表 76 条");
    check_str(sxcl_lang_get(en, "page.versions.title", NULL), "Game Versions", "英文 page.versions.title");
    check_str(sxcl_lang_get(en, "page.settings.title", NULL), "Settings", "英文 page.settings.title");
    check_str(sxcl_lang_get(en, "app.name", NULL), "Silent X Craft Launcher", "英文 app.name");
    check(sxcl_lang_has(en, "download.retry") == 1, "has 能查到键");
    check(sxcl_lang_has(en, "没有这个键") == 0, "has 对不存在的键返回 0");
    check_str(sxcl_lang_get(en, "没有这个键", "兜底"), "兜底", "查不到用 def");

    /* 归一化认不出的代码 = 中文(Python: lang_code 不在 _SUPPORTED 就 zh-cn) */
    sxcl_lang *fallback = sxcl_lang_open("ja-JP", nolang_dir, err, sizeof(err));
    check_str(sxcl_lang_code(fallback), "zh-cn", "认不出的语言 = 中文");
    check_str(sxcl_lang_get(fallback, "page.settings.title", NULL), "设置", "认不出时文案是中文");
    sxcl_lang_free(fallback);

    /* 迭代接口:键/文案成对,顺序 = 插入顺序 */
    check_str(sxcl_lang_key_at(zh, 0), "app.name", "第 0 条是 app.name");
    check_str(sxcl_lang_text_at(zh, 0), "Silent X Craft Launcher", "第 0 条的文案");
    check(sxcl_lang_key_at(zh, sxcl_lang_count(zh)) == NULL, "越界返回 NULL");

    sxcl_lang_free(zh);
    sxcl_lang_free(en);
}

static void test_parse(void)
{
    printf("-- .lang 解析规则(与 lang.py:137-150 逐字段一致)\n");
    const char *path = "build/_lang_parse/en-us.lang"; /* 名字必须是 <代码>.lang(搜索规则) */
    (void)sxcl_fs_mkdirs("build/_lang_parse");

    /* 故意混进:注释、空行、CRLF、BOM、无 '=' 的行、空值、值里带 '='、键值两侧空白、重复键 */
    const char *text =
        "\xEF\xBB\xBF# 注释行\r\n"
        "\r\n"
        "app.name = Custom Launcher\r\n"
        "   spaced.key   =   value with spaces   \r\n"
        "empty.value =\r\n"
        "has.equals = a=b=c\r\n"
        "dupe = first\r\n"
        "dupe = second\r\n"
        "no_equals_line\r\n"
        "#comment=not-a-key\r\n"
        "trailing.no.newline = ok";
    check(write_file(path, text) == 0, "写夹具语言文件");

    char err[SXCL_LANG_ERR_MAX];
    sxcl_lang *lang = sxcl_lang_open("en-US", "build/_lang_parse", err, sizeof(err));
    check(lang != NULL, "从磁盘目录打开");
    check_str(sxcl_lang_get(lang, "app.name", NULL), "Custom Launcher", "磁盘文件覆盖内置值");
    check_str(sxcl_lang_get(lang, "spaced.key", NULL), "value with spaces", "整行 strip 后键值各自 strip");
    check_str(sxcl_lang_get(lang, "empty.value", NULL), "", "空值合法(key=)");
    check_str(sxcl_lang_get(lang, "has.equals", NULL), "a=b=c", "只按第一个 '=' 切分");
    check_str(sxcl_lang_get(lang, "dupe", NULL), "second", "重复键以后一个为准(Python dict)");
    check(sxcl_lang_has(lang, "no_equals_line") == 0, "没有 '=' 的行忽略");
    check(sxcl_lang_has(lang, "comment") == 0, "被注释掉的键不是键");
    check_str(sxcl_lang_get(lang, "trailing.no.newline", NULL), "ok", "文件末尾没换行也认");
    /* 磁盘文件只覆盖/追加,不改其它键 */
    check_str(sxcl_lang_get(lang, "page.versions.title", NULL), "Game Versions",
              "没被覆盖的键还是内置英文值");
    check(sxcl_lang_count(lang) == PY_LANG_KEY_COUNT + 5, "76 + 新键 5 条");
    sxcl_lang_free(lang);

    /* 加载文本接口:追加到当前表(后加载覆盖先加载) */
    sxcl_lang *lang2 = sxcl_lang_open("zh-cn", "build/_lang_empty", err, sizeof(err));
    const char *one = "extra.one = 一\n";
    const char *two = "extra.one = 二\n";
    check(sxcl_lang_load_text(lang2, one, strlen(one), err, sizeof(err)) == 1, "load_text 返回条数");
    check_str(sxcl_lang_get(lang2, "extra.one", NULL), "一", "文本加载生效");
    check(sxcl_lang_load_text(lang2, two, strlen(two), err, sizeof(err)) == 1, "重复加载返回 1");
    check_str(sxcl_lang_get(lang2, "extra.one", NULL), "二", "后加载覆盖先加载");
    check_str(sxcl_lang_get(lang2, "page.settings.title", NULL), "设置", "覆盖不影响别的键");
    sxcl_lang_free(lang2);
}

static void test_fallback(void)
{
    printf("-- 找不到的键回落到中文\n");
    (void)sxcl_fs_mkdirs("build/_lang_fb");
    /* 中文包里加一条英文包没有的键(模拟"英文翻译没跟上") */
    check(write_file("build/_lang_fb/zh-cn.lang", "only.chinese = 只有中文里有这一条\n") == 0,
          "写中文兜底夹具");

    char err[SXCL_LANG_ERR_MAX];
    sxcl_lang *en = sxcl_lang_open("en-US", "build/_lang_fb", err, sizeof(err));
    check(en != NULL, "英文表 + 中文兜底");
    check_str(sxcl_lang_get(en, "only.chinese", "X"), "只有中文里有这一条",
              "英文表里没有 -> 去中文表找到(磁盘这份)");
    check_str(sxcl_lang_get(en, "page.progress.cancel", "X"), "Cancel",
              "内置英文有这条 -> 用它自己的(不做无谓回退)");
    check_str(sxcl_lang_get(en, "根本不存在的键", "X"), "X", "两边都没有 -> def");
    check(sxcl_lang_has(en, "only.chinese") == 1, "has 也认兜底表里的键");
    sxcl_lang_free(en);
}

static void test_format(void)
{
    printf("-- {占位符} 替换\n");
    char err[SXCL_LANG_ERR_MAX];
    sxcl_lang *zh = sxcl_lang_open("zh-cn", "build/_lang_empty", err, sizeof(err));
    char out[256];

    const char *names[2] = { "total", "installed" };
    const char *values[2] = { "42", "7" };
    check(sxcl_lang_format(zh, "page.versions.count", "", names, values, 2, out, sizeof(out)) ==
              SXCL_LANG_OK,
          "format 成功");
    check_str(out, "共 42 个版本，已安装 7 个", "两个占位符都换掉");

    const char *one_name[1] = { "path" };
    const char *one_value[1] = { "D:/mc/java.exe" };
    check(sxcl_lang_format(zh, "java.detected", "", one_name, one_value, 1, out, sizeof(out)) ==
              SXCL_LANG_OK,
          "java.detected 带 {path}");
    check_str(out, "自动检测到 Java: D:/mc/java.exe", "单占位符");

    /* 未知占位符原样保留(与 Python str.format 的 KeyError 不同:这里选择不炸) */
    check(sxcl_lang_format(zh, "page.versions.count", "", NULL, NULL, 0, out, sizeof(out)) ==
              SXCL_LANG_OK,
          "没有映射表也不报错");
    check_str(out, "共 {total} 个版本，已安装 {installed} 个", "未知占位符原样保留");

    /* 缓冲不够:截断 + ERR_SPACE(保证 NUL 结尾) */
    check(sxcl_lang_format(zh, "page.versions.count", "", names, values, 2, out, 8) ==
              SXCL_LANG_ERR_SPACE,
          "缓冲不够 = ERR_SPACE");
    check(strlen(out) == 7, "截断到 7 字节且 NUL 结尾");

    /* 键不存在也没 def -> 空串(不是错误) */
    check(sxcl_lang_format(zh, "没有这个键", NULL, NULL, NULL, 0, out, sizeof(out)) == SXCL_LANG_OK,
          "查不到也不报错");
    check_str(out, "", "空串");
    sxcl_lang_free(zh);
}

static void test_default_table(void)
{
    printf("-- 进程级默认表(set_default / tr / shutdown)\n");
    char err[SXCL_LANG_ERR_MAX];
    check(sxcl_lang_tr("page.settings.title", "兜底") != NULL, "没设默认表也不崩");
    check_str(sxcl_lang_tr("page.settings.title", "兜底"), "兜底", "没设默认表时返回 def");

    check(sxcl_lang_set_default("en-US", "build/_lang_empty", err, sizeof(err)) == SXCL_LANG_OK,
          "设默认表 = 英文");
    check_str(sxcl_lang_tr("page.settings.title", "兜底"), "Settings", "tr 走默认表(英文)");
    check_str(sxcl_lang_code(sxcl_lang_default()), "en-us", "默认表代码");

    check(sxcl_lang_set_default("zh-CN", "build/_lang_empty", err, sizeof(err)) == SXCL_LANG_OK,
          "切回中文(热切换)");
    check_str(sxcl_lang_tr("page.settings.title", "兜底"), "设置", "tr 立刻变中文(不用重启)");

    check(sxcl_lang_set_default("不存在的语言", "build/_lang_empty", err, sizeof(err)) ==
              SXCL_LANG_OK,
          "认不出的语言也能设(落到中文)");
    check_str(sxcl_lang_code(sxcl_lang_default()), "zh-cn", "落到 zh-cn");

    sxcl_lang_shutdown();
    check(sxcl_lang_default() == NULL, "shutdown 之后没有默认表");
    check_str(sxcl_lang_tr("page.settings.title", "兜底"), "兜底", "shutdown 之后 tr 返回 def");
    sxcl_lang_shutdown(); /* 幂等 */
    check(1, "shutdown 幂等");
}

static void test_default_dirs(void)
{
    printf("-- 语言文件搜索目录\n");
    char dirs[4][SXCL_LANG_PATH_MAX];
#if defined(_WIN32)
    _putenv_s("SXCL_LANG_DIR", "D:/langs");
#else
    setenv("SXCL_LANG_DIR", "/tmp/langs", 1);
#endif
    const size_t n = sxcl_lang_default_dirs(dirs, 4);
    check(n >= 2, "至少两条:显式目录 + 配置目录");
#if defined(_WIN32)
    check_str(dirs[0], "D:/langs", "第一条是 SXCL_LANG_DIR");
#else
    check_str(dirs[0], "/tmp/langs", "第一条是 SXCL_LANG_DIR");
#endif
#if defined(_WIN32)
    _putenv_s("SXCL_LANG_DIR", "");
#else
    unsetenv("SXCL_LANG_DIR");
#endif
    const size_t n2 = sxcl_lang_default_dirs(dirs, 4);
    check(n2 >= 1, "没有显式目录时也有一条(配置目录/lang)");
    check(strstr(dirs[0], "lang") != NULL, "拼的是 <配置目录>/lang");
}

/* 真文件对拍:把 Python 版那份 config/lang/*.lang 读进来,与内置表逐键逐字比 */
static void test_python_fixture(void)
{
#if defined(SXCL_LANG_FIXTURE_DIR)
    printf("-- 与 Python 版 .lang 真文件逐字段对拍(%s)\n", SXCL_LANG_FIXTURE_DIR);
    char err[SXCL_LANG_ERR_MAX];
    sxcl_lang *builtin_zh = sxcl_lang_open("zh-cn", "build/_lang_empty", err, sizeof(err));
    sxcl_lang *builtin_en = sxcl_lang_open("en-us", "build/_lang_empty", err, sizeof(err));
    const char *dir = SXCL_LANG_FIXTURE_DIR;

    /* 直接 load_file(不经过 open 的搜索路径):证明"能读它那份 .lang" */
    sxcl_lang *from_file_zh = sxcl_lang_open("zh-cn", "build/_lang_empty", err, sizeof(err));
    sxcl_lang *from_file_en = sxcl_lang_open("en-us", "build/_lang_empty", err, sizeof(err));
    char path[1024];
    snprintf(path, sizeof(path), "%s/zh-cn.lang", dir);
    check(sxcl_lang_load_file(from_file_zh, path, err, sizeof(err)) == PY_LANG_KEY_COUNT,
          "zh-cn.lang 解析出 76 条");
    snprintf(path, sizeof(path), "%s/en-us.lang", dir);
    check(sxcl_lang_load_file(from_file_en, path, err, sizeof(err)) == PY_LANG_KEY_COUNT,
          "en-us.lang 解析出 76 条");

    /* 逐键逐字对拍:文件里的每一条,内置表必须完全一样(键集合与文案都一致) */
    for (size_t i = 0; i < sxcl_lang_count(from_file_zh); ++i) {
        const char *key = sxcl_lang_key_at(from_file_zh, i);
        check_str(sxcl_lang_get(builtin_zh, key, "(内置表缺这个键)"),
                  sxcl_lang_text_at(from_file_zh, i), key);
    }
    for (size_t i = 0; i < sxcl_lang_count(from_file_en); ++i) {
        const char *key = sxcl_lang_key_at(from_file_en, i);
        check_str(sxcl_lang_get(builtin_en, key, "(内置表缺这个键)"),
                  sxcl_lang_text_at(from_file_en, i), key);
    }
    check(sxcl_lang_count(builtin_zh) == sxcl_lang_count(from_file_zh),
          "内置中文表条数 == 文件条数");
    check(sxcl_lang_count(builtin_en) == sxcl_lang_count(from_file_en),
          "内置英文表条数 == 文件条数");

    /* open(dir) 也能直接吃它那份文件(界面层的用法) */
    sxcl_lang *via_open = sxcl_lang_open("en-us", dir, err, sizeof(err));
    check_str(sxcl_lang_get(via_open, "page.settings.title", NULL), "Settings",
              "open(dir) 直接读 Python 那份语言包");
    sxcl_lang_free(via_open);
    sxcl_lang_free(from_file_zh);
    sxcl_lang_free(from_file_en);
    sxcl_lang_free(builtin_zh);
    sxcl_lang_free(builtin_en);
#else
    printf("-- (没有 Python 版 config/lang 目录,跳过真文件对拍)\n");
#endif
}

int main(void)
{
    test_normalize();
    test_builtin();
    test_parse();
    test_fallback();
    test_format();
    test_default_table();
    test_default_dirs();
    test_python_fixture();

    printf("lang 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
