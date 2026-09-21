/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* jvm.c - 进程内起 JVM(dlopen libjli.so + JLI_Launch)。见 include/sxcl/jvm.h 的完整说明。
 *
 * 本文件的代码是**我们自己写的**:只借用了"dlopen + JLI_Launch"这条公开做法
 * (Boardwalk / PojavLauncher 一系,FCL 走的也是同一条)与 OpenJDK launcher 里
 * JLI_Launch 的**公开函数签名**,没有从 FCL/Pojav 抄任何源码文本。
 *
 * 组织:
 *   1) sxcl_jvm_build_env   纯函数:算 JAVA_HOME / LD_LIBRARY_PATH / TMPDIR / HOME
 *   2) sxcl_jvm_build_args  纯函数:拼 argv(进程内 dlopen 必须自己给 -Djava.home)
 *   3) preload_libs         预加载 <home>/lib 与 <home>/lib/server 里的关键 .so
 *   4) capture              把 stdout/stderr 接到文件(自检留证)
 *   5) launch / probe / selfcheck
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/jvm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <fcntl.h>
#  include <io.h>
#  include <sys/stat.h>   /* _S_IREAD / _S_IWRITE */
#  include <windows.h>
#  define SXCL_JLI_CALL __stdcall
#  define SXCL_DLOPEN(p) ((void *)LoadLibraryA(p))
#  define SXCL_DLSYM(h, n) ((void *)GetProcAddress((HMODULE)(h), (n)))
#  define SXCL_JLI_NAME "java.dll"
#else
#  include <dlfcn.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define SXCL_JLI_CALL
#  define SXCL_DLOPEN(p) dlopen((p), RTLD_NOW | RTLD_GLOBAL)
#  define SXCL_DLSYM(h, n) dlsym((h), (n))
#  if defined(__APPLE__)
#    define SXCL_JLI_NAME "libjli.dylib"
#  else
#    define SXCL_JLI_NAME "libjli.so"
#  endif
#endif

/* JNI 的 jboolean/jint:为了不把 jni.h 拉进核心库,jni.h 里的定义照抄类型本身
 * (jboolean = unsigned char,jint = int),Windows 上调用约定是 __stdcall(JNICALL)。 */
typedef unsigned char sxcl_jboolean;
typedef int sxcl_jint;

typedef sxcl_jint(SXCL_JLI_CALL *sxcl_jli_launch_fn)(int argc, char **argv, int jargc,
                                                     const char **jargv, int appclassc,
                                                     const char **appclassv,
                                                     const char *fullversion,
                                                     const char *dotversion, const char *pname,
                                                     const char *lname, sxcl_jboolean javaargs,
                                                     sxcl_jboolean cpwildcard,
                                                     sxcl_jboolean javaw, sxcl_jint ergo);

/* ── 小工具 ── */

static void jvm_err(char *err, size_t err_len, const char *fmt, ...)
{
    char buf[SXCL_JVM_ERROR_MAX];
    va_list ap;
    if (err == NULL || err_len == 0) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    snprintf(err, err_len, "%s", buf);
    err[err_len - 1] = '\0';
}

static void copy_str(char *out, size_t out_len, const char *src)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "%s", src != NULL ? src : "");
}

static int join_path(char *out, size_t out_len, const char *dir, const char *leaf)
{
    const size_t dl = strlen(dir);
    const size_t ll = strlen(leaf);
    const int need_sep = (dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\') ? 1 : 0;
    if (dl + (size_t)need_sep + ll + 1 > out_len) {
        return -1;
    }
    memcpy(out, dir, dl);
    if (need_sep) {
        out[dl] = '/';
    }
    memcpy(out + dl + (size_t)need_sep, leaf, ll + 1);
    return 0;
}

/** 追加一段到 path 缓冲里(用平台分隔符);装不下返回 -1。 */
static int append_path(char *buf, size_t buf_len, const char *item)
{
    const size_t used = strlen(buf);
    const size_t need = strlen(item);
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    if (used > 0 && buf[used - 1] == sep) {
        if (used + need + 1 > buf_len) {
            return -1;
        }
        memcpy(buf + used, item, need + 1);
        return 0;
    }
    if (used + 1 + need + 1 > buf_len) {
        return -1;
    }
    buf[used] = sep;
    memcpy(buf + used + 1, item, need + 1);
    return 0;
}

const char *sxcl_jvm_code_name(int code)
{
    switch (code) {
    case SXCL_JVM_OK:
        return "ok";
    case SXCL_JVM_ERR_ARG:
        return "arg";
    case SXCL_JVM_ERR_NO_HOME:
        return "no_home";
    case SXCL_JVM_ERR_NO_JLI:
        return "no_jli";
    case SXCL_JVM_ERR_DLOPEN:
        return "dlopen";
    case SXCL_JVM_ERR_DLSYM:
        return "dlsym";
    case SXCL_JVM_ERR_LAUNCH:
        return "launch";
    case SXCL_JVM_ERR_UNSUPPORTED:
        return "unsupported";
    case SXCL_JVM_ERR_NOMEM:
        return "nomem";
    case SXCL_JVM_ERR_IO:
        return "io";
    case SXCL_JVM_ERR_MISSING:
        return "missing";
    default:
        return "?";
    }
}

/* ── 1) 环境变量 ── */

int sxcl_jvm_build_env(const sxcl_jvm_opts *opts, sxcl_jvm_env *out, char *err, size_t err_len)
{
    const char *home;
    size_t i;
    if (opts == NULL || out == NULL) {
        jvm_err(err, err_len, "参数不合法");
        return SXCL_JVM_ERR_ARG;
    }
    home = opts->java_home;
    if (home == NULL || *home == '\0') {
        jvm_err(err, err_len, "java_home 没给");
        return SXCL_JVM_ERR_NO_HOME;
    }
    memset(out, 0, sizeof(*out));
    copy_str(out->java_home, sizeof(out->java_home), home);
    if (opts->tmp_dir != NULL && *opts->tmp_dir != '\0') {
        copy_str(out->tmp_dir, sizeof(out->tmp_dir), opts->tmp_dir);
    } else if (join_path(out->tmp_dir, sizeof(out->tmp_dir), home, "tmp") != 0) {
        jvm_err(err, err_len, "拼 <java_home>/tmp 失败(路径太长)");
        return SXCL_JVM_ERR_ARG;
    }
    if (opts->user_home != NULL && *opts->user_home != '\0') {
        copy_str(out->home, sizeof(out->home), opts->user_home);
    } else if (join_path(out->home, sizeof(out->home), home, "home") != 0) {
        jvm_err(err, err_len, "拼 <java_home>/home 失败(路径太长)");
        return SXCL_JVM_ERR_ARG;
    }
    /* LD_LIBRARY_PATH:<home>/lib : <home>/lib/server : extra_lib_dirs… */
    {
        char lib[SXCL_JVM_PATH_MAX];
        char server[SXCL_JVM_PATH_MAX];
        if (join_path(lib, sizeof(lib), home, "lib") != 0 ||
            join_path(server, sizeof(server), home, "lib/server") != 0) {
            jvm_err(err, err_len, "拼 <java_home>/lib 失败(路径太长)");
            return SXCL_JVM_ERR_ARG;
        }
        copy_str(out->ld_library_path, sizeof(out->ld_library_path), lib);
        if (append_path(out->ld_library_path, sizeof(out->ld_library_path), server) != 0) {
            jvm_err(err, err_len, "LD_LIBRARY_PATH 太长");
            return SXCL_JVM_ERR_ARG;
        }
    }
    if (opts->extra_lib_dirs != NULL) {
        for (i = 0; opts->extra_lib_dirs[i] != NULL; ++i) {
            if (opts->extra_lib_dirs[i][0] == '\0') {
                continue;
            }
            if (append_path(out->ld_library_path, sizeof(out->ld_library_path),
                            opts->extra_lib_dirs[i]) != 0) {
                jvm_err(err, err_len, "LD_LIBRARY_PATH 太长(追加 %s 时)", opts->extra_lib_dirs[i]);
                return SXCL_JVM_ERR_ARG;
            }
        }
    }
    /* java.library.path:默认与 LD_LIBRARY_PATH 同一份 */
    if (opts->java_library_path != NULL && *opts->java_library_path != '\0') {
        copy_str(out->java_library_path, sizeof(out->java_library_path), opts->java_library_path);
    } else {
        copy_str(out->java_library_path, sizeof(out->java_library_path), out->ld_library_path);
    }
    return SXCL_JVM_OK;
}

int sxcl_jvm_apply_env(const sxcl_jvm_env *env, char *err, size_t err_len)
{
    if (env == NULL) {
        jvm_err(err, err_len, "env 为空");
        return SXCL_JVM_ERR_ARG;
    }
#if defined(_WIN32)
    if (_putenv_s("JAVA_HOME", env->java_home) != 0 ||
        _putenv_s("PATH", env->ld_library_path) != 0 || _putenv_s("TMP", env->tmp_dir) != 0 ||
        _putenv_s("TEMP", env->tmp_dir) != 0 || _putenv_s("USERPROFILE", env->home) != 0) {
        jvm_err(err, err_len, "设置环境变量失败(_putenv_s)");
        return SXCL_JVM_ERR_IO;
    }
#else
    if (setenv("JAVA_HOME", env->java_home, 1) != 0 ||
        setenv("LD_LIBRARY_PATH", env->ld_library_path, 1) != 0 ||
        setenv("TMPDIR", env->tmp_dir, 1) != 0 || setenv("HOME", env->home, 1) != 0) {
        jvm_err(err, err_len, "设置环境变量失败(setenv)");
        return SXCL_JVM_ERR_IO;
    }
#endif
    return SXCL_JVM_OK;
}

/* ── 2) argv ── */

static int push_arg(sxcl_jvm_args *args, const char *text, char *err, size_t err_len)
{
    if (args->count + 1 >= SXCL_JVM_ARG_MAX) {
        jvm_err(err, err_len, "参数太多了(超过 %d 个)", SXCL_JVM_ARG_MAX - 1);
        return SXCL_JVM_ERR_ARG;
    }
    if (strlen(text) >= SXCL_JVM_ARG_LEN) {
        jvm_err(err, err_len, "参数太长:%s", text);
        return SXCL_JVM_ERR_ARG;
    }
    snprintf(args->storage[args->count], SXCL_JVM_ARG_LEN, "%s", text);
    args->argv[args->count] = args->storage[args->count];
    ++args->count;
    args->argv[args->count] = NULL;
    return SXCL_JVM_OK;
}

static int is_start_on_first_thread(const char *arg)
{
    static const char *const kBad = "-XstartOnFirstThread";
    return (arg != NULL && strncmp(arg, kBad, strlen(kBad)) == 0) ? 1 : 0;
}

int sxcl_jvm_build_args(const sxcl_jvm_opts *opts, const sxcl_jvm_env *env, int for_version,
                        sxcl_jvm_args *out, char *err, size_t err_len)
{
    char buf[SXCL_JVM_ARG_LEN];
    size_t i;
    int rc;
    if (opts == NULL || env == NULL || out == NULL) {
        jvm_err(err, err_len, "参数不合法");
        return SXCL_JVM_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->argv[0] = NULL;
    /* argv[0]:JLI 靠它算 application home。必须是**存在**的 <home>/bin/java(不需要可执行位)。 */
    if (join_path(buf, sizeof(buf), env->java_home, "bin/java") != 0) {
        jvm_err(err, err_len, "拼 <java_home>/bin/java 失败");
        return SXCL_JVM_ERR_ARG;
    }
#if defined(_WIN32)
    {
        const size_t n = strlen(buf);
        if (n + 4 < sizeof(buf)) {
            memcpy(buf + n, ".exe", 5);
        }
    }
#endif
    rc = push_arg(out, buf, err, err_len);
    if (rc != SXCL_JVM_OK) {
        return rc;
    }
    /* 进程内 dlopen 起 JVM:java.home 推不出来(/proc/self/exe 是我们的 APK),必须显式给 */
    snprintf(buf, sizeof(buf), "-Djava.home=%s", env->java_home);
    rc = push_arg(out, buf, err, err_len);
    if (rc != SXCL_JVM_OK) {
        return rc;
    }
    snprintf(buf, sizeof(buf), "-Djava.io.tmpdir=%s", env->tmp_dir);
    rc = push_arg(out, buf, err, err_len);
    if (rc != SXCL_JVM_OK) {
        return rc;
    }
    snprintf(buf, sizeof(buf), "-Duser.home=%s", env->home);
    rc = push_arg(out, buf, err, err_len);
    if (rc != SXCL_JVM_OK) {
        return rc;
    }
    rc = push_arg(out, "-Dos.name=Linux", err, err_len);
    if (rc != SXCL_JVM_OK) {
        return rc;
    }
    if (opts->android_version != NULL && *opts->android_version != '\0') {
        snprintf(buf, sizeof(buf), "-Dos.version=Android-%s", opts->android_version);
        rc = push_arg(out, buf, err, err_len);
        if (rc != SXCL_JVM_OK) {
            return rc;
        }
    }
    snprintf(buf, sizeof(buf), "-Djava.library.path=%s", env->java_library_path);
    rc = push_arg(out, buf, err, err_len);
    if (rc != SXCL_JVM_OK) {
        return rc;
    }
    if (opts->class_path != NULL && *opts->class_path != '\0') {
        rc = push_arg(out, "-classpath", err, err_len);
        if (rc != SXCL_JVM_OK) {
            return rc;
        }
        rc = push_arg(out, opts->class_path, err, err_len);
        if (rc != SXCL_JVM_OK) {
            return rc;
        }
    }
    if (opts->extra_args != NULL) {
        for (i = 0; opts->extra_args[i] != NULL; ++i) {
            if (opts->extra_args[i][0] == '\0') {
                continue;
            }
            /* 安卓上**绝不能**加 macOS 的 -XstartOnFirstThread:误传的直接丢掉并记一笔 */
            if (is_start_on_first_thread(opts->extra_args[i])) {
                out->dropped_start_on_first_thread = 1;
                continue;
            }
            rc = push_arg(out, opts->extra_args[i], err, err_len);
            if (rc != SXCL_JVM_OK) {
                return rc;
            }
        }
    }
    if (for_version) {
        rc = push_arg(out, "-XshowSettings:properties", err, err_len);
        if (rc != SXCL_JVM_OK) {
            return rc;
        }
        rc = push_arg(out, "-version", err, err_len);
        if (rc != SXCL_JVM_OK) {
            return rc;
        }
    } else if (opts->main_class != NULL && *opts->main_class != '\0') {
        rc = push_arg(out, opts->main_class, err, err_len);
        if (rc != SXCL_JVM_OK) {
            return rc;
        }
        if (opts->app_args != NULL) {
            for (i = 0; opts->app_args[i] != NULL; ++i) {
                rc = push_arg(out, opts->app_args[i], err, err_len);
                if (rc != SXCL_JVM_OK) {
                    return rc;
                }
            }
        }
    }
    return SXCL_JVM_OK;
}

/* ── 3) 预加载关键 .so ──
 *
 * 为什么不能只靠 LD_LIBRARY_PATH:bionic 的 linker 在**进程启动时**读一次
 * LD_LIBRARY_PATH,运行期 setenv 不一定生效(Android 上还提供
 * android_update_LD_LIBRARY_PATH 这个私有入口)。稳妥做法是把关键库
 * 按绝对路径 dlopen(RTLD_GLOBAL)一遍,让 libjli/libjvm 的 DT_NEEDED 能解析到。 */

static const char *const kPreloadNames[] = {
    "libc++_shared.so",  /* 安卓 JRE 的 bin/java 与 libjli.so 的 DT_NEEDED,只在 APK 里 */
    "libjvm.so",
    "libjava.so",
    "libverify.so",
    "libzip.so",
    "libnet.so",
    "libnio.so",
    "libinstrument.so",
    "libmanagement.so",
    "libjli.so",
};

static int preload_from_dir(const char *dir, int *loaded)
{
    size_t i;
    int found = 0;
    for (i = 0; i < sizeof(kPreloadNames) / sizeof(kPreloadNames[0]); ++i) {
        char path[SXCL_JVM_PATH_MAX];
        if (join_path(path, sizeof(path), dir, kPreloadNames[i]) != 0) {
            continue;
        }
        if (!sxcl_fs_exists(path)) {
            continue;
        }
        (void)SXCL_DLOPEN(path); /* 失败不算错:有的是真不需要,有的是稍后由 JVM 自己加载 */
        ++*loaded;
        found = 1;
    }
    return found;
}

static void preload_all(const sxcl_jvm_opts *opts, const sxcl_jvm_env *env, int *loaded)
{
    char lib[SXCL_JVM_PATH_MAX];
    char server[SXCL_JVM_PATH_MAX];
    size_t i;
    int pass;
    *loaded = 0;
    if (opts->preload_libs == 0) {
        return;
    }
    if (join_path(lib, sizeof(lib), env->java_home, "lib") != 0 ||
        join_path(server, sizeof(server), env->java_home, "lib/server") != 0) {
        return;
    }
    /* 两遍:第一遍可能因为互相依赖而失败,第二遍就能解开了 */
    for (pass = 0; pass < 2; ++pass) {
        (void)preload_from_dir(server, loaded);
        (void)preload_from_dir(lib, loaded);
        if (opts->extra_lib_dirs != NULL) {
            for (i = 0; opts->extra_lib_dirs[i] != NULL; ++i) {
                if (opts->extra_lib_dirs[i][0] != '\0') {
                    (void)preload_from_dir(opts->extra_lib_dirs[i], loaded);
                }
            }
        }
    }
}

/* ── 4) 输出重定向(自检留证) ── */

typedef struct capture {
    int active;
    int saved_out;
    int saved_err;
    int fd;
} capture;

static int capture_begin(capture *c, const char *path, char *err, size_t err_len)
{
    memset(c, 0, sizeof(*c));
    c->saved_out = -1;
    c->saved_err = -1;
    c->fd = -1;
    if (path == NULL || *path == '\0') {
        return SXCL_JVM_OK;
    }
    (void)fflush(stdout);
    (void)fflush(stderr);
    (void)sxcl_fs_mkdirs_for_file(path); /* 证词文件落在 <home>/… 时父目录可能还不存在 */
#if defined(_WIN32)
    c->fd = _open(path, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (c->fd < 0) {
        jvm_err(err, err_len, "打不开证词文件: %s", path);
        return SXCL_JVM_ERR_IO;
    }
    c->saved_out = _dup(_fileno(stdout));
    c->saved_err = _dup(_fileno(stderr));
    if (c->saved_out < 0 || c->saved_err < 0 || _dup2(c->fd, _fileno(stdout)) != 0 ||
        _dup2(c->fd, _fileno(stderr)) != 0) {
        jvm_err(err, err_len, "重定向 stdout/stderr 失败");
        return SXCL_JVM_ERR_IO;
    }
#else
    c->fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (c->fd < 0) {
        jvm_err(err, err_len, "打不开证词文件: %s", path);
        return SXCL_JVM_ERR_IO;
    }
    c->saved_out = dup(STDOUT_FILENO);
    c->saved_err = dup(STDERR_FILENO);
    if (c->saved_out < 0 || c->saved_err < 0 || dup2(c->fd, STDOUT_FILENO) != 0 ||
        dup2(c->fd, STDERR_FILENO) != 0) {
        jvm_err(err, err_len, "重定向 stdout/stderr 失败");
        return SXCL_JVM_ERR_IO;
    }
#endif
    c->active = 1;
    return SXCL_JVM_OK;
}

static void capture_end(capture *c)
{
    if (c->fd < 0) {
        return;
    }
    (void)fflush(stdout);
    (void)fflush(stderr);
    if (c->active) {
#if defined(_WIN32)
        (void)_dup2(c->saved_out, _fileno(stdout));
        (void)_dup2(c->saved_err, _fileno(stderr));
#else
        (void)dup2(c->saved_out, STDOUT_FILENO);
        (void)dup2(c->saved_err, STDERR_FILENO);
#endif
    }
    if (c->saved_out >= 0) {
#if defined(_WIN32)
        (void)_close(c->saved_out);
#else
        (void)close(c->saved_out);
#endif
    }
    if (c->saved_err >= 0) {
#if defined(_WIN32)
        (void)_close(c->saved_err);
#else
        (void)close(c->saved_err);
#endif
    }
#if defined(_WIN32)
    (void)_close(c->fd);
#else
    (void)close(c->fd);
#endif
    c->fd = -1;
    c->active = 0;
}

/* ── 5) 启动 ── */

/** 从 <home>/release 里读一行 KEY="VALUE"。找到返回 0。 */
static int read_release_value(const char *home, const char *key, char *out, size_t out_len)
{
    char path[SXCL_JVM_PATH_MAX];
    FILE *fp;
    char line[512];
    const size_t key_len = strlen(key);
    if (join_path(path, sizeof(path), home, "release") != 0) {
        return -1;
    }
    fp = sxcl_fs_fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') {
            continue;
        }
        {
            const char *v = line + key_len + 1;
            size_t vn;
            if (*v == '"') {
                ++v;
            }
            vn = strlen(v);
            if (vn > 0 && v[vn - 1] == '"') {
                --vn;
            }
            if (vn >= out_len) {
                vn = out_len - 1;
            }
            memcpy(out, v, vn);
            out[vn] = '\0';
            (void)fclose(fp);
            return 0;
        }
    }
    (void)fclose(fp);
    return -1;
}

/** "17.0.9+11" -> "17.0.9"(JLI 的 dotversion 只要主版本样式) */
static void dot_version_of(const char *full, char *out, size_t out_len)
{
    size_t i = 0;
    int dots = 0;
    while (full[i] != '\0' && dots < 2) {
        if (full[i] == '.') {
            ++dots;
        }
        ++i;
    }
    if (full[i] == '\0') {
        copy_str(out, out_len, full);
        return;
    }
    while (full[i] != '\0' && full[i] != '.') {
        ++i;
    }
    if (i >= out_len) {
        i = out_len - 1;
    }
    memcpy(out, full, i);
    out[i] = '\0';
}

static int resolve_jli_path(const char *home, char *out, size_t out_len)
{
    char cand[SXCL_JVM_PATH_MAX];
#if !defined(_WIN32)
    char lib[SXCL_JVM_PATH_MAX];
#endif
#if defined(_WIN32)
    if (join_path(cand, sizeof(cand), home, "bin/" SXCL_JLI_NAME) == 0 && sxcl_fs_exists(cand)) {
        copy_str(out, out_len, cand);
        return 0;
    }
    if (join_path(cand, sizeof(cand), home, "bin/java.dll") == 0 && sxcl_fs_exists(cand)) {
        copy_str(out, out_len, cand);
        return 0;
    }
#else
    if (join_path(lib, sizeof(lib), home, "lib") == 0 &&
        join_path(cand, sizeof(cand), lib, SXCL_JLI_NAME) == 0 && sxcl_fs_exists(cand)) {
        copy_str(out, out_len, cand);
        return 0;
    }
    /* jre8 的布局是 <home>/jre/lib(有些打包把 jre 套了一层) */
    if (join_path(lib, sizeof(lib), home, "jre/lib") == 0 &&
        join_path(cand, sizeof(cand), lib, SXCL_JLI_NAME) == 0 && sxcl_fs_exists(cand)) {
        copy_str(out, out_len, cand);
        return 0;
    }
#endif
    return -1;
}

static int jvm_prepare(const sxcl_jvm_opts *opts, sxcl_jvm_env *env, sxcl_jvm_result *out,
                       char *err, size_t err_len)
{
    if (opts == NULL || out == NULL) {
        return SXCL_JVM_ERR_ARG;
    }
    if (opts->java_home == NULL || opts->java_home[0] == '\0') {
        jvm_err(err, err_len, "java_home 没给");
        return SXCL_JVM_ERR_NO_HOME;
    }
    if (!sxcl_fs_is_dir(opts->java_home)) {
        jvm_err(err, err_len, "java_home 不是目录: %s", opts->java_home);
        return SXCL_JVM_ERR_NO_HOME;
    }
    {
        const int rc = sxcl_jvm_build_env(opts, env, err, err_len);
        if (rc != SXCL_JVM_OK) {
            return rc;
        }
    }
    copy_str(out->java_home, sizeof(out->java_home), opts->java_home);
    copy_str(out->ld_library_path, sizeof(out->ld_library_path), env->ld_library_path);
    copy_str(out->tmp_dir, sizeof(out->tmp_dir), env->tmp_dir);
    copy_str(out->user_home, sizeof(out->user_home), env->home);
    if (opts->android_version != NULL && *opts->android_version != '\0') {
        snprintf(out->os_version, sizeof(out->os_version), "Android-%s", opts->android_version);
    }
    if (resolve_jli_path(opts->java_home, out->jli_path, sizeof(out->jli_path)) != 0) {
        jvm_err(err, err_len, "在 %s 里找不到 %s(这份 JRE 不完整?)", opts->java_home,
                SXCL_JLI_NAME);
        return SXCL_JVM_ERR_NO_JLI;
    }
    (void)sxcl_fs_mkdirs(out->tmp_dir);
    (void)sxcl_fs_mkdirs(out->user_home);
    if (opts->apply_env != 0) {
        const int rc = sxcl_jvm_apply_env(env, err, err_len);
        if (rc != SXCL_JVM_OK) {
            return rc;
        }
    }
    return SXCL_JVM_OK;
}

int sxcl_jvm_probe(const sxcl_jvm_opts *opts, sxcl_jvm_result *out)
{
    sxcl_jvm_env env;
    char err[SXCL_JVM_ERROR_MAX];
    void *handle;
    void *sym;
    int loaded = 0;
    int rc;
    if (out == NULL) {
        return SXCL_JVM_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    err[0] = '\0';
    rc = jvm_prepare(opts, &env, out, err, sizeof(err));
    if (rc != SXCL_JVM_OK) {
        copy_str(out->error, sizeof(out->error), err);
        out->code = rc;
        return rc;
    }
    preload_all(opts, &env, &loaded);
    handle = SXCL_DLOPEN(out->jli_path);
    if (handle == NULL) {
#if defined(_WIN32)
        jvm_err(err, sizeof(err), "LoadLibrary(%s) 失败(错误码 %lu)", out->jli_path,
                (unsigned long)GetLastError());
#else
        {
            const char *dl = dlerror();
            jvm_err(err, sizeof(err), "dlopen(%s) 失败: %s", out->jli_path,
                    dl != NULL ? dl : "(没有 dlerror)");
        }
#endif
        copy_str(out->error, sizeof(out->error), err);
        out->code = SXCL_JVM_ERR_DLOPEN;
        return SXCL_JVM_ERR_DLOPEN;
    }
    sym = SXCL_DLSYM(handle, "JLI_Launch");
    if (sym == NULL) {
        jvm_err(err, sizeof(err), "dlsym(JLI_Launch) 失败: %s", out->jli_path);
        copy_str(out->error, sizeof(out->error), err);
        out->code = SXCL_JVM_ERR_DLSYM;
        return SXCL_JVM_ERR_DLSYM;
    }
    out->code = SXCL_JVM_OK;
    return SXCL_JVM_OK;
}

int sxcl_jvm_launch(const sxcl_jvm_opts *opts, int for_version, sxcl_jvm_result *out)
{
    sxcl_jvm_env env;
    sxcl_jvm_args args;
    char err[SXCL_JVM_ERROR_MAX];
    char full_version[64];
    char dot_version[64];
    capture cap;
    void *handle;
    void *sym;
    sxcl_jli_launch_fn launch;
    int loaded = 0;
    int rc;
    int jli_rc = 0;

    if (opts == NULL || out == NULL) {
        return SXCL_JVM_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    err[0] = '\0';
    rc = jvm_prepare(opts, &env, out, err, sizeof(err));
    if (rc != SXCL_JVM_OK) {
        copy_str(out->error, sizeof(out->error), err);
        out->code = rc;
        return rc;
    }
    rc = sxcl_jvm_build_args(opts, &env, for_version, &args, err, sizeof(err));
    if (rc != SXCL_JVM_OK) {
        copy_str(out->error, sizeof(out->error), err);
        out->code = rc;
        return rc;
    }
    if (args.count == 0) {
        copy_str(out->error, sizeof(out->error), "argv 是空的");
        out->code = SXCL_JVM_ERR_ARG;
        return SXCL_JVM_ERR_ARG;
    }
    /* JLI 的 fullversion/dotversion:优先用 JRE 自带 release 里的 JAVA_VERSION */
    full_version[0] = '\0';
    if (read_release_value(opts->java_home, "JAVA_VERSION", full_version,
                           sizeof(full_version)) != 0) {
        copy_str(full_version, sizeof(full_version), "1.8.0-internal");
    }
    dot_version_of(full_version, dot_version, sizeof(dot_version));

    rc = capture_begin(&cap, opts->capture_path, err, sizeof(err));
    if (rc != SXCL_JVM_OK) {
        capture_end(&cap);
        copy_str(out->error, sizeof(out->error), err);
        out->code = rc;
        return rc;
    }
    if (opts->capture_path != NULL && *opts->capture_path != '\0') {
        copy_str(out->output_path, sizeof(out->output_path), opts->capture_path);
    }
    preload_all(opts, &env, &loaded);
    handle = SXCL_DLOPEN(out->jli_path);
    if (handle == NULL) {
#if defined(_WIN32)
        jvm_err(err, sizeof(err), "LoadLibrary(%s) 失败(错误码 %lu)", out->jli_path,
                (unsigned long)GetLastError());
#else
        {
            const char *dl = dlerror();
            jvm_err(err, sizeof(err), "dlopen(%s) 失败: %s", out->jli_path,
                    dl != NULL ? dl : "(没有 dlerror)");
        }
#endif
        capture_end(&cap);
        copy_str(out->error, sizeof(out->error), err);
        out->code = SXCL_JVM_ERR_DLOPEN;
        return SXCL_JVM_ERR_DLOPEN;
    }
    sym = SXCL_DLSYM(handle, "JLI_Launch");
    if (sym == NULL) {
        capture_end(&cap);
        copy_str(out->error, sizeof(out->error), "dlsym(JLI_Launch) 拿不到符号");
        out->code = SXCL_JVM_ERR_DLSYM;
        return SXCL_JVM_ERR_DLSYM;
    }
    /* 参数顺序与 OpenJDK launcher(java.c)的 JLI_Launch 逐条对齐 */
    launch = (sxcl_jli_launch_fn)sym;
    jli_rc = (int)launch((int)args.count, (char **)args.argv, 0, NULL, 0, NULL, full_version,
                         dot_version, args.argv[0], args.argv[0], (sxcl_jboolean)0,
                         (sxcl_jboolean)1, (sxcl_jboolean)0, (sxcl_jint)0);
    capture_end(&cap);
    out->jli_rc = jli_rc;
    if (opts->capture_path != NULL && *opts->capture_path != '\0') {
        int64_t size = 0;
        if (sxcl_fs_stat(opts->capture_path, &size, NULL) == 0 && size > 0) {
            out->output_bytes = size;
        }
    }
    rc = (jli_rc == 0) ? SXCL_JVM_OK : SXCL_JVM_ERR_LAUNCH;
    if (rc != SXCL_JVM_OK) {
        jvm_err(err, sizeof(err), "JLI_Launch 返回 %d(非 0 = JVM 没起来或启动失败)", jli_rc);
        copy_str(out->error, sizeof(out->error), err);
    }
    out->code = rc;
    return rc;
}

/* ── 6) 自检 ── */

int sxcl_jvm_scan_property(const char *text, const char *key, char *out, size_t out_len)
{
    const char *p;
    const size_t key_len = strlen(key);
    if (text == NULL || key == NULL || out == NULL || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    for (p = text; (p = strstr(p, key)) != NULL; ++p) {
        const char *q = p + key_len;
        while (*q == ' ' || *q == '\t') {
            ++q;
        }
        if (*q != '=') {
            continue;
        }
        ++q;
        while (*q == ' ' || *q == '\t') {
            ++q;
        }
        {
            size_t n = 0;
            while (q[n] != '\0' && q[n] != '\n' && q[n] != '\r') {
                ++n;
            }
            if (n >= out_len) {
                n = out_len - 1;
            }
            memcpy(out, q, n);
            out[n] = '\0';
            return 0;
        }
    }
    return -1;
}

static int read_all_file(const char *path, char **out, size_t *out_len)
{
    FILE *fp = sxcl_fs_fopen(path, "rb");
    long size;
    char *buf;
    if (fp == NULL) {
        return -1;
    }
    (void)fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    if (size < 0 || size > 16 * 1024 * 1024) {
        (void)fclose(fp);
        return -1;
    }
    buf = (char *)malloc((size_t)size + 1u);
    if (buf == NULL) {
        (void)fclose(fp);
        return -1;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        (void)fclose(fp);
        return -1;
    }
    (void)fclose(fp);
    buf[size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

int sxcl_jvm_selfcheck(const sxcl_jvm_opts *opts, const char *capture_path, sxcl_jvm_result *out)
{
    sxcl_jvm_opts local;
    char err[SXCL_JVM_ERROR_MAX];
    int rc;

    if (opts == NULL || out == NULL) {
        return SXCL_JVM_ERR_ARG;
    }
    if (capture_path == NULL || *capture_path == '\0') {
        return SXCL_JVM_ERR_ARG; /* 自检必须留证:没有证词文件就不叫自检 */
    }
    local = *opts;
    local.capture_path = capture_path;
    rc = sxcl_jvm_launch(&local, 1 /* for_version */, out);
    if (rc != SXCL_JVM_OK) {
        return rc;
    }
    {
        char *text = NULL;
        size_t len = 0;
        if (read_all_file(capture_path, &text, &len) != 0) {
            jvm_err(err, sizeof(err), "自检跑完了,但读不到证词文件 %s", capture_path);
            copy_str(out->error, sizeof(out->error), err);
            out->code = SXCL_JVM_ERR_IO;
            return SXCL_JVM_ERR_IO;
        }
        if (sxcl_jvm_scan_property(text, "java.version", out->java_version,
                                   sizeof(out->java_version)) != 0) {
            /* -XshowSettings 的写法是 "    java.version = 17.0.9";老版本可能是 "java version" */
            (void)sxcl_jvm_scan_property(text, "java.version", out->java_version,
                                         sizeof(out->java_version));
        }
        (void)sxcl_jvm_scan_property(text, "java.home", out->reported_home,
                                     sizeof(out->reported_home));
        free(text);
    }
    if (out->java_version[0] == '\0') {
        jvm_err(err, sizeof(err),
                "自检输出里没有 java.version(原始输出在 %s,自己看一眼)", capture_path);
        copy_str(out->error, sizeof(out->error), err);
        out->code = SXCL_JVM_ERR_MISSING;
        return SXCL_JVM_ERR_MISSING;
    }
    out->home_matches = 0;
    if (out->reported_home[0] != '\0') {
        size_t i = strlen(out->reported_home);
        char normalized[SXCL_JVM_VALUE_MAX];
        copy_str(normalized, sizeof(normalized), out->reported_home);
        while (i > 1 && (normalized[i - 1] == '/' || normalized[i - 1] == '\\')) {
            normalized[--i] = '\0';
        }
        out->home_matches = (strcmp(normalized, out->java_home) == 0) ? 1 : 0;
    }
    out->code = SXCL_JVM_OK;
    return SXCL_JVM_OK;
}
