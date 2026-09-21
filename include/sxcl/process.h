/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_PROCESS_H
#define SXCL_PROCESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sxcl_process_opts {
    const char *program;      /**< 可执行文件(可只给名字,按 PATH 查找);UTF-8 */
    const char *const *args;  /**< 参数数组,NULL 结尾;**不含** program 本身;可空 */
    const char *work_dir;     /**< 工作目录;可空 = 继承 */
    const char *const *env;   /**< 追加/覆盖的环境变量,"KEY=VALUE" 数组,NULL 结尾;可空 */
    int timeout_ms;           /**< <=0 表示不限时 */
    /** 每读到一行输出调用一次(尾随换行已去掉)。
     *  返回非 0 = 请求终止进程(用于"发现安装完成标记就收工"这类场景)。 */
    int (*on_line)(void *userdata, int is_stderr, const char *line);
    void *userdata;
    /** 进程**真的起来了**时调用一次,参数是它的 PID(在 sxcl_process_run 的同一条线程里)。
     *  可空。存在的理由:界面要在进程还在跑的时候就把 PID 显示出来、并且能**单独结束它** ——
     *  只靠 sxcl_process_result.pid 的话要等到进程结束才知道(那就没用了)。
     *  返回非 0 = 立刻终止这个进程(与 on_line 返回非 0 同一语义,算 killed_by_client)。 */
    int (*on_started)(void *userdata, int64_t pid);
} sxcl_process_opts;

typedef struct sxcl_process_result {
    int exit_code;        /**< 进程退出码;未能启动时为 -1 */
    int timed_out;        /**< 1 = 超时被我们终止 */
    int killed_by_client; /**< 1 = on_line / on_started 回调请求终止 */
    int64_t elapsed_ms;
    int64_t pid;          /**< 子进程 PID;**没能启动时为 -1**。进程结束后它仍然保留
                           *   (用来对日志/crash-report,或事后核对"是不是它")。 */
    char error[192];      /**< 失败原因(人话) */
} sxcl_process_result;

/** 运行并等待结束。返回 0 = 正常跑完(退出码在 out 里);<0 = 没能启动或出错。 */
int sxcl_process_run(const sxcl_process_opts *opts, sxcl_process_result *out);

/* ── 单独操作一个"我们自己起的"子进程 ──
 *
 * 为什么要有:启动器把游戏作为**独立进程**起出去之后,界面要能
 *   ① 显示它还活着(不是"启动器还在 = 游戏还在");
 *   ② 用户点"结束游戏"时**只结束游戏**、启动器自己不受影响;
 *   ③ 启动器的取消不再需要"等进程下一次输出"才生效。
 *
 * **范围**:只针对本进程自己(或同用户)起的 PID。不遍历进程表、不按名字匹配、不猜 ——
 * 免得误杀别的程序。返回码都是"尽力而为"的语义,不做任何持久化。 */

/** 这个 PID 现在还在不在。1 = 在,0 = 不在(或没权限判断/参数非法)。
 *  **注意**:PID 会被操作系统复用,所以它只适合"刚起的那个子进程还在不在"这种短语义;
 *  不要拿它做长期身份判断。 */
int sxcl_process_pid_alive(int64_t pid);

/** 请求终止这个 PID(先温和后强硬由实现决定)。返回 0 = 已经发出终止请求(不代表已死透,
 *  要确认请接着轮询 sxcl_process_pid_alive)。pid <= 0 / 系统调用失败返回负。 */
int sxcl_process_kill_pid(int64_t pid);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_PROCESS_H */
