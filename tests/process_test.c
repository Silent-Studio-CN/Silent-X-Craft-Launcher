/* 进程启动与输出捕获测试:正常输出、退出码、超时终止、回调主动终止 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* strcat 在 MSVC 下默认被标记弃用 */
#endif
#include <stdio.h>
#include <string.h>

#include "sxcl/process.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *what) {
    if (ok) { ++g_pass; } else { ++g_fail; printf("  [!!] %s\n", what); }
}

typedef struct collector {
    char out[1024];
    char err[1024];
    int lines;
    int stop_after;
} collector;

static int on_line(void *ud, int is_stderr, const char *line) {
    collector *c = (collector *)ud;
    char *dst = is_stderr ? c->err : c->out;
    const size_t used = strlen(dst);
    if (used + strlen(line) + 2 < sizeof(c->out)) {
        if (used > 0) { strcat(dst, "|"); }
        strcat(dst, line);
    }
    ++c->lines;
    return (c->stop_after > 0 && c->lines >= c->stop_after) ? 1 : 0;
}

#if defined(_WIN32)
#  define SH "cmd"
#  define SH_C "/c"
#else
#  define SH "/bin/sh"
#  define SH_C "-c"
#endif

int main(void) {
    sxcl_process_result res;
    collector c;

    /* 1) 正常输出 + 退出码 0 */
    memset(&c, 0, sizeof(c));
    const char *args1[] = {SH_C, "echo hello-from-sxcl", NULL};
    sxcl_process_opts o1;
    memset(&o1, 0, sizeof(o1));
    o1.program = SH;
    o1.args = args1;
    o1.on_line = on_line;
    o1.userdata = &c;
    check(sxcl_process_run(&o1, &res) == 0, "启动并等待成功");
    check(res.exit_code == 0, "退出码 0");
    check(strstr(c.out, "hello-from-sxcl") != NULL, "捕获到 stdout");
    printf("   stdout=[%s] 用时=%lldms\n", c.out, (long long)res.elapsed_ms);

    /* 2) 非零退出码 */
    memset(&c, 0, sizeof(c));
    const char *args2[] = {SH_C, "exit 3", NULL};
    sxcl_process_opts o2;
    memset(&o2, 0, sizeof(o2));
    o2.program = SH;
    o2.args = args2;
    o2.on_line = on_line;
    o2.userdata = &c;
    check(sxcl_process_run(&o2, &res) == 0, "非零退出也能正常返回");
    check(res.exit_code == 3, "退出码被原样带回");

    /* 3) 超时终止 */
#if defined(_WIN32)
    const char *args3[] = {SH_C, "ping -n 6 127.0.0.1 > nul", NULL};
#else
    const char *args3[] = {SH_C, "sleep 5", NULL};
#endif
    sxcl_process_opts o3;
    memset(&o3, 0, sizeof(o3));
    o3.program = SH;
    o3.args = args3;
    o3.timeout_ms = 400;
    check(sxcl_process_run(&o3, &res) == 0, "超时用例能返回");
    check(res.timed_out == 1, "超时被标记");
    check(res.elapsed_ms < 3000, "没有真等满 5 秒");

    /* 4) 回调主动终止(安装器"看到完成标记就收工"就是这个用法) */
    memset(&c, 0, sizeof(c));
    c.stop_after = 2;
#if defined(_WIN32)
    const char *args4[] = {SH_C, "echo one & echo two & echo three & ping -n 6 127.0.0.1 > nul", NULL};
#else
    const char *args4[] = {SH_C, "echo one; echo two; echo three; sleep 5", NULL};
#endif
    sxcl_process_opts o4;
    memset(&o4, 0, sizeof(o4));
    o4.program = SH;
    o4.args = args4;
    o4.on_line = on_line;
    o4.userdata = &c;
    o4.timeout_ms = 5000;
    check(sxcl_process_run(&o4, &res) == 0, "回调终止用例能返回");
    check(res.killed_by_client == 1, "回调请求终止被标记");
    check(res.elapsed_ms < 3000, "没等后面的 sleep");

    printf("process 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
