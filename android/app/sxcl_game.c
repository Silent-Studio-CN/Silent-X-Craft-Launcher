/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* sxcl_game.c - :game 进程(游戏进程)的原生层(Android only)。
 *
 * 它负责四件事,一件都不多:
 *   1) 把 :game 进程自己的 JVM 起起来 —— 复用 android/app/sxcl_jre_bootstrap.c
 *      (和诊断探针同一份实现,不复制);
 *   2) 与主进程的通道(和主进程 GameHost.java 的 LocalServerSocket 对接);
 *   3) 把本进程的 stdout/stderr 接成"日志文件 + 通道 + logcat"三条出口;
 *   4) 崩溃/被杀时留下**可被主进程读到的**退出记录(信号号 + 128+sig 退出码)。
 *
 * 通道为什么是 AF_UNIX 抽象名字的本地 socket(而不是 pipe / Binder):
 *   * pipe 需要"先有 fd 再把 fd 交给子进程" —— 而 :game 进程是 ActivityManager 拉起来的
 *     (Activity 的 android:process=":game"),我们在它出生之前拿不到它的任何 fd,
 *     也没有可传递 fd 的父子进程关系。**名字空间是唯一能在"对方还没出生"时就准备好的会合点。**
 *   * Binder 要写 AIDL、要在 :game 里声明并绑定一个 Service;而且 Binder 是同步事务语义,
 *     对端崩溃时调用方拿到的是 DeadObjectException —— 信号号/退出码仍然拿不到,还得另补旁路。
 *   * 抽象名字的本地 socket:同一 UID 内可用、不需要文件系统权限与 SELinux 标签、
 *     进程一死对端立刻读到 EOF(这就是"对端没了"的可靠信号),写失败立刻 EPIPE/ECONNRESET。
 *
 * 异常断开怎么办(三种情况都写清楚,而且都不靠猜):
 *   * :game 被 kill -9 / 被系统 OOM 杀:信号不可捕获,通道直接 EOF。主进程按
 *     "通道关闭 + 没有退出记录 + /proc/<pid> 消失"报 killed(退出码/信号不可观测)——
 *     这是**事实**,不是猜测:我们从进程外面本来就无法读取非子进程的退出状态。
 *   * :game 自己崩(ABRT/BUS/ILL/FPE/TERM,可捕获):崩溃兜底在看门线程里回调
 *     game_fatal_hook(),把 "signal=N code=128+N" 用裸 write()/send() 写进通道**并且**
 *     写进预先打开的退出记录文件(崩溃路径里不 malloc、不 open,只 write+fsync)。
 *   * 主进程先没了(用户划掉启动器):这里 send 失败(EPIPE/ECONNRESET)只把通道置灰,
 *     日志照旧写文件、logcat 照旧,游戏继续跑 —— **通道不是游戏的前提**。
 */

#include "sxcl_jre_bootstrap.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SXCL_GAME_TAG "sxcl-game"
#define SXCL_GAME_PATH 512
#define SXCL_GAME_LINE 2048
#define SXCL_GAME_ARG_MAX 64

/* ═════════ 传输层(可整段搬走:只依赖 libc,零 UI 依赖) ═════════
 *
 * 与主进程 GameHost 的本地 socket 对接:AF_UNIX **抽象名字**(不落文件系统、不涉 SELinux 标签,
 * 同一 UID 内可用)。为什么不是 pipe / Binder,以及异常断开怎么办,见文件头的说明。
 * 将来把游戏运行器整体搬到另一个应用时,这一段是最先可以整段带走的代码。
 *
 * 断了对端怎么办(sxcl_chan_send 的语义,刻意与产品行为一致):
 *   写失败 = 把通道标记为不可用(fd 置 -1)并返回 -1,**不重试、不阻塞、不崩**;
 *   调用方继续把日志写自己的文件 —— 通道永远不是游戏的前提。
 */
#define SXCL_CHAN_LINE 2048

typedef struct sxcl_chan {
    int fd;                        /* -1 = 不可用(没连上 / 对端已断) */
    char buf[SXCL_CHAN_LINE];      /* 读缓冲:按换行切行 */
    size_t used;
    pthread_mutex_t lock;          /* 只保护发送;崩溃路径走 sxcl_chan_send_raw,不加锁 */
} sxcl_chan;

static int sxcl_chan_is_open(const sxcl_chan *c)
{
    return c != NULL && c->fd >= 0;
}

/* 连一个抽象名字的本地 socket;timeout_ms 内按 100ms 轮询重试(主进程可能还没建好监听)。 */
static int sxcl_chan_open(sxcl_chan *c, const char *abstract_name, int timeout_ms)
{
    struct sockaddr_un addr;
    const size_t name_len = abstract_name != NULL ? strlen(abstract_name) : 0;
    int attempt = 0;
    int max_attempts = 1;

    if (c == NULL)
        return -1;
    (void)memset(c, 0, sizeof(*c));
    c->fd = -1;
    (void)pthread_mutex_init(&c->lock, NULL);
    if (name_len == 0 || name_len > sizeof(addr.sun_path) - 2)
        return -1;
    if (timeout_ms > 0)
        max_attempts = timeout_ms / 100 > 1 ? timeout_ms / 100 : 1;

    for (attempt = 0; attempt < max_attempts; ++attempt) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        (void)memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        /* 抽象名字:第一个字节为 0,后面跟名字(不需要文件系统节点) */
        addr.sun_path[0] = '\0';
        (void)memcpy(addr.sun_path + 1, abstract_name, name_len);
        if (connect(fd, (struct sockaddr *)&addr,
                    (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len)) == 0) {
            c->fd = fd;
            return 0;
        }
        (void)close(fd);
        if (attempt + 1 < max_attempts)
            (void)usleep(100 * 1000);
    }
    return -1;
}

/* 崩溃路径专用:不加锁、不更新状态,只发一次(看门线程会在 _exit 前用它送信号号)。 */
static int sxcl_chan_send_raw(int fd, const char *line)
{
    char buf[SXCL_CHAN_LINE + 2];
    int len = 0;

    if (fd < 0 || line == NULL)
        return -1;
    len = (int)snprintf(buf, sizeof(buf) - 1, "%s\n", line);
    if (len <= 0)
        return -1;
    if (len > (int)sizeof(buf) - 1)
        len = (int)sizeof(buf) - 1;
    if (send(fd, buf, (size_t)len, MSG_NOSIGNAL) < 0)
        return -1;
    return 0;
}

/* 正常路径:带锁 + MSG_NOSIGNAL(对端没了也不拿 SIGPIPE 打死游戏)。 */
static int sxcl_chan_send(sxcl_chan *c, const char *line)
{
    int rc = 0;

    if (c == NULL || line == NULL)
        return -1;
    (void)pthread_mutex_lock(&c->lock);
    if (c->fd < 0) {
        rc = -1;
    } else {
        rc = sxcl_chan_send_raw(c->fd, line);
        if (rc != 0) {
            __android_log_print(ANDROID_LOG_WARN, SXCL_GAME_TAG,
                                "channel send failed errno=%d -> 通道置灰(日志仍走文件)", errno);
            c->fd = -1;
        }
    }
    (void)pthread_mutex_unlock(&c->lock);
    return rc;
}

/* 读一行(阻塞)。1 = 拿到一行;0 = 对端关闭(EOF);-1 = 出错。 */
static int sxcl_chan_read_line(sxcl_chan *c, char *out, size_t cap)
{
    if (c == NULL || out == NULL || cap == 0)
        return -1;
    for (;;) {
        size_t i = 0;
        for (i = 0; i < c->used; ++i) {
            if (c->buf[i] == '\n') {
                const size_t n = i < cap - 1 ? i : cap - 1;
                (void)memcpy(out, c->buf, n);
                out[n] = '\0';
                (void)memmove(c->buf, c->buf + i + 1, c->used - i - 1);
                c->used -= i + 1;
                return 1;
            }
        }
        if (c->fd < 0)
            return 0;
        if (c->used >= sizeof(c->buf) - 1) {
            /* 没有换行的超长行:截断送出一行,不丢连接 */
            (void)memcpy(out, c->buf, cap - 1);
            out[cap - 1] = '\0';
            c->used = 0;
            return 1;
        }
        {
            const ssize_t got = read(c->fd, c->buf + c->used, sizeof(c->buf) - c->used - 1);
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                __android_log_print(ANDROID_LOG_WARN, SXCL_GAME_TAG, "channel read errno=%d",
                                    errno);
                return -1;
            }
            if (got == 0) {
                c->fd = -1;
                return 0;
            }
            c->used += (size_t)got;
            c->buf[c->used] = '\0';
        }
    }
}

/* ═════════ 游戏进程状态 ═════════ */

typedef struct {
    sxcl_chan chan;              /* 与主进程的本地 socket(传输层见 sxcl_chan.[ch]) */
    int exit_fd;                 /* <dir>/exit.txt:预打开,信号路径只 write+fsync */
    int log_fd;                  /* <dir>/game.log:预打开 */
    char session[128];
    char dir[SXCL_GAME_PATH];
    char jre[SXCL_GAME_PATH];
    char nativelib[SXCL_GAME_PATH];
    char classpath[2048];
    char gamedir[512];
    char mainclass[256];
    char jvmargs[1024];
    char gameargs[1024];
    char renderer[64];
    int launcher_pip;            /* spec.launcherPip:窗口就绪后是否允许请启动器进画中画 */
    int crash_mode;              /* 0=无 1=abort(SIGABRT) 2=SIGTERM 3=SIGKILL */
    pid_t pid;
    char proc[160];
    volatile int stop;           /* 收到结束请求 */
    int ended;                   /* 退出记录已经写过(只能写一次) */
    unsigned long log_lines;
    unsigned long long log_bytes;
    unsigned long pings;
} sxcl_game_ctx;

/* 游戏进程跟踪的致命信号:比探针多两个可捕获的(SIGTERM/SIGINT)——
 * adb shell kill <pid> / am 停进程都会走到这里,主进程因此能报出真信号号。
 * SIGSEGV 一律留给 ART/JVM。 */
static const int kGameSignals[] = {SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTERM, SIGINT};
static const int kGameSignalCount = (int)(sizeof(kGameSignals) / sizeof(kGameSignals[0]));

static sxcl_game_ctx g;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_exit_lock = PTHREAD_MUTEX_INITIALIZER;
static JavaVM *g_vm = NULL;
static jclass g_activity_cls = NULL;
static jmethodID g_mid_stopped = NULL;
static jmethodID g_mid_launcher_pip = NULL;

/* ── 底层写:一律裸 write/send(write 是 async-signal-safe 的,崩溃路径也用它) ── */

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* 发一行到主进程:传输层在 sxcl_chan.c("游戏运行器"边界的一部分,将来整体搬走)。
 * 对端没了只把通道置灰并返回 -1,绝不影响游戏自己的日志与运行。 */
static void chan_send(const char *line)
{
    (void)sxcl_chan_send(&g.chan, line);
}

/* 崩溃路径专用:不加锁(看门线程绝不能等一个可能永不释放的锁)。 */
static void chan_send_async(const char *line)
{
    (void)sxcl_chan_send_raw(sxcl_chan_is_open(&g.chan) ? g.chan.fd : -1, line);
}

/* 游戏自己的日志文件:每一行都落到文件里(主进程读不到通道时这就是唯一凭据) */
static void log_file_write(const char *line)
{
    char buf[SXCL_GAME_LINE + 2];
    int len;
    size_t n;

    if (line == NULL)
        return;
    len = (int)snprintf(buf, sizeof(buf) - 2, "%s\n", line);
    if (len <= 0)
        return;
    if (len > (int)sizeof(buf) - 2)
        len = (int)sizeof(buf) - 2;
    n = (size_t)len;
    (void)pthread_mutex_lock(&g_log_lock);
    if (g.log_fd >= 0) {
        (void)write_all(g.log_fd, buf, n);
        g.log_lines++;
        g.log_bytes += (unsigned long long)n;
    }
    (void)pthread_mutex_unlock(&g_log_lock);
}

/* ── 自举层的日志钩子:文件 + 通道 + logcat 三条出口 ── */
static void game_log_hook(void *ud, const char *line)
{
    (void)ud;
    log_file_write(line);
    if (sxcl_chan_is_open(&g.chan)) {
        char fwd[SXCL_GAME_LINE + 8];
        (void)snprintf(fwd, sizeof(fwd), "LOG %s", line);
        chan_send(fwd);
    }
    __android_log_print(ANDROID_LOG_INFO, SXCL_GAME_TAG, "%s", line);
}

/* ── 退出记录:只能写一次(code/signal/reason 的真实来源) ── */
static int game_exit_record(int code, int sig, const char *reason)
{
    char buf[1024];
    int len;
    int first = 0;

    (void)pthread_mutex_lock(&g_exit_lock);
    if (g.ended) {
        (void)pthread_mutex_unlock(&g_exit_lock);
        return 0;
    }
    g.ended = 1;
    (void)pthread_mutex_unlock(&g_exit_lock);
    first = 1;

    len = snprintf(buf, sizeof(buf),
                   "pid=%d\nproc=%s\nsession=%s\ncode=%d\nsignal=%d\nreason=%s\n"
                   "pings=%lu\nlog_lines=%lu\nlog_bytes=%llu\n",
                   (int)g.pid, g.proc, g.session, code, sig, reason != NULL ? reason : "",
                   g.pings, g.log_lines, g.log_bytes);
    if (len > 0 && g.exit_fd >= 0) {
        (void)lseek(g.exit_fd, 0, SEEK_SET);
        (void)write_all(g.exit_fd, buf, (size_t)len);
        (void)ftruncate(g.exit_fd, (off_t)len);
        (void)fsync(g.exit_fd);
    }
    {
        char line[512];
        (void)snprintf(line, sizeof(line), "EXIT code=%d signal=%d reason=%s", code, sig,
                       reason != NULL ? reason : "");
        chan_send(line);
    }
    return first;
}

/* ── 崩溃兜底钩子(看门线程里跑,不是信号处理器里) ── */
static void game_fatal_hook(void *ud, int sig)
{
    char line[256];
    (void)ud;
    (void)snprintf(line, sizeof(line), "SIG signal=%d code=%d", sig, 128 + sig);
    /* 通道与退出记录文件两条路都写:对端读到哪条都能报出真信号 */
    chan_send_async(line);
    log_file_write(line);
    if (g.exit_fd >= 0) {
        (void)lseek(g.exit_fd, 0, SEEK_SET);
        (void)write_all(g.exit_fd, line, strlen(line));
        (void)write_all(g.exit_fd, "\n", 1);
        (void)fsync(g.exit_fd);
    }
}

/* ── 通知 Java 侧收尾(游戏进程该结束了) ── */
static void call_java_stopped(int code, int sig, const char *reason)
{
    JNIEnv *env = NULL;
    int attached = 0;
    jstring jreason = NULL;

    if (g_vm == NULL || g_activity_cls == NULL || g_mid_stopped == NULL) {
        __android_log_print(ANDROID_LOG_WARN, SXCL_GAME_TAG,
                            "没有 Java 侧可通知(g_vm=%p),只做原生收尾", (void *)g_vm);
        return;
    }
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK)
            return;
        attached = 1;
    }
    jreason = (*env)->NewStringUTF(env, reason != NULL ? reason : "");
    (*env)->CallStaticVoidMethod(env, g_activity_cls, g_mid_stopped, (jint)code, (jint)sig, jreason);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (jreason != NULL)
        (*env)->DeleteLocalRef(env, jreason);
    if (attached)
        (*g_vm)->DetachCurrentThread(g_vm);
}

/* ── 请启动器回前台进画中画(由主进程的 PIP 回答触发) ── */
static void call_java_launcher_pip(void)
{
    JNIEnv *env = NULL;
    int attached = 0;

    if (g_vm == NULL || g_activity_cls == NULL || g_mid_launcher_pip == NULL) {
        __android_log_print(ANDROID_LOG_WARN, SXCL_GAME_TAG,
                            "没有 Java 侧可请求画中画(g_vm=%p),跳过", (void *)g_vm);
        return;
    }
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK)
            return;
        attached = 1;
    }
    (*env)->CallStaticVoidMethod(env, g_activity_cls, g_mid_launcher_pip);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (attached)
        (*g_vm)->DetachCurrentThread(g_vm);
}

/* ── 有序收尾(主进程的 END、或本进程自己要求停) ── */
static void game_flush_log(void)
{
    (void)pthread_mutex_lock(&g_log_lock);
    if (g.log_fd >= 0) {
        char line[256];
        (void)fsync(g.log_fd);
        (void)snprintf(line, sizeof(line),
                       "[sxcl-game] flush ok lines=%lu bytes=%llu log=%s/game.log", g.log_lines,
                       g.log_bytes, g.dir);
        (void)write_all(g.log_fd, line, strlen(line));
        (void)write_all(g.log_fd, "\n", 1);
        (void)fsync(g.log_fd);
        g.log_lines++;
    }
    (void)pthread_mutex_unlock(&g_log_lock);
}

static void game_request_stop(const char *why)
{
    if (g.stop)
        return;
    g.stop = 1;
    sxcl_jre_logf("收到结束请求(%s):开始有序收尾(先 JVM,再 flush 日志,最后写退出记录)",
                  why != NULL ? why : "");
    /* ① JVM:有 JVM 才需要(本轮真机没有 JRE -> 返回 0,直接走 flush 路径) */
    if (sxcl_jre_request_shutdown() == 0)
        sxcl_jre_logf("JVM 收尾:本进程没有活着的 JVM(或拿不到 JVM 句柄),按 flush 路径收尾");
    /* ② flush 日志(有序收尾的最低要求:一行都不能丢) */
    game_flush_log();
    /* ③ 退出记录:通道 + 文件各一份,并通知 Java 侧真正把进程结束掉 */
    if (game_exit_record(0, 0, why != NULL ? why : "command")) {
        chan_send("BYE");
        call_java_stopped(0, 0, why != NULL ? why : "command");
    }
}

/* ── stdout/stderr -> 文件 + 通道 + logcat ── */
static int g_pipe[2] = {-1, -1};

static void *stdio_pump(void *arg)
{
    char buf[SXCL_GAME_LINE];
    size_t used = 0;
    (void)arg;

    for (;;) {
        ssize_t got = 0;
        if (used >= sizeof(buf) - 1u) {
            buf[used] = '\0';
            log_file_write(buf);
            chan_send(buf);
            used = 0;
        }
        got = read(g_pipe[0], buf + used, sizeof(buf) - used - 1u);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (got == 0)
            break;
        used += (size_t)got;
        buf[used] = '\0';
        {
            size_t start = 0;
            size_t i = 0;
            for (i = 0; i < used; ++i) {
                if (buf[i] == '\n') {
                    buf[i] = '\0';
                    if (i > start) {
                        log_file_write(buf + start);
                        chan_send(buf + start);
                        __android_log_print(ANDROID_LOG_INFO, SXCL_GAME_TAG, "%s", buf + start);
                    }
                    start = i + 1;
                }
            }
            if (start > 0) {
                (void)memmove(buf, buf + start, used - start);
                used -= start;
            }
        }
    }
    return NULL;
}

static void stdio_redirect(void)
{
    pthread_t tid;
    if (pipe(g_pipe) != 0)
        return;
    if (dup2(g_pipe[1], STDOUT_FILENO) < 0 || dup2(g_pipe[1], STDERR_FILENO) < 0)
        return;
    (void)close(g_pipe[1]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pthread_create(&tid, NULL, stdio_pump, NULL) == 0)
        (void)pthread_detach(tid);
}

/* ── 主进程 -> 游戏进程的命令读取 ──
 * 命令表刻意只有三条:END(有序结束)、PING?(探活)、其它一律记一行。
 * 将来拆两个 APK 时,这张表就是 A/B 之间控制面的全部约定。 */
static void *chan_reader(void *arg)
{
    char line[SXCL_GAME_LINE];
    (void)arg;

    for (;;) {
        const int rc = sxcl_chan_read_line(&g.chan, line, sizeof(line));
        if (rc == 0) {
            __android_log_print(ANDROID_LOG_INFO, SXCL_GAME_TAG,
                                "channel closed by host(EOF):游戏继续跑,通道置灰");
            break;
        }
        if (rc < 0)
            break;
        if (line[0] == '\0')
            continue;
        if (strcmp(line, "END") == 0) {
            game_request_stop("command");
        } else if (strcmp(line, "PING?") == 0) {
            chan_send("PONG");
        } else if (strncmp(line, "PIP ", 4) == 0) {
            /* 主进程对 "PIP?" 的回答:need=1 才值得把启动器拉回前台(它自己知道在不在画中画里)。
             * 这样"已经在画中画里"的情况下不会再把启动器拉出来打断游戏窗口。 */
            const int need = atoi(line + 4 + 5); /* "PIP need=1" -> 下标 9 */
            sxcl_jre_logf("启动器画中画握手:主进程回答 need=%d", need);
            if (need != 0)
                call_java_launcher_pip();
        } else {
            __android_log_print(ANDROID_LOG_INFO, SXCL_GAME_TAG, "unknown command: %s", line);
        }
    }
    return NULL;
}

/* ── 心跳:让主进程能区分"游戏还活着"与"通道只是没话" ── */
static void *ping_thread(void *arg)
{
    (void)arg;
    while (!g.stop) {
        int i = 0;
        for (i = 0; i < 50 && !g.stop; ++i)
            (void)usleep(100 * 1000); /* 5s 一次 */
        if (g.stop)
            break;
        g.pings++;
        {
            char line[128];
            (void)snprintf(line, sizeof(line), "PING pid=%d n=%lu", (int)g.pid, g.pings);
            chan_send(line);
        }
    }
    return NULL;
}

/* ── 崩溃注入(验收用:真机上看崩溃兜底把信号号传回主进程) ── */
static void *crash_thread(void *arg)
{
    (void)arg;
    (void)usleep(6 * 1000 * 1000); /* 等主进程把会话读起来 */
    if (g.crash_mode == 1) {
        sxcl_jre_logf("崩溃注入:abort() -> SIGABRT(可捕获,应当传回 signal=6 code=134)");
        (void)fprintf(stderr, "[sxcl-game] crash injection: abort()\n");
        (void)fflush(stderr);
        abort();
    } else if (g.crash_mode == 2) {
        sxcl_jre_logf("崩溃注入:raise(SIGTERM) -> signal=15 code=143(可捕获)");
        (void)raise(SIGTERM);
    } else if (g.crash_mode == 3) {
        sxcl_jre_logf("崩溃注入:kill(getpid(), SIGKILL) -> 不可捕获(对端只能看到通道断开)");
        (void)kill(getpid(), SIGKILL);
    }
    return NULL;
}

/* ── 游戏主体:有 JRE 就起 JVM,没有就如实报告并待命(本轮真机就是这条) ── */
static void *game_body(void *arg)
{
    (void)arg;
    if (g.jre[0] != '\0') {
        static char *jvm_argv[SXCL_GAME_ARG_MAX];
        static char *app_argv[SXCL_GAME_ARG_MAX];
        static char jvm_buf[sizeof(g.jvmargs) + 16];
        static char app_buf[sizeof(g.gameargs) + 16];
        static char gamedir_opt[sizeof(g.gamedir) + 16];
        sxcl_jre_launch_opts opts;
        int jvm_argc = 0;
        int app_argc = 0;
        int rc = 0;
        int i = 0;

        (void)memset(&opts, 0, sizeof(opts));
        (void)snprintf(jvm_buf, sizeof(jvm_buf), "%s", g.jvmargs);
        (void)snprintf(app_buf, sizeof(app_buf), "%s", g.gameargs);
        /* JVM 参数与游戏参数都是空格分隔(与 boot 文件同一口径,不做引号解析) */
        {
            char *save = NULL;
            char *tok = strtok_r(jvm_buf, " \t", &save);
            while (tok != NULL && jvm_argc < SXCL_GAME_ARG_MAX - 8) {
                jvm_argv[jvm_argc++] = tok;
                tok = strtok_r(NULL, " \t", &save);
            }
            save = NULL;
            tok = strtok_r(app_buf, " \t", &save);
            while (tok != NULL && app_argc < SXCL_GAME_ARG_MAX) {
                app_argv[app_argc++] = tok;
                tok = strtok_r(NULL, " \t", &save);
            }
        }
        /* 边界输入里,classpath / natives / -Djava.home / -Dos.version 由**核心库**
         * (sxcl/jvm.h)统一拼 —— 这里只补游戏特有的那一条:-Duser.dir=<游戏目录>,
         * 让游戏把它当工作目录(存档/资源都挂在它下面)。 */
        if (g.gamedir[0] != '\0' && jvm_argc < SXCL_GAME_ARG_MAX - 8) {
            (void)snprintf(gamedir_opt, sizeof(gamedir_opt), "-Duser.dir=%s", g.gamedir);
            jvm_argv[jvm_argc++] = gamedir_opt;
        }
        (void)i;
        opts.java_home = g.jre;
        opts.native_lib_dir = g.nativelib;
        opts.class_path = g.classpath;
        opts.android_version = NULL; /* 由适配层读 ro.build.version.release 填上 */
        opts.report_path = NULL;     /* 报告就在通道/日志里,不再另写一份 */
        opts.log_prefix = "[sxcl-game]";
        opts.args = (const char *const *)jvm_argv;
        opts.arg_count = jvm_argc;
        opts.app_args = (const char *const *)app_argv;
        opts.app_arg_count = app_argc;
        opts.main_class = g.mainclass[0] != '\0' ? g.mainclass : NULL;
        opts.fatal_signals = kGameSignals;
        opts.fatal_signal_count = kGameSignalCount;
        sxcl_jre_logf("游戏进程开始起 JVM:jre=%s main=%s jvm-args=%d app-args=%d classpath=%s "
                      "natives=%s gamedir=%s renderer=%s",
                      g.jre, opts.main_class != NULL ? opts.main_class : "(未指定,跑探针参数)",
                      jvm_argc, app_argc, g.classpath[0] != '\0' ? "(给了)" : "(空)",
                      g.nativelib[0] != '\0' ? "(给了)" : "(空)",
                      g.gamedir[0] != '\0' ? g.gamedir : "(空)",
                      g.renderer[0] != '\0' ? g.renderer : "(空,渲染层本轮未接)");
        rc = sxcl_jre_launch(&opts);
        sxcl_jre_logf("JLI_Launch 返回 rc=%d", rc);
        if (game_exit_record(rc, 0, "jvm-returned")) {
            chan_send("BYE");
            call_java_stopped(rc, 0, "jvm-returned");
        }
        return NULL;
    }

    /* 没有 JRE:架构照样跑通(两个进程 + 通道 + 收尾),但绝不说"游戏起来了" */
    sxcl_jre_logf("本进程没有配 JRE(本轮只验架构):java=absent 待命中,通道与收尾照常工作");
    chan_send("STATE java=absent reason=no-jre-configured");
    while (!g.stop)
        (void)usleep(200 * 1000);
    return NULL;
}

/* ── JNI 入口(GameActivity 调) ── */

static void jstr_dup(JNIEnv *env, jstring s, char *dst, size_t dst_len)
{
    const char *utf = NULL;
    dst[0] = '\0';
    if (s == NULL)
        return;
    utf = (*env)->GetStringUTFChars(env, s, NULL);
    if (utf == NULL)
        return;
    (void)snprintf(dst, dst_len, "%s", utf);
    (*env)->ReleaseStringUTFChars(env, s, utf);
}

JNIEXPORT jint JNICALL Java_com_silentstudio_sxcl_GameActivity_nativeStart(
    JNIEnv *env, jclass cls, jstring jdir, jstring jsocket, jstring jjre, jstring jnativelib,
    jstring jclasspath, jstring jgamedir, jstring jmainclass, jstring jjvmargs, jstring jgameargs,
    jstring jrenderer, jint crash_mode)
{
    pthread_t tid;
    char socket_name[160];
    char path[SXCL_GAME_PATH + 32];
    char cmdline[256];
    FILE *f = NULL;

    (void)cls;
    (void)memset(&g, 0, sizeof(g));
    g.chan.fd = -1;
    g.exit_fd = -1;
    g.log_fd = -1;
    g.crash_mode = (int)crash_mode;
    g.pid = getpid();

    jstr_dup(env, jdir, g.dir, sizeof(g.dir));
    jstr_dup(env, jsocket, socket_name, sizeof(socket_name));
    jstr_dup(env, jjre, g.jre, sizeof(g.jre));
    jstr_dup(env, jnativelib, g.nativelib, sizeof(g.nativelib));
    jstr_dup(env, jclasspath, g.classpath, sizeof(g.classpath));
    jstr_dup(env, jgamedir, g.gamedir, sizeof(g.gamedir));
    jstr_dup(env, jmainclass, g.mainclass, sizeof(g.mainclass));
    jstr_dup(env, jjvmargs, g.jvmargs, sizeof(g.jvmargs));
    jstr_dup(env, jgameargs, g.gameargs, sizeof(g.gameargs));
    jstr_dup(env, jrenderer, g.renderer, sizeof(g.renderer));
    g.launcher_pip = 1; /* spec 里 launcherPip 的默认值;需要在 Intent 里关掉时见 GameActivity */
    (void)snprintf(g.session, sizeof(g.session), "%s", "unknown");

    /* 进程名(/proc/self/cmdline)就是 "com.silentstudio.sxcl:game",写进 HELLO 当证据 */
    g.proc[0] = '\0';
    f = fopen("/proc/self/cmdline", "rb");
    if (f != NULL) {
        const size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
        cmdline[n] = '\0';
        (void)snprintf(g.proc, sizeof(g.proc), "%s", cmdline);
        (void)fclose(f);
    }
    if (g.proc[0] == '\0')
        (void)snprintf(g.proc, sizeof(g.proc), "pid=%d", (int)g.pid);

    /* 会话号:host 侧约定 socket 名是 sxcl-game-<hostpid>-<session>,取最后一段只为日志好认 */
    {
        const char *dash = strrchr(socket_name, '-');
        if (dash != NULL && dash[1] != '\0')
            (void)snprintf(g.session, sizeof(g.session), "%s", dash + 1);
    }

    /* 1) SIGPIPE 忽略:对端(主进程/日志泵)没了不能把游戏带走 */
    (void)signal(SIGPIPE, SIG_IGN);

    /* 2) 退出记录文件与日志文件先打开(崩溃路径只 write+fsync,不再 open) */
    if (g.dir[0] != '\0') {
        (void)snprintf(path, sizeof(path), "%s/exit.txt", g.dir);
        g.exit_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        (void)snprintf(path, sizeof(path), "%s/game.log", g.dir);
        g.log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }

    /* 3) 通道先接上,再动别的 —— 主进程那边早就在监听(2s 内轮询重试) */
    if (sxcl_chan_open(&g.chan, socket_name, 2000) != 0) {
        __android_log_print(ANDROID_LOG_WARN, SXCL_GAME_TAG,
                            "channel connect failed(name=%s):降级为文件+logcat", socket_name);
    }

    /* 4) 自举层的日志与崩溃钩子都接到本进程的三个出口上。
     *    崩溃兜底在这里就装上(clear_all=0:ART 自己的 SIGSEGV/SIGQUIT 原样不动)——
     *    这样即使本进程根本没配 JRE,abort()/SIGTERM 也能把信号号写回主进程;真要起 JVM 时
     *    sxcl_jre_launch() 会再装一次(带 clear_all=1),把处理器清干净后重装。 */
    sxcl_jre_set_log_hook(game_log_hook, NULL);
    sxcl_jre_set_fatal_hook(game_fatal_hook, NULL);
    sxcl_jre_set_log_prefix("[sxcl-game]");
    sxcl_jre_install_guard(kGameSignals, kGameSignalCount, 0);

    /* 5) stdout/stderr -> 文件 + 通道 + logcat(JVM 的输出靠这条进日志) */
    stdio_redirect();

    game_log_hook(NULL, "=== :game 进程原生层启动 ===");
    {
        char line[640];
        (void)snprintf(line, sizeof(line),
                       "HELLO pid=%d proc=%s session=%s chan=%d java=%s", (int)g.pid, g.proc,
                       g.session, sxcl_chan_is_open(&g.chan) ? 1 : 0,
                       g.jre[0] != '\0' ? "present" : "absent");
        chan_send(line);
        (void)snprintf(line, sizeof(line),
                       "READY dir=%s nativelib=%s mainclass=%s classpath=%s gamedir=%s renderer=%s",
                       g.dir, g.nativelib, g.mainclass[0] != '\0' ? g.mainclass : "(none)",
                       g.classpath[0] != '\0' ? "(given)" : "(empty)",
                       g.gamedir[0] != '\0' ? g.gamedir : "(empty)",
                       g.renderer[0] != '\0' ? g.renderer : "(empty)");
        chan_send(line);
        /* 画中画握手:主进程知道它自己在不在画中画里(静态字段是各进程各一份,本进程不知道),
         * 所以由本进程问、主进程答;只有 need=1 才把启动器拉回前台 ——
         * 否则"已经在画中画里"还会被再拉出来一次,把游戏窗口打断。 */
        if (g.launcher_pip)
            chan_send("PIP?");
    }

    /* 6) 命令读取线程 + 心跳 + 崩溃注入 */
    if (sxcl_chan_is_open(&g.chan)) {
        if (pthread_create(&tid, NULL, chan_reader, NULL) == 0)
            (void)pthread_detach(tid);
    }
    if (pthread_create(&tid, NULL, ping_thread, NULL) == 0)
        (void)pthread_detach(tid);
    if (g.crash_mode != 0) {
        if (pthread_create(&tid, NULL, crash_thread, NULL) == 0)
            (void)pthread_detach(tid);
    }

    /* 7) 游戏主体(没有 JRE 时它会一直待命到 stop;有 JRE 时 JLI_Launch 的生命周期
     *    就是游戏的生命周期 —— 这个 JNI 调用会一直阻塞,Java 侧是拿独立线程调它的) */
    game_body(NULL);

    /* 8) 主体退出:确保退出记录一定写了(重复调用是空操作) */
    if (game_exit_record(0, 0, "body-returned"))
        call_java_stopped(0, 0, "body-returned");
    return 0;
}

JNIEXPORT jint JNICALL Java_com_silentstudio_sxcl_GameActivity_nativeRequestStop(JNIEnv *env,
                                                                                 jclass cls)
{
    (void)env;
    (void)cls;
    game_request_stop("local");
    return 0;
}

JNIEXPORT jint JNICALL Java_com_silentstudio_sxcl_GameActivity_nativeHasJvm(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return sxcl_jre_was_started();
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    JNIEnv *env = NULL;
    jclass cls = NULL;
    (void)reserved;

    g_vm = vm;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    cls = (*env)->FindClass(env, "com/silentstudio/sxcl/GameActivity");
    if (cls == NULL) {
        (*env)->ExceptionClear(env);
        __android_log_print(ANDROID_LOG_WARN, SXCL_GAME_TAG, "JNI_OnLoad:找不到 GameActivity");
        return JNI_VERSION_1_6;
    }
    g_activity_cls = (jclass)(*env)->NewGlobalRef(env, cls);
    g_mid_stopped = (*env)->GetStaticMethodID(env, cls, "onGameStopped", "(IILjava/lang/String;)V");
    if (g_mid_stopped == NULL) {
        (*env)->ExceptionClear(env);
        __android_log_print(ANDROID_LOG_WARN, SXCL_GAME_TAG, "JNI_OnLoad:找不到 onGameStopped");
    }
    g_mid_launcher_pip = (*env)->GetStaticMethodID(env, cls, "onLauncherPipNeeded", "()V");
    if (g_mid_launcher_pip == NULL) {
        (*env)->ExceptionClear(env);
        __android_log_print(ANDROID_LOG_WARN, SXCL_GAME_TAG,
                            "JNI_OnLoad:找不到 onLauncherPipNeeded");
    }
    return JNI_VERSION_1_6;
}
