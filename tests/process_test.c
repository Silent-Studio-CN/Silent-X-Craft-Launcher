/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

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

/* PID 用例的夹具:on_started 记下 PID(不要求终止),on_line 在进程**还在跑**的时候
 * 用 sxcl_process_kill_pid 把它**单独结束**掉 —— 这正是界面"结束游戏"要用的那条路。 */
typedef struct pid_probe {
    long long pid;
    int alive_during_run; /* 进程还在跑时查 sxcl_process_pid_alive 的结果 */
    int kill_rc;          /* sxcl_process_kill_pid 的返回码(没来得及调就是 99) */
    int lines;
} pid_probe;

static int capture_pid(void *ud, int64_t pid) {
    pid_probe *p = (pid_probe *)ud;
    p->pid = (long long)pid;
    return 0; /* **不**在这里终止:要在外面用 kill_pid 单独结束 */
}

static int kill_on_first_line(void *ud, int is_stderr, const char *line) {
    (void)is_stderr;
    (void)line;
    pid_probe *p = (pid_probe *)ud;
    if (++p->lines == 1) {
        p->alive_during_run = sxcl_process_pid_alive(p->pid);
        p->kill_rc = sxcl_process_kill_pid(p->pid);
    }
    return 0; /* 不用回调返回值终止 —— 那是另一条路,这里专门验 kill_pid */
}

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

    /* 5) PID:短命令跑完之后 pid > 0,而且**进程结束后查得到"它不在了"**
     *    (启动器要显示"游戏还在不在"、并且能单独结束它,靠的就是这两个能力) */
    memset(&c, 0, sizeof(c));
    const char *args5[] = {SH_C, "echo pid-case", NULL};
    sxcl_process_opts o5;
    memset(&o5, 0, sizeof(o5));
    o5.program = SH;
    o5.args = args5;
    o5.on_line = on_line;
    o5.userdata = &c;
    check(sxcl_process_run(&o5, &res) == 0, "PID 用例能返回");
    check(res.pid > 0, "跑完的进程有 PID");
    check(sxcl_process_pid_alive(res.pid) == 0, "进程结束后判为「已不在」");
    check(sxcl_process_pid_alive(-1) == 0, "非法 PID 判为不在");
    printf("   pid=%lld alive-after-exit=%d\n", (long long)res.pid,
           sxcl_process_pid_alive(res.pid));

    /* 6) on_started:进程**还在跑的时候**就能拿到 PID(界面显示 PID / 单独结束游戏靠它),
     *    并且 sxcl_process_kill_pid 真的能把它单独结束掉。 */
#if defined(_WIN32)
    /* 每秒钟打一行,保证 on_line 会被调到(ping 的 "> nul" 会把输出吃掉,这里不能用) */
    const char *args6[] = {SH_C, "ping -n 20 127.0.0.1", NULL};
#else
    const char *args6[] = {SH_C, "echo tick; sleep 20", NULL};
#endif
    pid_probe probe;
    memset(&probe, 0, sizeof(probe));
    probe.pid = -1;
    probe.kill_rc = 99;
    sxcl_process_opts o6;
    memset(&o6, 0, sizeof(o6));
    o6.program = SH;
    o6.args = args6;
    o6.on_started = capture_pid;      /* 起来就记下 PID(不终止) */
    o6.on_line = kill_on_first_line;  /* 跑着的时候用 kill_pid 单独结束它 */
    o6.userdata = &probe;
    o6.timeout_ms = 8000;             /* 兜底:万一 kill 没生效,别把测试挂 20 秒 */
    check(sxcl_process_run(&o6, &res) == 0, "on_started 用例能返回");
    check(probe.pid > 0, "on_started 在进程**还在跑**时就拿到了 PID");
    check(probe.pid == (long long)res.pid, "on_started 的 PID 与结果里的一致");
    check(probe.alive_during_run == 1, "进程运行期间判为「还在」");
    check(probe.kill_rc == 0, "sxcl_process_kill_pid 接受了终止请求");
    check(res.timed_out == 0, "是 kill_pid 结束的,不是超时");
    check(sxcl_process_pid_alive(probe.pid) == 0, "被 kill 之后判为「已不在」");
    check(res.elapsed_ms < 6000, "进程被提前结束(没等满 20 秒)");
    printf("   on_started pid=%lld alive-during=%d kill-rc=%d elapsed=%lldms\n", probe.pid,
           probe.alive_during_run, probe.kill_rc, (long long)res.elapsed_ms);

    printf("process 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
