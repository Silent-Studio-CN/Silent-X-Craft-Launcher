/* 子进程(Windows 实现):CreateProcess + 两条匿名管道 + 轮询读取。
 * 不用 Qt/不用 CRT 的 popen,因为要同时收两条流、要逐行回调、要能超时终止。 */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "sxcl/process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wchar_t *to_wide(const char *utf8)
{
    if (!utf8) {
        return NULL;
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (need <= 0) {
        return NULL;
    }
    wchar_t *w = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, need) != need) {
        free(w);
        w = NULL;
    }
    return w;
}

/* Windows 命令行参数转义(规则来自 CreateProcess 文档):按需加引号,并转义反斜杠+引号 */
static void append_arg(char **buf, size_t *cap, size_t *len, const char *arg)
{
    const size_t need = strlen(arg) * 2 + 8;
    if (*len + need > *cap) {
        while (*len + need > *cap) {
            *cap *= 2;
        }
        *buf = (char *)realloc(*buf, *cap);
    }
    const int quote = (strchr(arg, ' ') || strchr(arg, '\t') || *arg == '\0');
    size_t backslashes = 0;
    if (quote) {
        (*buf)[(*len)++] = '"';
    }
    for (const char *p = arg; *p; ++p) {
        if (*p == '\\') {
            ++backslashes;
            (*buf)[(*len)++] = *p;
            continue;
        }
        if (*p == '"') {
            for (size_t i = 0; i <= backslashes; ++i) {
                (*buf)[(*len)++] = '\\';
            }
            (*buf)[(*len)++] = '\\';
            (*buf)[(*len)++] = '"';
            backslashes = 0;
            continue;
        }
        backslashes = 0;
        (*buf)[(*len)++] = *p;
    }
    if (quote) {
        for (size_t i = 0; i < backslashes; ++i) {
            (*buf)[(*len)++] = '\\';
        }
        (*buf)[(*len)++] = '"';
    }
    (*buf)[(*len)] = '\0';
}

/* 把管道里已经到达的数据按行回调。返回 1 = 回调请求终止 */
static int drain(HANDLE pipe, int is_stderr, char *pending, size_t *pending_len,
                 const sxcl_process_opts *opts)
{
    char chunk[4096];
    DWORD avail = 0;
    while (PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
        DWORD got = 0;
        if (!ReadFile(pipe, chunk, (DWORD)sizeof(chunk), &got, NULL) || got == 0) {
            break;
        }
        for (DWORD i = 0; i < got; ++i) {
            const char c = chunk[i];
            if (c == '\n' || *pending_len >= 4000) {
                pending[*pending_len] = '\0';
                size_t n = *pending_len;
                while (n > 0 && (pending[n - 1] == '\r')) {
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
    return 0;
}

int sxcl_process_run(const sxcl_process_opts *opts, sxcl_process_result *out)
{
    if (!opts || !opts->program || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;

    /* 命令行 = program + 转义后的参数 */
    size_t cap = 512, len = 0;
    char *cmdline = (char *)malloc(cap);
    if (!cmdline) {
        return -1;
    }
    cmdline[0] = '\0';
    append_arg(&cmdline, &cap, &len, opts->program);
    for (const char *const *a = opts->args; a && *a; ++a) {
        cmdline[len++] = ' ';
        cmdline[len] = '\0';
        append_arg(&cmdline, &cap, &len, *a);
    }

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
        snprintf(out->error, sizeof(out->error), "创建管道失败");
        free(cmdline);
        return -1;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    /* 环境块:父环境 + 追加项 */
    char *envblock = NULL;
    if (opts->env && opts->env[0]) {
        /* 简单做法:不改父环境块,改用 "set KEY=VAL && cmd" 是 shell 思路,这里不取;
         * 需要自定义环境时由调用方传完整列表(见 README 的进程接口说明)。 */
    }
    (void)envblock;

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = out_w;
    si.hStdError = err_w;

    wchar_t *wcmd = to_wide(cmdline);
    wchar_t *wdir = to_wide(opts->work_dir);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    const BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                   NULL, wdir, &si, &pi);
    free(wcmd);
    free(wdir);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        snprintf(out->error, sizeof(out->error), "无法启动进程(错误码 %lu): %s", GetLastError(),
                 opts->program);
        CloseHandle(out_r);
        CloseHandle(err_r);
        free(cmdline);
        return -1;
    }
    CloseHandle(pi.hThread);

    const DWORD t0 = GetTickCount();
    char pending[4096];
    size_t pending_len = 0;
    char epending[4096];
    size_t epending_len = 0;
    int killed = 0, timed_out = 0;

    for (;;) {
        if (drain(out_r, 0, pending, &pending_len, opts) || drain(err_r, 1, epending, &epending_len, opts)) {
            killed = 1;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            break;
        }
        if (opts->timeout_ms > 0 && (int)(GetTickCount() - t0) > opts->timeout_ms) {
            timed_out = 1;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    /* 进程结束后把管道里剩下的读完 */
    for (int i = 0; i < 50; ++i) {
        if (!drain(out_r, 0, pending, &pending_len, opts) && !drain(err_r, 1, epending, &epending_len, opts)) {
            DWORD avail = 0;
            if (!PeekNamedPipe(out_r, NULL, 0, NULL, &avail, NULL) || avail == 0) {
                break;
            }
        }
    }
    if (pending_len > 0 && opts->on_line) {
        pending[pending_len] = '\0';
        opts->on_line(opts->userdata, 0, pending);
    }
    if (epending_len > 0 && opts->on_line) {
        epending[epending_len] = '\0';
        opts->on_line(opts->userdata, 1, epending);
    }

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    out->exit_code = (int)code;
    out->timed_out = timed_out;
    out->killed_by_client = killed;
    out->elapsed_ms = (int64_t)(GetTickCount() - t0);
    CloseHandle(pi.hProcess);
    CloseHandle(out_r);
    CloseHandle(err_r);
    free(cmdline);
    return 0;
}
