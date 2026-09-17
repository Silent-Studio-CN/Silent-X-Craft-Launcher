/* settings 存储测试:默认值、顺序、解析回退、坏行/注释/CRLF、每实例键、原子写往返。
 * 断言式:任一断言失败,main 返回 1。 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdio.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/settings.h"

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
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)");
    }
}

static void check_i64(int64_t got, int64_t want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %lld want %lld\n", what, (long long)got, (long long)want);
    }
}

/* 不用 fabs()/math.h:测试也不想拉数学库;这里的取值都是能精确表示的。 */
static void check_dbl(double got, double want, const char *what)
{
    double d = got - want;
    if (d < 0) {
        d = -d;
    }
    if (d <= 1e-9) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %.17g want %.17g\n", what, got, want);
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

static char *read_file(const char *path, char *buf, size_t n)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return NULL;
    }
    const size_t got = fread(buf, 1, n - 1, fh);
    buf[got] = '\0';
    fclose(fh);
    return buf;
}

int main(void)
{
    const char *dir  = "build/_settings_tmp";
    const char *raw  = "build/_settings_tmp/raw.txt";
    const char *rt   = "build/_settings_tmp/roundtrip.txt";
    const char *rtmp = "build/_settings_tmp/roundtrip.txt.tmp";
    const char *zh   = "build/_settings_tmp/中文路径 设置.txt";
    char buf[4096];
    char snap_key[32][96];
    char snap_val[32][160];

    check(sxcl_fs_mkdirs(dir) == 0, "建临时目录(否则后面写文件全失败)");

    /* ── 1) 文件不存在 = 全新,逐个默认值 ── */
    {
        sxcl_settings *s = sxcl_settings_open("build/_settings_tmp/压根不存在.txt");
        check(s != NULL, "文件不存在也返回可用句柄(不算失败)");
        check_i64((int64_t)sxcl_settings_count(s), 0, "新建设置 count=0");
        check_str(sxcl_settings_download_rate_text(s), "0", "download.rate 默认 \"0\"");
        check_i64(sxcl_settings_download_workers(s), 0, "download.workers 默认 0");
        check_i64(sxcl_settings_download_max_conn(s), 1, "download.max_conn 默认 1");
        check_str(sxcl_settings_download_cache_dir(s), "", "download.cache_dir 默认 空串");
        check_str(sxcl_settings_game_default_dir(s), "", "game.default_dir 默认 空串");
        check_str(sxcl_settings_ui_theme(s), "auto", "ui.theme 默认 auto");
        check_str(sxcl_settings_ui_language(s), "zh-CN", "ui.language 默认 zh-CN");
        check_str(sxcl_settings_instance_graphics_api(s, "默认实例"), "default", "实例 graphicsApi 默认 default");
        check_i64((int64_t)sxcl_settings_count(s), 0, "读默认值不会往表里塞键");
        check_str(sxcl_settings_get(s, "没有的键", "回退值"), "回退值", "get 取不到返回 def");
        check(sxcl_settings_get(s, "没有的键", NULL) == NULL, "get 的 def 可以是 NULL");
        sxcl_settings_free(s);
        sxcl_settings_free(NULL); /* 传 NULL 不能崩 */
        check(1, "free(NULL) 安全");
    }

    /* ── 2) 设值 / 取值 / 删除 / 条数 / 顺序 ── */
    {
        sxcl_settings *s = sxcl_settings_open("build/_settings_tmp/顺序.txt");
        check(sxcl_settings_set(s, "ui.theme", "dark") == 0, "设值 ui.theme");
        check(sxcl_settings_set(s, "download.workers", "8") == 0, "设值 download.workers");
        check(sxcl_settings_set(s, "download.rate", "1048576") == 0, "设值 download.rate");
        check_i64((int64_t)sxcl_settings_count(s), 3, "3 条");
        check_str(sxcl_settings_key_at(s, 0), "ui.theme", "插入顺序:第 0 条 ui.theme");
        check_str(sxcl_settings_key_at(s, 1), "download.workers", "插入顺序:第 1 条 download.workers");
        check_str(sxcl_settings_value_at(s, 2), "1048576", "value_at 对得上");

        check(sxcl_settings_set(s, "ui.theme", "light") == 0, "改已有键的值");
        check_str(sxcl_settings_get(s, "ui.theme", NULL), "light", "改完取到新值");
        check_str(sxcl_settings_key_at(s, 0), "ui.theme", "改值不换位置");
        check_i64((int64_t)sxcl_settings_count(s), 3, "改值不新增条目");

        check(sxcl_settings_set(s, "ui.language", "en-US") == 0, "新增键");
        check_str(sxcl_settings_key_at(s, 3), "ui.language", "新增追加到末尾");
        check_i64((int64_t)sxcl_settings_count(s), 4, "新增后 4 条");
        check(sxcl_settings_key_at(s, 4) == NULL, "key_at 越界返回 NULL");
        check(sxcl_settings_value_at(s, 99) == NULL, "value_at 越界返回 NULL");

        check(sxcl_settings_remove(s, "download.workers") == 0, "删除 download.workers");
        check(sxcl_settings_get(s, "download.workers", NULL) == NULL, "删掉后取不到");
        check(sxcl_settings_remove(s, "download.workers") == 0, "删不存在的键不算错");
        check_i64((int64_t)sxcl_settings_count(s), 3, "删除后 3 条");
        check_str(sxcl_settings_key_at(s, 1), "download.rate", "删除后后面的条目前移");

        check(sxcl_settings_set(s, "坏 键", "1") == -1, "键名带空格被拒");
        check(sxcl_settings_set(s, "bad=key", "1") == -1, "键名带 = 被拒");
        check(sxcl_settings_set(s, "bad#key", "1") == -1, "键名带 # 被拒");
        check(sxcl_settings_set(s, "", "1") == -1, "空键名被拒");
        check(sxcl_settings_set(NULL, "k", "1") == -1, "settings 为 NULL 被拒");
        check_i64((int64_t)sxcl_settings_count(s), 3, "被拒的键没有进表");

        /* ── 3) int / double / bool 解析 ── */
        check(sxcl_settings_set(s, "n.ok", "42") == 0, "写 n.ok");
        check(sxcl_settings_set(s, "n.neg", "-7") == 0, "写 n.neg");
        check(sxcl_settings_set(s, "n.max", "9223372036854775807") == 0, "写 n.max");
        check(sxcl_settings_set(s, "n.over", "99999999999999999999") == 0, "写 n.over(溢出)");
        check(sxcl_settings_set(s, "n.bad", "abc") == 0, "写 n.bad");
        check(sxcl_settings_set(s, "n.frac", "3.5") == 0, "写 n.frac");
        check(sxcl_settings_set(s, "n.empty", "") == 0, "写 n.empty(空值)");
        check_i64(sxcl_settings_get_int(s, "n.ok", -1), 42, "int 合法值");
        check_i64(sxcl_settings_get_int(s, "n.neg", -1), -7, "int 负数");
        check_i64(sxcl_settings_get_int(s, "n.max", -1), INT64_MAX, "int INT64_MAX");
        check_i64(sxcl_settings_get_int(s, "n.over", -1), -1, "int 溢出回退 def");
        check_i64(sxcl_settings_get_int(s, "n.bad", -1), -1, "int 非法回退 def");
        check_i64(sxcl_settings_get_int(s, "n.frac", -1), -1, "int 不接受小数");
        check_i64(sxcl_settings_get_int(s, "n.empty", -1), -1, "int 空值回退 def");
        check_i64(sxcl_settings_get_int(s, "没有的键", 123), 123, "int 缺键回退 def");

        check(sxcl_settings_set(s, "d.ok", "3.25") == 0, "写 d.ok");
        check(sxcl_settings_set(s, "d.neg", "-2.5") == 0, "写 d.neg");
        check(sxcl_settings_set(s, "d.exp", "1e3") == 0, "写 d.exp");
        check(sxcl_settings_set(s, "d.point", ".5") == 0, "写 d.point(无整数部分)");
        check(sxcl_settings_set(s, "d.bad", "1.2.3") == 0, "写 d.bad");
        check(sxcl_settings_set(s, "d.badexp", "1e") == 0, "写 d.badexp");
        check_dbl(sxcl_settings_get_double(s, "d.ok", -1.0), 3.25, "double 合法值");
        check_dbl(sxcl_settings_get_double(s, "d.neg", 0.0), -2.5, "double 负数");
        check_dbl(sxcl_settings_get_double(s, "d.exp", 0.0), 1000.0, "double 指数");
        check_dbl(sxcl_settings_get_double(s, "d.point", 0.0), 0.5, "double .5");
        check_dbl(sxcl_settings_get_double(s, "d.bad", -1.0), -1.0, "double 非法回退 def");
        check_dbl(sxcl_settings_get_double(s, "d.badexp", -1.0), -1.0, "double 坏指数回退 def");
        check_dbl(sxcl_settings_get_double(s, "没有的键", 2.5), 2.5, "double 缺键回退 def");

        static const struct { const char *v; int want; } bools[] = {
            { "1", 1 }, { "0", 0 }, { "true", 1 }, { "TRUE", 1 }, { "True", 1 },
            { "false", 0 }, { "FALSE", 0 }, { "yes", 1 }, { "YES", 1 },
            { "no", 0 }, { "No", 0 },
        };
        for (size_t i = 0; i < sizeof(bools) / sizeof(bools[0]); ++i) {
            check(sxcl_settings_set(s, "b.v", bools[i].v) == 0, "写 b.v");
            check(sxcl_settings_get_bool(s, "b.v", -1) == bools[i].want, "bool 取值(1/0/true/false/yes/no,不分大小写)");
        }
        check(sxcl_settings_set(s, "b.v", "maybe") == 0, "写 b.v=maybe");
        check(sxcl_settings_get_bool(s, "b.v", 1) == 1, "bool 非法回退 def=1");
        check(sxcl_settings_get_bool(s, "b.v", 0) == 0, "bool 非法回退 def=0");
        check(sxcl_settings_get_bool(s, "没有的键", 1) == 1, "bool 缺键回退 def");
        check(sxcl_settings_remove(s, "b.v") == 0, "清掉 b.v");
        check(sxcl_settings_remove(s, "n.ok") == 0, "清掉 n.ok");
        check(sxcl_settings_remove(s, "n.neg") == 0, "清掉 n.neg");
        check(sxcl_settings_remove(s, "n.max") == 0, "清掉 n.max");
        check(sxcl_settings_remove(s, "n.over") == 0, "清掉 n.over");
        check(sxcl_settings_remove(s, "n.bad") == 0, "清掉 n.bad");
        check(sxcl_settings_remove(s, "n.frac") == 0, "清掉 n.frac");
        check(sxcl_settings_remove(s, "n.empty") == 0, "清掉 n.empty");
        check(sxcl_settings_remove(s, "d.ok") == 0, "清掉 d.ok");
        check(sxcl_settings_remove(s, "d.neg") == 0, "清掉 d.neg");
        check(sxcl_settings_remove(s, "d.exp") == 0, "清掉 d.exp");
        check(sxcl_settings_remove(s, "d.point") == 0, "清掉 d.point");
        check(sxcl_settings_remove(s, "d.bad") == 0, "清掉 d.bad");
        check(sxcl_settings_remove(s, "d.badexp") == 0, "清掉 d.badexp");
        check_i64((int64_t)sxcl_settings_count(s), 3, "又回到 3 条");
        sxcl_settings_free(s);
    }

    /* ── 4) 磁盘格式:注释行 / 空行 / 坏行 / 值里带 = / 空值 / CRLF ── */
    check(write_file(raw,
                     "# 顶部注释(写回时会被丢掉)\r\n"
                     "download.workers=4\r\n"
                     "\r\n"
                     "empty.value=\r\n"
                     "eq.value=a=b=c\r\n"
                     "noequals\r\n"
                     "=nokey\r\n"
                     "bad key=1\r\n"
                     "download.workers=9\r\n") == 0,
          "写测试文件(CRLF)");
    {
        sxcl_settings *s2 = sxcl_settings_open(raw);
        check(s2 != NULL, "打开 raw.txt");
        check_i64((int64_t)sxcl_settings_count(s2), 3, "坏行/空行/注释行都跳过,只剩 3 条");
        check_str(sxcl_settings_get(s2, "eq.value", NULL), "a=b=c", "值里含 = 只按第一个 = 切");
        check_str(sxcl_settings_get(s2, "empty.value", NULL), "", "空值合法");
        check_str(sxcl_settings_get(s2, "empty.value", "x"), "", "空值不会当成\"不存在\"");
        check(sxcl_settings_get(s2, "noequals", NULL) == NULL, "没有 = 的坏行跳过");
        check(sxcl_settings_get(s2, "bad key", NULL) == NULL, "键名非法的坏行跳过");
        check(sxcl_settings_get(s2, "nokey", NULL) == NULL, "空键名的坏行跳过");
        check_i64(sxcl_settings_download_workers(s2), 9, "CRLF 被剥干净且同名键后者覆盖前者");
        check(strchr(sxcl_settings_get(s2, "eq.value", ""), '\r') == NULL, "值里没有残留 CR");

        /* ── 5) 每实例设置 ── */
        check_str(sxcl_settings_instance_get(s2, "默认实例", "graphicsApi", "default"), "default",
                  "实例键不存在返回 def");
        check_str(sxcl_settings_instance_graphics_api(s2, "别的实例"), "default", "别的实例默认也是 default");
        check(sxcl_settings_instance_set(s2, "默认实例", "graphicsApi", "vulkan") == 0, "写实例 graphicsApi");
        check_str(sxcl_settings_instance_graphics_api(s2, "默认实例"), "vulkan", "读回实例 graphicsApi");
        check_str(sxcl_settings_instance_graphics_api(s2, "别的实例"), "default", "实例之间互不干扰");
        check_str(sxcl_settings_get(s2, "instance.默认实例.graphicsApi", NULL), "vulkan",
                  "内部键名就是 instance.<实例名>.<键>");
        check(sxcl_settings_instance_set(s2, "默认实例", "javaPath", "C:/jdk/bin/java.exe") == 0,
              "写实例 javaPath(值里有 : 和 /)");
        check(sxcl_settings_instance_set(NULL, "i", "graphicsApi", "vulkan") == -1, "instance_set(NULL) 被拒");
        check(sxcl_settings_instance_set(s2, "", "graphicsApi", "vulkan") == -1, "空实例名被拒");
        check_str(sxcl_settings_instance_get(s2, NULL, "graphicsApi", "回退"), "回退", "instance_get(NULL) 回退");
        check(sxcl_settings_instance_graphics_api(s2, NULL) != NULL, "instance_graphics_api(NULL) 不崩");

        /* ── 6) 保存 -> 重新打开:逐条比对(往返) ── */
        check(sxcl_settings_set(s2, "ui.theme", "dark") == 0, "写 ui.theme");
        check(sxcl_settings_set(s2, "空 key", "x") == -1, "非法键依旧被拒");
        const size_t nb = sxcl_settings_count(s2);
        check(nb > 0 && nb <= 32, "条目数在快照上限内");
        for (size_t i = 0; i < nb; ++i) {
            snprintf(snap_key[i], sizeof snap_key[i], "%s", sxcl_settings_key_at(s2, i));
            snprintf(snap_val[i], sizeof snap_val[i], "%s", sxcl_settings_value_at(s2, i));
        }
        check(sxcl_settings_save(s2, rt) == 0, "保存到 roundtrip.txt");
        check(sxcl_settings_save(NULL, rt) == -1, "save(NULL) 被拒");
        check(sxcl_settings_save(s2, NULL) == -1, "save(NULL 路径) 被拒");
        sxcl_settings_free(s2);

        sxcl_settings *s3 = sxcl_settings_open(rt);
        check(s3 != NULL, "重新打开 roundtrip.txt");
        check_i64((int64_t)sxcl_settings_count(s3), (int64_t)nb, "往返后条数一致");
        int same = 1;
        for (size_t i = 0; i < nb; ++i) {
            const char *k = sxcl_settings_key_at(s3, i);
            const char *v = sxcl_settings_value_at(s3, i);
            if (!k || strcmp(k, snap_key[i]) != 0 || !v || strcmp(v, snap_val[i]) != 0) {
                printf("  [!!] 往返第 %d 条不一致\n", (int)i);
                same = 0;
            }
        }
        check(same, "往返后键、值、顺序完全一致");
        check_str(sxcl_settings_instance_graphics_api(s3, "默认实例"), "vulkan", "往返后实例 graphicsApi 还在");
        check_str(sxcl_settings_instance_get(s3, "默认实例", "javaPath", ""), "C:/jdk/bin/java.exe",
                  "往返后实例 javaPath 还在");
        check_i64(sxcl_settings_download_workers(s3), 9, "往返后 download.workers");

        /* 删除也要落盘 */
        check(sxcl_settings_remove(s3, "empty.value") == 0, "删除 empty.value");
        check(sxcl_settings_save(s3, rt) == 0, "再保存");
        sxcl_settings_free(s3);
        sxcl_settings *s3b = sxcl_settings_open(rt);
        check(sxcl_settings_get(s3b, "empty.value", NULL) == NULL, "删除在往返后生效");
        check_i64((int64_t)sxcl_settings_count(s3b), (int64_t)nb - 1, "往返后条数 -1");
        sxcl_settings_free(s3b);
    }

    /* ── 7) 原子写:文件里没有 CR,.tmp 不残留 ── */
    {
        check(read_file(rt, buf, sizeof(buf)) != NULL, "回读 roundtrip.txt");
        check(strchr(buf, '\r') == NULL, "保存后的文件里不含 \\r(写出的是 LF)");
        check(strstr(buf, "instance.默认实例.graphicsApi=vulkan") != NULL, "文件里能看到实例键");
        check(strchr(buf, '#') == NULL, "注释不会被写回(头文件已声明)");
        check(sxcl_fs_exists(rtmp) == 0, "临时文件 .tmp 已被改名,不残留");
    }

    /* ── 8) UTF-8 路径(_wfopen_s) ── */
    {
        sxcl_settings *s4 = sxcl_settings_open(zh);
        check(s4 != NULL, "UTF-8 路径也能打开");
        check(sxcl_settings_set(s4, "ui.theme", "深色") == 0, "UTF-8 路径下设值");
        check(sxcl_settings_set(s4, "中文键", "ok") == 0, "UTF-8 键名可以用");
        check(sxcl_settings_save(s4, zh) == 0, "UTF-8 路径保存(先写 tmp 再改名)");
        sxcl_settings_free(s4);

        check(sxcl_fs_exists(zh) == 1, "UTF-8 路径文件真的落盘了");
        sxcl_settings *s5 = sxcl_settings_open(zh);
        check_str(sxcl_settings_ui_theme(s5), "深色", "UTF-8 路径往返 ui.theme");
        check_str(sxcl_settings_get(s5, "中文键", NULL), "ok", "UTF-8 路径往返中文键");
        sxcl_settings_free(s5);
    }

    printf("settings 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
