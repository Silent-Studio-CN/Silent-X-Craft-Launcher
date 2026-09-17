/* 子进程(POSIX 实现):fork/execvp + 两条管道 + poll 轮询。
 * 本机(Windows)不编译这条分支,由四平台 CI 验证。 */
#include "sxcl/process.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 把 fd 里可读的数据按行回调。返回 1 = 回调请求终止 */
static int drain_fd(int fd, int is_stderr, char *pending, size_t *pending_len,
                    const sxcl_process_opts *opts, int *eof)
{
    char chunk[4096];
    for (;;) {
        const ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            *eof = 1;
            return 0;
        }
        if (got == 0) {
            *eof = 1;
            return 0;
        }
        for (ssize_t i = 0; i < got; ++i) {
            const char c = chunk[i];
            if (c == '\n' || *pending_len >= 4000) {
                pending[*pending_len] = '\0';
                size_t n = *pending_len;
                while (n > 0 && pending[n - 1] == '\r') {
                    pending[--n] = '\0';
                }
                *pending_len = 0;
                if (opts->on_line && opts->on_line(opts->userdata, is_stderr, pending) != 0) {
                    return 1;
                }
            } else {
                pending[(*pending_len)++] = c;
            }
        }
    }
}

int sxcl_process_run(const sxcl_process_opts *opts, sxcl_process_result *out)
{
    if (!opts || !opts->program || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;

    int op[2] = {-1, -1};
    int ep[2] = {-1, -1};
    if (pipe(op) != 0 || pipe(ep) != 0) {
        snprintf(out->error, sizeof(out->error), "创建管道失败");
        return -1;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        snprintf(out->error, sizeof(out->error), "fork 失败: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* 子进程 */
        if (dup2(op[1], STDOUT_FILENO) < 0 || dup2(ep[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(op[0]);
        close(op[1]);
        close(ep[0]);
        close(ep[1]);
        if (opts->work_dir && chdir(opts->work_dir) != 0) {
            _exit(127);
        }
        if (opts->env) {
            for (const char *const *e = opts->env; *e; ++e) {
                putenv(strdup(*e));
            }
        }
        size_t n = 1;
        for (const char *const *a = opts->args; a && *a; ++a) {
            ++n;
        }
        char **argv = (char **)malloc((n + 1) * sizeof(char *));
        if (!argv) {
            _exit(127);
        }
        size_t i = 0;
        argv[i++] = (char *)opts->program;
        for (const char *const *a = opts->args; a && *a; ++a) {
            argv[i++] = (char *)*a;
        }
        argv[i] = NULL;
        execvp(opts->program, argv);
        _exit(127);
    }

    close(op[1]);
    close(ep[1]);
    fcntl(op[0], F_SETFL, O_NONBLOCK);
    fcntl(ep[0], F_SETFL, O_NONBLOCK);

    const int64_t t0 = now_ms();
    char pending[4096];
    size_t pending_len = 0;
    char epending[4096];
    size_t epending_len = 0;
    int killed = 0, timed_out = 0, out_eof = 0, err_eof = 0;

    for (;;) {
        struct pollfd fds[2];
        fds[0].fd = op[0];
        fds[0].events = POLLIN;
        fds[1].fd = ep[0];
        fds[1].events = POLLIN;
        poll(fds, 2, 20);

        if (drain_fd(op[0], 0, pending, &pending_len, opts, &out_eof) ||
            drain_fd(ep[0], 1, epending, &epending_len, opts, &err_eof)) {
            killed = 1;
            kill(pid, SIGKILL);
            break;
        }
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            /* 进程已结束:把剩余数据读完 */
            drain_fd(op[0], 0, pending, &pending_len, opts, &out_eof);
            drain_fd(ep[0], 1, epending, &epending_len, opts, &err_eof);
            if (pending_len > 0 && opts->on_line) {
                pending[pending_len] = '\0';
                opts->on_line(opts->userdata, 0, pending);
            }
            if (epending_len > 0 && opts->on_line) {
                epending[epending_len] = '\0';
                opts->on_line(opts->userdata, 1, epending);
            }
            out->exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                               : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
            out->timed_out = timed_out;
            out->killed_by_client = killed;
            out->elapsed_ms = now_ms() - t0;
            close(op[0]);
            close(ep[0]);
            return 0;
        }
        if (opts->timeout_ms > 0 && now_ms() - t0 > opts->timeout_ms) {
            timed_out = 1;
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            break;
        }
    }

    int status = 0;
    waitpid(pid, &status, 0);
    out->exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                       : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    out->timed_out = timed_out;
    out->killed_by_client = killed;
    out->elapsed_ms = now_ms() - t0;
    close(op[0]);
    close(ep[0]);
    return 0;
}
