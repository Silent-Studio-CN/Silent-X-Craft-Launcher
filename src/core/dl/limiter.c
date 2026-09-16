#include "sxcl/limiter.h"

#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <time.h>
#  include <pthread.h>
#  include <errno.h>
#endif

/* 桶容量取 0.25 秒额度,并夹在 64KiB ~ 8MiB 之间(与 Python 版一致) */
#define SXCL_BURST_SECONDS 0.25
#define SXCL_BURST_MIN (64.0 * 1024.0)
#define SXCL_BURST_MAX (8.0 * 1024.0 * 1024.0)

/* consume 的单次睡眠上限(秒) */
#define SXCL_CONSUME_SLICE 0.2

struct sxcl_limiter {
#if defined(_WIN32)
    CRITICAL_SECTION lock;
    LARGE_INTEGER freq;
#else
    pthread_mutex_t lock;
#endif
    double rate;   /* 字节/秒,0 = 不限速 */
    double burst;  /* 桶容量 */
    double tokens; /* 当前令牌 */
    double last;   /* 上次装填时刻(单调秒) */
};

static double sxcl_calc_burst(double rate)
{
    if (rate <= 0.0) {
        return 0.0;
    }
    double burst = rate * SXCL_BURST_SECONDS;
    if (burst < SXCL_BURST_MIN) {
        burst = SXCL_BURST_MIN;
    }
    if (burst > SXCL_BURST_MAX) {
        burst = SXCL_BURST_MAX;
    }
    return burst;
}

double sxcl_limiter_now(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static int inited = 0;
    LARGE_INTEGER counter;
    if (!inited) {
        QueryPerformanceFrequency(&freq);
        inited = 1;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void sxcl_lock(sxcl_limiter *lim)
{
#if defined(_WIN32)
    EnterCriticalSection(&lim->lock);
#else
    pthread_mutex_lock(&lim->lock);
#endif
}

static void sxcl_unlock(sxcl_limiter *lim)
{
#if defined(_WIN32)
    LeaveCriticalSection(&lim->lock);
#else
    pthread_mutex_unlock(&lim->lock);
#endif
}

sxcl_limiter *sxcl_limiter_create(double rate_bps)
{
    sxcl_limiter *lim = (sxcl_limiter *)calloc(1, sizeof(sxcl_limiter));
    if (!lim) {
        return NULL;
    }
#if defined(_WIN32)
    QueryPerformanceFrequency(&lim->freq);
    InitializeCriticalSection(&lim->lock);
#else
    if (pthread_mutex_init(&lim->lock, NULL) != 0) {
        free(lim);
        return NULL;
    }
#endif
    lim->rate = rate_bps > 0.0 ? rate_bps : 0.0;
    lim->burst = sxcl_calc_burst(lim->rate);
    lim->tokens = lim->burst;
    lim->last = sxcl_limiter_now();
    return lim;
}

void sxcl_limiter_destroy(sxcl_limiter *lim)
{
    if (!lim) {
        return;
    }
#if defined(_WIN32)
    DeleteCriticalSection(&lim->lock);
#else
    pthread_mutex_destroy(&lim->lock);
#endif
    free(lim);
}

void sxcl_limiter_set_rate(sxcl_limiter *lim, double rate_bps)
{
    if (!lim) {
        return;
    }
    sxcl_lock(lim);
    lim->rate = rate_bps > 0.0 ? rate_bps : 0.0;
    lim->burst = sxcl_calc_burst(lim->rate);
    if (lim->rate <= 0.0) {
        lim->tokens = 0.0;
    } else if (lim->tokens > lim->burst) {
        lim->tokens = lim->burst;
    }
    lim->last = sxcl_limiter_now();
    sxcl_unlock(lim);
}

double sxcl_limiter_rate(const sxcl_limiter *lim)
{
    return lim ? lim->rate : 0.0;
}

double sxcl_limiter_burst(const sxcl_limiter *lim)
{
    return lim ? lim->burst : 0.0;
}

static void sxcl_refill_locked(sxcl_limiter *lim)
{
    const double now = sxcl_limiter_now();
    const double delta = now - lim->last;
    lim->last = now;
    if (lim->rate > 0.0 && delta > 0.0) {
        lim->tokens += delta * lim->rate;
        if (lim->tokens > lim->burst) {
            lim->tokens = lim->burst;
        }
    }
}

double sxcl_limiter_take(sxcl_limiter *lim, uint64_t bytes)
{
    if (!lim || bytes == 0) {
        return 0.0;
    }
    if (lim->rate <= 0.0) {
        return 0.0; /* 不限速 */
    }

    double wait = 0.0;
    sxcl_lock(lim);
    sxcl_refill_locked(lim);

    const double need = (double)bytes;
    if (need > lim->burst) {
        /* 单次请求比桶还大:按攒满即放行处理,避免永远等不满 */
        lim->tokens = 0.0;
        wait = need / lim->rate;
    } else if (lim->tokens >= need) {
        lim->tokens -= need;
    } else {
        /* 关键:令牌不足时保留零头,只回报还需等待的时间 */
        wait = (need - lim->tokens) / lim->rate;
    }
    sxcl_unlock(lim);
    return wait;
}

void sxcl_limiter_consume(sxcl_limiter *lim, uint64_t bytes)
{
    if (!lim || bytes == 0) {
        return;
    }
    for (;;) {
        const double wait = sxcl_limiter_take(lim, bytes);
        if (wait <= 0.0) {
            return;
        }
        double slice = wait < SXCL_CONSUME_SLICE ? wait : SXCL_CONSUME_SLICE;
        if (slice <= 0.0) {
            slice = 0.001;
        }
#if defined(_WIN32)
        Sleep((DWORD)(slice * 1000.0 + 0.5));
#else
        struct timespec ts;
        ts.tv_sec = (time_t)slice;
        ts.tv_nsec = (long)((slice - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
#endif
    }
}

void sxcl_limiter_reset(sxcl_limiter *lim)
{
    if (!lim) {
        return;
    }
    sxcl_lock(lim);
    lim->tokens = lim->burst;
    lim->last = sxcl_limiter_now();
    sxcl_unlock(lim);
}

/* 就地删除 str 中所有出现的 pattern(python str.replace 语义) */
static void sxcl_remove_all(char *str, const char *pattern)
{
    const size_t plen = strlen(pattern);
    if (plen == 0) {
        return;
    }
    char *read = str;
    char *write = str;
    while (*read != '\0') {
        if (strncmp(read, pattern, plen) == 0) {
            read += plen;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

/* 与 python parse_rate 逐条对齐:
 *   raw = text.strip().lower().replace("/s","").replace("ps","").strip()
 *   空 / "0" / "unlimited" / "none" / "off" / "不限速"  => 不限速
 *   前导数字与 '.' 为数值,其余为单位;_UNITS 未命中时按 1 倍(不是不限速) */
double sxcl_limiter_parse_rate(const char *text)
{
    if (!text) {
        return 0.0;
    }
    const size_t len = strlen(text);
    char *norm = (char *)malloc(len + 1);
    if (!norm) {
        return 0.0;
    }
    size_t w = 0;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (isspace(c)) {
            continue;
        }
        norm[w++] = (char)tolower(c);
    }
    norm[w] = '\0';
    sxcl_remove_all(norm, "/s");
    sxcl_remove_all(norm, "ps");

    double result = 0.0;
    if (norm[0] == '\0' || strcmp(norm, "0") == 0 || strcmp(norm, "unlimited") == 0 ||
        strcmp(norm, "none") == 0 || strcmp(norm, "off") == 0 || strcmp(norm, "不限速") == 0) {
        free(norm);
        return 0.0;
    }

    size_t n = 0;
    while (norm[n] != '\0' && (isdigit((unsigned char)norm[n]) != 0 || norm[n] == '.')) {
        ++n;
    }
    if (n > 0) {
        char *num = (char *)malloc(n + 1);
        if (num) {
            memcpy(num, norm, n);
            num[n] = '\0';
            char *endp = NULL;
            const double value = strtod(num, &endp);
            /* python float(num):必须整段都是合法数值,否则 ValueError -> 不限速 */
            if (endp == num + n) {
                const char *unit = norm + n;
                double mult = 1.0;
                if (unit[0] != '\0' && strcmp(unit, "b") != 0) {
                    if (strcmp(unit, "k") == 0 || strcmp(unit, "kb") == 0 || strcmp(unit, "kib") == 0) {
                        mult = 1024.0;
                    } else if (strcmp(unit, "m") == 0 || strcmp(unit, "mb") == 0 || strcmp(unit, "mib") == 0) {
                        mult = 1024.0 * 1024.0;
                    } else if (strcmp(unit, "g") == 0 || strcmp(unit, "gb") == 0 || strcmp(unit, "gib") == 0) {
                        mult = 1024.0 * 1024.0 * 1024.0;
                    }
                    /* 未知单位:_UNITS.get(unit, 1) -> 1 倍 */
                }
                result = value * mult;
            }
            free(num);
        }
    }
    free(norm);
    return result;
}
