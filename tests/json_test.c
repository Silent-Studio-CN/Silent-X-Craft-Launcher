/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/json.h"

static int g_checks = 0;
static int g_failures = 0;

/* 注意参数顺序:说明在前、条件在后(与其它 check_* 一致)。 */
static void check_true(const char *what, int cond)
{
    ++g_checks;
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s\n", what);
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    ++g_checks;
    const char *g = got ? got : "(NULL)";
    if (got && strcmp(got, want) == 0) {
        printf("ok   %s = \"%s\"\n", what, g);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s\n     got  = \"%s\"\n     want = \"%s\"\n", what, g, want);
    }
}

static void check_size(const char *what, size_t got, size_t want)
{
    ++g_checks;
    if (got == want) {
        printf("ok   %s = %zu\n", what, got);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s: got %zu want %zu\n", what, got, want);
    }
}

static void check_i64(const char *what, int64_t got, int64_t want)
{
    ++g_checks;
    if (got == want) {
        printf("ok   %s = %lld\n", what, (long long)got);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s: got %lld want %lld\n", what, (long long)got, (long long)want);
    }
}

static void check_double(const char *what, double got, double want)
{
    ++g_checks;
    if (got == want) {
        printf("ok   %s = %g\n", what, got);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s: got %.17g want %.17g\n", what, got, want);
    }
}

static void check_close(const char *what, double got, double want, double rel)
{
    const double diff = (got > want) ? (got - want) : (want - got);
    const double scale = (want > 1.0 || want < -1.0) ? (want > 0 ? want : -want) : 1.0;
    ++g_checks;
    if (diff / scale <= rel) {
        printf("ok   %s ~= %g\n", what, got);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s: got %.17g want %.17g (相对误差 %.3g > %.3g)\n", what, got, want,
                diff / scale, rel);
    }
}

static void check_ptr_null(const char *what, const void *p)
{
    ++g_checks;
    if (p == NULL) {
        printf("ok   %s 为 NULL\n", what);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s: 期望 NULL,实际非空\n", what);
    }
}

static void check_ptr_nonnull(const char *what, const void *p)
{
    ++g_checks;
    if (p != NULL) {
        printf("ok   %s 非 NULL\n", what);
    } else {
        ++g_failures;
        fprintf(stderr, "FAIL %s: 期望非 NULL,实际是 NULL\n", what);
    }
}

/* 解析一个应当成功的输入:返回根节点,失败则记 FAIL。 */
static const sxcl_json_value *parse_ok(sxcl_json **doc_out, const char *text, const char *what)
{
    char err[256];
    err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(text, strlen(text), err, sizeof err);
    ++g_checks;
    if (!doc) {
        ++g_failures;
        fprintf(stderr, "FAIL %s: 应当解析成功却失败 (err=%s) 输入=%s\n", what, err, text);
        *doc_out = NULL;
        return NULL;
    }
    if (err[0] != '\0') {
        ++g_failures;
        fprintf(stderr, "FAIL %s: 解析成功但 err 非空: %s\n", what, err);
    } else {
        printf("ok   %s\n", what);
    }
    *doc_out = doc;
    return sxcl_json_root(doc);
}

/* 解析一个应当失败的输入:必须返回 NULL 且 err 非空。 */
static void expect_invalid(const char *text, const char *what)
{
    char err[256];
    err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(text, strlen(text), err, sizeof err);
    ++g_checks;
    if (doc != NULL) {
        ++g_failures;
        fprintf(stderr, "FAIL %s: 非法输入竟然解析成功\n", what);
        sxcl_json_free(doc);
        return;
    }
    if (err[0] == '\0') {
        ++g_failures;
        fprintf(stderr, "FAIL %s: 解析失败但 err 为空\n", what);
        return;
    }
    printf("ok   %s -> %s\n", what, err);
}

/* ── 1) 基本类型与嵌套 ── */
static void test_basic(void)
{
    static const char kJson[] = "{\"a\":1,\"b\":[true,false,null,\"x\"],\"c\":{\"d\":-2.5e3}}";
    sxcl_json *doc = NULL;
    const sxcl_json_value *root = parse_ok(&doc, kJson, "基本:顶层对象解析");
    if (!root) {
        return;
    }
    check_true("基本:根类型是对象", sxcl_json_type_of(root) == SXCL_JSON_OBJECT);
    check_double("基本:a 的数字值", sxcl_json_number(sxcl_json_get(root, "a")), 1.0);
    check_i64("基本:get_int64(a)", sxcl_json_get_int64(root, "a", -1), 1);

    const sxcl_json_value *b = sxcl_json_get(root, "b");
    check_true("基本:b 是数组", sxcl_json_type_of(b) == SXCL_JSON_ARRAY);
    check_size("基本:b 的元素个数", sxcl_json_size(b), 4);
    check_true("基本:b[0] 是布尔", sxcl_json_type_of(sxcl_json_at(b, 0)) == SXCL_JSON_BOOL);
    check_true("基本:b[1] 是布尔", sxcl_json_type_of(sxcl_json_at(b, 1)) == SXCL_JSON_BOOL);
    check_true("基本:b[2] 是 null", sxcl_json_type_of(sxcl_json_at(b, 2)) == SXCL_JSON_NULL);
    check_str("基本:b[3] 字符串", sxcl_json_string(sxcl_json_at(b, 3)), "x");

    const sxcl_json_value *c = sxcl_json_get(root, "c");
    check_true("基本:c 是对象", sxcl_json_type_of(c) == SXCL_JSON_OBJECT);
    check_double("基本:c.d", sxcl_json_number(sxcl_json_get(c, "d")), -2500.0);
    check_i64("基本:get_int64(c.d)", sxcl_json_get_int64(c, "d", 0), -2500);

    check_ptr_null("基本:get 缺失键", sxcl_json_get(root, "zzz"));
    check_str("基本:get_string 缺失键返回默认值", sxcl_json_get_string(root, "zzz", "默认"), "默认");
    check_i64("基本:get_int64 缺失键返回默认值", sxcl_json_get_int64(root, "zzz", -7), -7);
    check_i64("基本:get_bool 缺失键返回默认值", sxcl_json_get_bool(root, "zzz", 1), 1);
    sxcl_json_free(doc);
}

/* ── 2) 转义与 UTF-8 ── */
static void test_strings(void)
{
    /* JSON: "\"\\\/\b\f\n\r\t\u0041\u4e2d\uD83D\uDE00" */
    static const char kEscapesJson[] =
        "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0041\\u4e2d\\uD83D\\uDE00\"";
    /* 解码后:" \ / \b \f \n \r \t A 中(3 字节) 😀(4 字节) = 16 字节 */
    static const char kEscapesWant[] = "\"\\/\b\f\n\r\tA\xE4\xB8\xAD\xF0\x9F\x98\x80";

    sxcl_json *doc = NULL;
    const sxcl_json_value *root = parse_ok(&doc, kEscapesJson, "转义:全部转义序列解析");
    if (root) {
        const char *s = sxcl_json_string(root);
        check_str("转义:解码结果", s, kEscapesWant);
        check_size("转义:解码后字节数", s ? strlen(s) : 0u, 16u);
        if (s) {
            /* 代理对合成的码点必须是 F0 9F 98 80(😀),不是两个代理项的 CESU-8 */
            check_true("转义:代理对 -> 四字节 UTF-8",
                       (unsigned char)s[12] == 0xF0u && (unsigned char)s[13] == 0x9Fu &&
                           (unsigned char)s[14] == 0x98u && (unsigned char)s[15] == 0x80u);
        }
        sxcl_json_free(doc);
    }

    /* 中文(含键)原样透传 */
    static const char kCn[] = "{\"名称\":\"中文测试\",\"emoji\":\"\xF0\x9F\x98\x80\"}";
    root = parse_ok(&doc, kCn, "UTF-8:中文对象解析");
    if (root) {
        check_str("UTF-8:中文值透传", sxcl_json_get_string(root, "名称", NULL), "中文测试");
        check_str("UTF-8:emoji 透传", sxcl_json_get_string(root, "emoji", NULL), "\xF0\x9F\x98\x80");
        sxcl_json_free(doc);
    }

    /* \u0000:按 JSON 规范解码成 0 字节(接口是 NUL 结尾字符串,内容到此为止) */
    root = parse_ok(&doc, "\"a\\u0000b\"", "转义:\\u0000 解析");
    if (root) {
        /* 接口是 NUL 结尾字符串:内嵌 \u0000 会截断可见内容(已知且有意的取舍) */
        check_str("转义:\\u0000 在首个 NUL 处截断", sxcl_json_string(root), "a");
        sxcl_json_free(doc);
    }

    /* 空字符串 */
    root = parse_ok(&doc, "\"\"", "字符串:空串");
    if (root) {
        check_str("字符串:空串内容", sxcl_json_string(root), "");
        sxcl_json_free(doc);
    }

    /* 字符串里的 UTF-8 多字节字符不会被当控制字符 */
    root = parse_ok(&doc, "\"\\u00e9\\u4e2d\"", "转义:非代理 BMP 码点");
    if (root) {
        check_str("转义:\\u00e9\\u4e2d", sxcl_json_string(root), "\xC3\xA9\xE4\xB8\xAD");
        sxcl_json_free(doc);
    }
}

/* ── 3) 类型不符与越界 ── */
static void test_mismatch(void)
{
    static const char kJson[] = "{\"n\":42,\"s\":\"txt\",\"b\":true,\"a\":[1,2],\"o\":{}}";
    sxcl_json *doc = NULL;
    const sxcl_json_value *root = parse_ok(&doc, kJson, "类型:样例解析");
    if (!root) {
        return;
    }
    const sxcl_json_value *arr = sxcl_json_get(root, "a");
    const sxcl_json_value *obj = sxcl_json_get(root, "o");
    const sxcl_json_value *num = sxcl_json_get(root, "n");
    const sxcl_json_value *str = sxcl_json_get(root, "s");
    const sxcl_json_value *boo = sxcl_json_get(root, "b");

    check_str("类型:get_string(数字) 返回默认值", sxcl_json_get_string(root, "n", "D"), "D");
    check_str("类型:get_string(布尔) 返回默认值", sxcl_json_get_string(root, "b", "D"), "D");
    check_i64("类型:get_int64(字符串) 返回默认值", sxcl_json_get_int64(root, "s", -5), -5);
    check_i64("类型:get_int64(数组) 返回默认值", sxcl_json_get_int64(root, "a", -5), -5);
    check_i64("类型:get_bool(数字) 返回默认值", sxcl_json_get_bool(root, "n", 9), 9);
    check_i64("类型:get_bool(字符串) 返回默认值", sxcl_json_get_bool(root, "s", 9), 9);
    check_i64("类型:get_bool(真)", sxcl_json_get_bool(root, "b", 0), 1);

    check_ptr_null("类型:string(数字) 为 NULL", sxcl_json_string(num));
    check_ptr_null("类型:string(布尔) 为 NULL", sxcl_json_string(boo));
    check_ptr_null("类型:string(对象) 为 NULL", sxcl_json_string(obj));
    check_double("类型:number(字符串) 为 0", sxcl_json_number(str), 0.0);
    check_double("类型:number(数组) 为 0", sxcl_json_number(arr), 0.0);
    check_size("类型:size(对象) 为 0", sxcl_json_size(obj), 0);
    check_size("类型:size(字符串) 为 0", sxcl_json_size(str), 0);
    check_size("类型:size(NULL) 为 0", sxcl_json_size(NULL), 0);
    check_ptr_null("越界:at(2) 为 NULL", sxcl_json_at(arr, 2));
    check_ptr_null("越界:at(SIZE_MAX) 为 NULL", sxcl_json_at(arr, (size_t)-1));
    check_ptr_null("越界:at(NULL,0) 为 NULL", sxcl_json_at(NULL, 0));
    check_ptr_null("越界:at(对象,0) 为 NULL", sxcl_json_at(obj, 0));
    check_ptr_null("越界:get(数组,\"x\") 为 NULL", sxcl_json_get(arr, "x"));
    check_ptr_null("越界:get(NULL,NULL) 为 NULL", sxcl_json_get(NULL, NULL));
    check_ptr_null("越界:get(对象,NULL) 为 NULL", sxcl_json_get(root, NULL));
    check_i64("越界:at 越界后元素非空检查", sxcl_json_at(arr, 0) != NULL ? 1 : 0, 1);

    /* 空对象 / 空数组 */
    check_size("空:对象成员数为 0", sxcl_json_size(sxcl_json_get(root, "o")), 0);
    check_ptr_null("空:空对象 get 任意键为 NULL", sxcl_json_get(obj, "anything"));

    sxcl_json_free(doc);
}

/* ── 4) 非法输入 ── */
static void test_invalid(void)
{
    expect_invalid("{\"a\":1,}", "非法:对象尾随逗号");
    expect_invalid("[1,2,]", "非法:数组尾随逗号");
    expect_invalid("[,1]", "非法:数组开头逗号");
    expect_invalid("{\"a\":1,,}", "非法:对象双逗号");
    expect_invalid("\"abc", "非法:字符串未闭合");
    expect_invalid("[1,2", "非法:数组未闭合");
    expect_invalid("{\"a\":1", "非法:对象未闭合");
    expect_invalid("{", "非法:只有左花括号");
    expect_invalid("[", "非法:只有左方括号");
    expect_invalid("\"\\q\"", "非法:未知转义 \\q");
    expect_invalid("\"\\x41\"", "非法:未知转义 \\x");
    expect_invalid("\"\\u12\"", "非法:\\u 只有两位");
    expect_invalid("\"\\u12G4\"", "非法:\\u 非十六进制");
    expect_invalid("\"\\uD800\"", "非法:孤立高位代理");
    expect_invalid("\"\\uDC00\"", "非法:孤立低位代理");
    expect_invalid("\"\\uD83D\\u0041\"", "非法:高位代理后不是低位代理");
    expect_invalid("\"\\uD83D\"", "非法:高位代理后字符串结束");
    expect_invalid("{a:1}", "非法:键没有引号");
    expect_invalid("{'a':1}", "非法:单引号字符串");
    expect_invalid("[\"a\" 'b']", "非法:单引号元素");
    expect_invalid("[1 2]", "非法:缺逗号");
    expect_invalid("{\"a\" 1}", "非法:缺冒号");
    expect_invalid("{\"a\":}", "非法:缺值");
    expect_invalid("{:1}", "非法:缺键");
    expect_invalid("{}\"x\"", "非法:根值后多余内容");
    expect_invalid("{} x", "非法:根值后多余内容(非空白)");
    expect_invalid("[1]]", "非法:多余右括号");
    expect_invalid("tru", "非法:截断的 true");
    expect_invalid("nul", "非法:截断的 null");
    expect_invalid("NaN", "非法:NaN");
    expect_invalid("Infinity", "非法:Infinity");
    expect_invalid("-Infinity", "非法:-Infinity");
    expect_invalid("01", "非法:前导零");
    expect_invalid("-01", "非法:负前导零");
    expect_invalid("1.", "非法:小数点后无数字");
    expect_invalid(".5", "非法:没有整数部分");
    expect_invalid("+1", "非法:正号");
    expect_invalid("1e", "非法:指数无数字");
    expect_invalid("1e+", "非法:指数只有符号");
    expect_invalid("-", "非法:只有一个负号");
    expect_invalid("0x10", "非法:十六进制");
    expect_invalid("{\"a\":1} // 注释", "非法:行注释");
    expect_invalid("{/*x*/\"a\":1}", "非法:块注释");
    expect_invalid("\"a\tb\"", "非法:字符串里的裸 TAB");
    expect_invalid("\"a\nb\"", "非法:字符串里的裸换行");
    expect_invalid("\"a", "非法:裸控制字符之外的未闭合");
    expect_invalid("", "非法:空输入");
    expect_invalid("   \r\n\t ", "非法:只有空白");
    expect_invalid("[1,2,3", "非法:数组未闭合(多元素)");
    expect_invalid("{\"a\":[1,2}", "非法:数组对象括号错配");
    expect_invalid("{\"a\"::1}", "非法:双冒号");
    expect_invalid("[,]", "非法:只有逗号");
}

/* ── 5) 空容器 / 纯标量 ── */
static void test_scalars(void)
{
    sxcl_json *doc = NULL;
    const sxcl_json_value *root = parse_ok(&doc, "{}", "标量:空对象");
    if (root) {
        check_true("标量:{} 是对象", sxcl_json_type_of(root) == SXCL_JSON_OBJECT);
        check_size("标量:{} 大小为 0", sxcl_json_size(root), 0u);
        check_ptr_null("标量:{} get 为 NULL", sxcl_json_get(root, "a"));
        sxcl_json_free(doc);
    }
    root = parse_ok(&doc, "[]", "标量:空数组");
    if (root) {
        check_true("标量:[] 是数组", sxcl_json_type_of(root) == SXCL_JSON_ARRAY);
        check_size("标量:[] 大小为 0", sxcl_json_size(root), 0u);
        check_ptr_null("标量:[] at(0) 为 NULL", sxcl_json_at(root, 0));
        sxcl_json_free(doc);
    }
    root = parse_ok(&doc, "  {  }  ", "标量:带空白的空对象");
    if (root) {
        check_size("标量:空白不影响", sxcl_json_size(root), 0u);
        check_true("标量:{ } 是对象", sxcl_json_type_of(root) == SXCL_JSON_OBJECT);
        sxcl_json_free(doc);
    }
    root = parse_ok(&doc, "\n[\n\t]\r\n", "标量:带换行的空数组");
    if (root) {
        check_true("标量:[\\n\\t] 是数组", sxcl_json_type_of(root) == SXCL_JSON_ARRAY);
        sxcl_json_free(doc);
    }
    root = parse_ok(&doc, "\"hello\"", "标量:纯字符串");
    if (root) {
        check_true("标量:纯字符串类型", sxcl_json_type_of(root) == SXCL_JSON_STRING);
        check_str("标量:纯字符串内容", sxcl_json_string(root), "hello");
        check_i64("标量:纯字符串上取 int 用默认值", sxcl_json_get_int64(root, "x", 3), 3);
        sxcl_json_free(doc);
    }
    root = parse_ok(&doc, "42", "标量:纯数字");
    if (root) {
        check_true("标量:纯数字类型", sxcl_json_type_of(root) == SXCL_JSON_NUMBER);
        check_double("标量:纯数字值", sxcl_json_number(root), 42.0);
        check_str("标量:纯数字上取字符串用默认值", sxcl_json_get_string(root, "x", "d"), "d");
        sxcl_json_free(doc);
    }
    root = parse_ok(&doc, "true", "标量:true");
    if (root) {
        check_true("标量:true 类型", sxcl_json_type_of(root) == SXCL_JSON_BOOL);
        sxcl_json_free(doc);
    }
    root = parse_ok(&doc, "false", "标量:false");
    if (root) {
        check_true("标量:false 类型", sxcl_json_type_of(root) == SXCL_JSON_BOOL);
        sxcl_json_free(doc);
    }
    root = parse_ok(&doc, "null", "标量:null");
    if (root) {
        check_true("标量:null 类型", sxcl_json_type_of(root) == SXCL_JSON_NULL);
        check_double("标量:null 取数字得 0", sxcl_json_number(root), 0.0);
        check_ptr_null("标量:null 取字符串得 NULL", sxcl_json_string(root));
        sxcl_json_free(doc);
    }
    /* 嵌套空容器 */
    root = parse_ok(&doc, "[{},[],\"\",0]", "标量:数组里套空容器");
    if (root) {
        check_size("标量:嵌套空容器数组大小", sxcl_json_size(root), 4u);
        check_true("标量:[0] 是对象", sxcl_json_type_of(sxcl_json_at(root, 0)) == SXCL_JSON_OBJECT);
        check_size("标量:[1] 是空数组", sxcl_json_size(sxcl_json_at(root, 1)), 0u);
        check_str("标量:[2] 是空串", sxcl_json_string(sxcl_json_at(root, 2)), "");
        check_double("标量:[3] 是 0", sxcl_json_number(sxcl_json_at(root, 3)), 0.0);
        sxcl_json_free(doc);
    }
    /* 带 BOM 的输入应当被容忍 */
    root = parse_ok(&doc, "\xEF\xBB\xBF{\"a\":1}", "标量:UTF-8 BOM");
    if (root) {
        check_i64("标量:BOM 后内容正确", sxcl_json_get_int64(root, "a", 0), 1);
        sxcl_json_free(doc);
    }
}

/* ── 6) 数字 ── */
static void test_numbers(void)
{
    struct {
        const char *text;
        double want;
    } kCases[] = {
        {"0", 0.0},
        {"-0", -0.0},
        {"1", 1.0},
        {"-1", -1.0},
        {"1.5", 1.5},
        {"-2.5e3", -2500.0},
        {"1e3", 1000.0},
        {"1E3", 1000.0},
        {"1e+3", 1000.0},
        {"1e-3", 0.001},
        {"0.5", 0.5},
        {"-0.5", -0.5},
        {"3.141592653589793", 3.141592653589793},
        {"123456789012345678901234567890", 1.2345678901234568e29},
        {"0.000001", 1e-6},
        {"1e308", 1e308},
    };
    for (size_t i = 0; i < sizeof kCases / sizeof kCases[0]; ++i) {
        sxcl_json *doc = NULL;
        char what[96];
        snprintf(what, sizeof what, "数字:%s", kCases[i].text);
        const sxcl_json_value *root = parse_ok(&doc, kCases[i].text, what);
        if (root) {
            if (kCases[i].want == 0.0) {
                check_double(what, sxcl_json_number(root), kCases[i].want);
            } else {
                check_close(what, sxcl_json_number(root), kCases[i].want, 1e-15);
            }
            sxcl_json_free(doc);
        }
    }

    /* 超长数字不 UB、不崩溃:1e400 上溢为 inf,1e-400 下溢为 0 */
    sxcl_json *doc = NULL;
    const sxcl_json_value *root =
        parse_ok(&doc, "{\"big\":1e400,\"small\":1e-400,\"huge\":99999999999999999999999999999999}",
                 "数字:上溢/下溢/超长");
    if (root) {
        check_true("数字:1e400 上溢为 inf", sxcl_json_number(sxcl_json_get(root, "big")) > 1e308);
        check_double("数字:1e-400 下溢为 0", sxcl_json_number(sxcl_json_get(root, "small")), 0.0);
        check_i64("数字:溢出时 get_int64 钳到 INT64_MAX", sxcl_json_get_int64(root, "big", 0),
                  INT64_MAX);
        /* 32 个 9 = 1e32-1,双精度保留 17 位有效数字即可 */
        check_close("数字:32 位整数近似值", sxcl_json_number(sxcl_json_get(root, "huge")), 1e32,
                    1e-14);
        sxcl_json_free(doc);
    }

    /* int64 转换:截断、负数、钳位 */
    root = parse_ok(&doc, "{\"t\":2.9,\"n\":-2.9,\"z\":0,\"hi\":9223372036854775807,\"lo\":-1e30}",
                    "数字:int64 截断与钳位");
    if (root) {
        check_i64("数字:2.9 截断为 2", sxcl_json_get_int64(root, "t", 0), 2);
        check_i64("数字:-2.9 截断为 -2", sxcl_json_get_int64(root, "n", 0), -2);
        check_i64("数字:0", sxcl_json_get_int64(root, "z", 9), 0);
        check_i64("数字:2^63-1 钳到 INT64_MAX", sxcl_json_get_int64(root, "hi", 0), INT64_MAX);
        check_i64("数字:-1e30 钳到 INT64_MIN", sxcl_json_get_int64(root, "lo", 0), INT64_MIN);
        sxcl_json_free(doc);
    }

    /* 解析器不会产生 NaN */
    root = parse_ok(&doc, "[0.0]", "数字:零");
    if (root) {
        check_true("数字:0.0 不是 NaN", sxcl_json_number(sxcl_json_at(root, 0)) == 0.0);
        sxcl_json_free(doc);
    }
}

/* ── 7) 重复键 ── */
static void test_duplicate_keys(void)
{
    sxcl_json *doc = NULL;
    const sxcl_json_value *root = parse_ok(&doc, "{\"a\":1,\"b\":2,\"a\":3,\"a\":4}",
                                           "重复键:最后者胜");
    if (root) {
        check_i64("重复键:a 取最后一个值", sxcl_json_get_int64(root, "a", -1), 4);
        check_i64("重复键:b 正常", sxcl_json_get_int64(root, "b", -1), 2);
        check_str("重复键:类型也以最后为准", sxcl_json_get_string(root, "b", "d"), "d");
        sxcl_json_free(doc);
    }
    root = parse_ok(&doc, "{\"o\":{\"x\":\"first\",\"x\":\"last\"}}", "重复键:嵌套对象");
    if (root) {
        check_str("重复键:嵌套对象取最后", sxcl_json_get_string(sxcl_json_get(root, "o"), "x", NULL),
                  "last");
        sxcl_json_free(doc);
    }
    /* 空键 */
    root = parse_ok(&doc, "{\"\":1,\"k\":2}", "重复键:空字符串键");
    if (root) {
        check_i64("空键:取值", sxcl_json_get_int64(root, "", -1), 1);
        sxcl_json_free(doc);
    }
}

/* ── 8) 深度与规模 ── */
static char *make_nested(int depth)
{
    char *buf = (char *)malloc((size_t)depth * 2u + 1u);
    if (!buf) {
        return NULL;
    }
    for (int i = 0; i < depth; ++i) {
        buf[i] = '[';
        buf[(size_t)depth * 2u - 1u - (size_t)i] = ']';
    }
    buf[(size_t)depth * 2u] = '\0';
    return buf;
}

static void test_depth_and_scale(void)
{
    char *ok64 = make_nested(64);
    char *bad65 = make_nested(65);
    if (ok64 && bad65) {
        sxcl_json *doc = NULL;
        const sxcl_json_value *root = parse_ok(&doc, ok64, "深度:64 层通过");
        if (root) {
            const sxcl_json_value *cur = root;
            size_t levels = 0;
            while (cur && sxcl_json_type_of(cur) == SXCL_JSON_ARRAY) {
                ++levels;
                cur = sxcl_json_at(cur, 0);
            }
            check_size("深度:实际层数", levels, 64u);
            sxcl_json_free(doc);
        }
        expect_invalid(bad65, "深度:65 层超限");
        /* 对象版本的深度也走同一条路径 */
        char *obj65 = (char *)malloc(65u * 12u + 8u);
        if (obj65) {
            size_t n = 0;
            for (int i = 0; i < 65; ++i) {
                memcpy(obj65 + n, "{\"a\":", 5);
                n += 5;
            }
            obj65[n++] = '1';
            for (int i = 0; i < 65; ++i) {
                obj65[n++] = '}';
            }
            obj65[n] = '\0';
            expect_invalid(obj65, "深度:对象 65 层超限");
            free(obj65);
        }
    } else {
        check_true("深度:构造测试输入失败", 0);
    }
    free(ok64);
    free(bad65);

    /* 大规模数组:练 tmpvec 扩容与 arena 跨块(20000 个元素) */
    const size_t kN = 20000u;
    size_t cap = kN * 8u + 16u;
    char *big = (char *)malloc(cap);
    if (big) {
        size_t n = 0;
        big[n++] = '[';
        for (size_t i = 0; i < kN; ++i) {
            n += (size_t)snprintf(big + n, cap - n, "%s%zu", (i == 0) ? "" : ",", i);
        }
        big[n++] = ']';
        big[n] = '\0';
        sxcl_json *doc = NULL;
        const sxcl_json_value *root = parse_ok(&doc, big, "规模:20000 元素数组");
        if (root) {
            check_size("规模:元素个数", sxcl_json_size(root), kN);
            check_double("规模:最后一个元素", sxcl_json_number(sxcl_json_at(root, kN - 1)),
                         (double)(kN - 1));
            check_close("规模:第 12345 个元素", sxcl_json_number(sxcl_json_at(root, 12345)), 12345.0,
                        1e-15);
            check_ptr_null("规模:越界仍然是 NULL", sxcl_json_at(root, kN));
            sxcl_json_free(doc);
        }
        free(big);
    } else {
        check_true("规模:构造大数组失败", 0);
    }

    /* len 之外的内容不能被当成输入(输入不要求 NUL 结尾) */
    {
        char raw[32];
        memcpy(raw, "[1,2]", 5);
        raw[5] = 'X';
        char err[128];
        err[0] = '\0';
        sxcl_json *doc = sxcl_json_parse(raw, 5, err, sizeof err);
        check_ptr_nonnull("len:输入不要求 NUL 结尾", doc);
        if (doc) {
            check_size("len:元素个数", sxcl_json_size(sxcl_json_root(doc)), 2u);
            sxcl_json_free(doc);
        } else {
            fprintf(stderr, "     err=%s\n", err);
        }
    }
}

/* ── 9) 真实形状的小清单(模拟 version_manifest) ── */
static void test_manifest_shape(void)
{
    static const char kManifest[] =
        "{\"latest\":{\"release\":\"1.21.4\",\"snapshot\":\"25w03a\"},"
        "\"versions\":["
        "{\"id\":\"1.21.4\",\"type\":\"release\",\"url\":\"https://x/1.21.4.json\","
        "\"time\":\"2024-12-03T10:12:08+00:00\",\"sha1\":\"abc\",\"complianceLevel\":1},"
        "{\"id\":\"25w03a\",\"type\":\"snapshot\",\"url\":\"https://x/25w03a.json\","
        "\"time\":\"2025-01-15T12:00:00+00:00\",\"sha1\":\"def\",\"complianceLevel\":1}"
        "]}";
    sxcl_json *doc = NULL;
    const sxcl_json_value *root = parse_ok(&doc, kManifest, "清单:version_manifest 形状");
    if (!root) {
        return;
    }
    check_str("清单:latest.release", sxcl_json_get_string(sxcl_json_get(root, "latest"), "release", NULL),
              "1.21.4");
    const sxcl_json_value *versions = sxcl_json_get(root, "versions");
    check_size("清单:versions 长度", sxcl_json_size(versions), 2u);
    check_str("清单:versions[1].id", sxcl_json_get_string(sxcl_json_at(versions, 1), "id", NULL),
              "25w03a");
    check_i64("清单:complianceLevel", sxcl_json_get_int64(sxcl_json_at(versions, 0), "complianceLevel", -1),
              1);
    check_i64("清单:缺失字段走默认值",
              sxcl_json_get_int64(sxcl_json_at(versions, 0), "notThere", -1), -1);
    sxcl_json_free(doc);
}

/* ── 10) 文件解析 ── */
static void test_parse_file(void)
{
    const char *tmp_path = "sxcl_json_test_tmp.json";
    static const char kContent[] =
        "{\"ver\":7,\"name\":\"中文名称\",\"list\":[1,2,3],\"flag\":true,"
        "\"esc\":\"\\u4e2d\\uD83D\\uDE00\",\"nested\":{\"a\":{\"b\":[null]}}}";

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        check_true("文件:无法创建临时文件", 0);
        return;
    }
    const size_t written = fwrite(kContent, 1, strlen(kContent), f);
    fclose(f);
    check_size("文件:临时文件写入字节数", written, strlen(kContent));

    char err[256];
    err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse_file(tmp_path, err, sizeof err);
    check_ptr_nonnull("文件:parse_file 成功", doc);
    if (doc) {
        const sxcl_json_value *root = sxcl_json_root(doc);
        check_i64("文件:ver", sxcl_json_get_int64(root, "ver", -1), 7);
        check_str("文件:中文值", sxcl_json_get_string(root, "name", NULL), "中文名称");
        check_size("文件:list 长度", sxcl_json_size(sxcl_json_get(root, "list")), 3u);
        check_i64("文件:flag", sxcl_json_get_bool(root, "flag", 0), 1);
        check_str("文件:转义+代理对", sxcl_json_get_string(root, "esc", NULL),
                  "\xE4\xB8\xAD\xF0\x9F\x98\x80");
        const sxcl_json_value *nested = sxcl_json_get(root, "nested");
        check_true("文件:深层嵌套取到", nested != NULL);
        if (nested) {
            const sxcl_json_value *a = sxcl_json_get(nested, "a");
            const sxcl_json_value *b = a ? sxcl_json_get(a, "b") : NULL;
            check_size("文件:深层数组长度", sxcl_json_size(b), 1u);
            check_true("文件:深层 null 元素", sxcl_json_type_of(sxcl_json_at(b, 0)) == SXCL_JSON_NULL);
        }
        sxcl_json_free(doc);
    } else {
        fprintf(stderr, "     err=%s\n", err);
    }

    /* 不存在的文件 */
    char err2[256];
    err2[0] = '\0';
    sxcl_json *missing = sxcl_json_parse_file("sxcl_json_no_such_file_12345.json", err2, sizeof err2);
    check_ptr_null("文件:不存在的路径返回 NULL", missing);
    check_true("文件:不存在的路径 err 非空", err2[0] != '\0');
    printf("ok   文件:不存在的路径错误信息 = %s\n", err2);

    /* 空路径 */
    char err3[64];
    check_ptr_null("文件:空路径返回 NULL", sxcl_json_parse_file("", err3, sizeof err3));
    check_ptr_null("文件:NULL 路径返回 NULL", sxcl_json_parse_file(NULL, err3, sizeof err3));

    check_true("文件:删除临时文件", remove(tmp_path) == 0);
}

/* ── 11) NULL / 边界防御 ── */
static void test_api_robustness(void)
{
    char err[128];
    check_ptr_null("防御:parse(NULL)", sxcl_json_parse(NULL, 0, err, sizeof err));
    check_true("防御:parse(NULL) err 非空", err[0] != '\0');
    check_ptr_null("防御:parse 空串", sxcl_json_parse("", 0, err, sizeof err));
    check_ptr_null("防御:parse err 缓冲区为 NULL", sxcl_json_parse("bad", 3, NULL, 0));

    /* 很小的 err 缓冲区必须被 NUL 结尾且不越界 */
    char tiny[8];
    memset(tiny, 'Z', sizeof tiny);
    sxcl_json *doc = sxcl_json_parse("[1,", 3, tiny, sizeof tiny);
    check_ptr_null("防御:小 err 缓冲区仍返回 NULL", doc);
    check_true("防御:小 err 缓冲区 NUL 结尾", tiny[sizeof tiny - 1] == '\0');

    check_ptr_null("防御:root(NULL)", sxcl_json_root(NULL));
    check_true("防御:type_of(NULL) 是 NULL 类型", sxcl_json_type_of(NULL) == SXCL_JSON_NULL);
    check_size("防御:size(NULL)", sxcl_json_size(NULL), 0u);
    check_ptr_null("防御:at(NULL,5)", sxcl_json_at(NULL, 5));
    check_ptr_null("防御:string(NULL)", sxcl_json_string(NULL));
    check_double("防御:number(NULL)", sxcl_json_number(NULL), 0.0);
    check_str("防御:get_string(NULL)", sxcl_json_get_string(NULL, "k", "d"), "d");
    check_i64("防御:get_int64(NULL)", sxcl_json_get_int64(NULL, "k", 5), 5);
    check_i64("防御:get_bool(NULL)", sxcl_json_get_bool(NULL, "k", 1), 1);
    sxcl_json_free(NULL);          /* 必须安全 */
    check_true("防御:free(NULL) 不崩溃", 1);
}

/* ── 12) 错误信息带行列号 ── */
static void test_error_position(void)
{
    static const char kBad[] = "{\n  \"a\": 1,\n  \"b\": [1, 2,, 3]\n}";
    char err[256];
    err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(kBad, strlen(kBad), err, sizeof err);
    check_ptr_null("行列:多行非法输入返回 NULL", doc);
    check_true("行列:错误信息含 '行' 与 '列'",
               strstr(err, "行") != NULL && strstr(err, "列") != NULL);
    printf("ok   行列:错误信息 = %s\n", err);
    /* 第 3 行第 12 列附近(列按 UTF-8 码点数,ASCII 输入即字节数) */
    check_true("行列:行号是 3", strstr(err, "第 3 行") != NULL);

    static const char kBadUtf8[] = "{\"中文键\": \"值\", }";
    err[0] = '\0';
    doc = sxcl_json_parse(kBadUtf8, strlen(kBadUtf8), err, sizeof err);
    check_ptr_null("行列:中文输入非法", doc);
    printf("ok   行列:中文输入错误信息 = %s\n", err);
    check_true("行列:中文输入列号按码点数", strstr(err, "第 1 行第 14 列") != NULL);
}

int main(void)
{
    test_basic();
    test_strings();
    test_mismatch();
    test_invalid();
    test_scalars();
    test_numbers();
    test_duplicate_keys();
    test_depth_and_scale();
    test_manifest_shape();
    test_parse_file();
    test_api_robustness();
    test_error_position();

    printf("\n==== sxcl json 测试:%d 项检查,%d 项失败 ====\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
