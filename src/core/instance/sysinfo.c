/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/sysinfo.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <sys/sysctl.h>
#  include <sys/types.h>
#  include <unistd.h>
#else
#  include <unistd.h>
#endif

#define SXCL_MB (1024ull * 1024ull)

static void sysinfo_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static void sysinfo_copy(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    const size_t n = strlen(src);
    const size_t take = (n < cap - 1) ? n : cap - 1;
    (void)memcpy(dst, src, take);
    dst[take] = '\0';
}

/* ══════════════════════ 平台:物理内存 ══════════════════════ */

#if defined(_WIN32)

static int sysinfo_platform_memory(uint64_t *total, uint64_t *available)
{
    MEMORYSTATUSEX status;
    (void)memset(&status, 0, sizeof(status));
    status.dwLength = (DWORD)sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) {
        return -1;
    }
    *total = (uint64_t)status.ullTotalPhys;
    *available = (uint64_t)status.ullAvailPhys;
    return 0;
}

static int sysinfo_platform_physical_cores(void)
{
    DWORD len = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len) == 0 &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return 0;
    }
    if (len == 0) {
        return 0;
    }
    unsigned char *buffer = (unsigned char *)malloc((size_t)len);
    if (buffer == NULL) {
        return 0;
    }
    int cores = 0;
    DWORD got = len;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                                        (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer, &got) != 0) {
        DWORD offset = 0;
        while (offset < got) {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *item =
                (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(const void *)(buffer + offset);
            if (item->Relationship == RelationProcessorCore) {
                ++cores;
            }
            if (item->Size == 0) {
                break;
            }
            offset += item->Size;
        }
    }
    free(buffer);
    return cores;
}

static void sysinfo_platform_cpu_name(char *out, size_t out_len)
{
    /* 环境变量是零依赖的:形如 "Intel64 Family 6 Model 140 Stepping 1, GenuineIntel" */
    const char *value = getenv("PROCESSOR_IDENTIFIER");
    sysinfo_copy(out, out_len, (value != NULL) ? value : "");
}

#elif defined(__APPLE__)

static int sysinfo_platform_memory(uint64_t *total, uint64_t *available)
{
    uint64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) != 0 || memsize == 0) {
        return -1;
    }
    *total = memsize;
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    *available = (pages > 0 && page_size > 0) ? ((uint64_t)pages * (uint64_t)page_size) : 0;
    return 0;
}

static int sysinfo_platform_physical_cores(void)
{
    int value = 0;
    size_t len = sizeof(value);
    if (sysctlbyname("hw.physicalcpu", &value, &len, NULL, 0) != 0) {
        return 0;
    }
    return value;
}

static void sysinfo_platform_cpu_name(char *out, size_t out_len)
{
    char brand[128];
    size_t len = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", brand, &len, NULL, 0) == 0) {
        brand[sizeof(brand) - 1] = '\0';
        sysinfo_copy(out, out_len, brand);
        return;
    }
    sysinfo_copy(out, out_len, "");
}

#else /* Linux / Android / 其它 POSIX */

/* /proc/meminfo 的 "MemTotal:  16384000 kB" / "MemAvailable: ... kB" */
static int sysinfo_read_meminfo(uint64_t *total, uint64_t *available)
{
    FILE *fh = fopen("/proc/meminfo", "rb");
    if (fh == NULL) {
        return -1;
    }
    char line[256];
    while (fgets(line, (int)sizeof(line), fh) != NULL) {
        unsigned long long value = 0;
        if (strncmp(line, "MemTotal:", 9) == 0 && sscanf(line + 9, "%llu", &value) == 1) {
            *total = (uint64_t)value * 1024ull;
        } else if (strncmp(line, "MemAvailable:", 13) == 0 && sscanf(line + 13, "%llu", &value) == 1) {
            *available = (uint64_t)value * 1024ull;
        }
    }
    (void)fclose(fh);
    return (*total > 0) ? 0 : -1;
}

static int sysinfo_platform_memory(uint64_t *total, uint64_t *available)
{
    *total = 0;
    *available = 0;
    if (sysinfo_read_meminfo(total, available) == 0) {
        return 0;
    }
    /* Android 老内核可能没有 MemAvailable;sysconf 是最后的兜底 */
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        *total = (uint64_t)pages * (uint64_t)page_size;
    }
    const long avail_pages = sysconf(_SC_AVPHYS_PAGES);
    if (avail_pages > 0 && page_size > 0) {
        *available = (uint64_t)avail_pages * (uint64_t)page_size;
    }
    return (*total > 0) ? 0 : -1;
}

static int sysinfo_platform_physical_cores(void)
{
    /* Linux 的物理核心数要看 /proc/cpuinfo 的 core id / physical id 组合,这里不去猜:
     * 拿不到就返回 0,由调用方决定(界面只显示逻辑核心数)。 */
    return 0;
}

static void sysinfo_platform_cpu_name(char *out, size_t out_len)
{
    out[0] = '\0';
    FILE *fh = fopen("/proc/cpuinfo", "rb");
    if (fh == NULL) {
        return;
    }
    char line[256];
    while (fgets(line, (int)sizeof(line), fh) != NULL) {
        const char *keys[] = { "model name", "Hardware", "Processor" };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            const size_t klen = strlen(keys[i]);
            if (strncmp(line, keys[i], klen) == 0 && line[klen] == ':') {
                const char *value = line + klen + 1;
                while (*value == ' ' || *value == '\t') {
                    ++value;
                }
                size_t n = strlen(value);
                while (n > 0 && (value[n - 1] == '\n' || value[n - 1] == '\r')) {
                    --n;
                }
                if (n > 0) {
                    const size_t take = (n < out_len - 1) ? n : out_len - 1;
                    (void)memcpy(out, value, take);
                    out[take] = '\0';
                    (void)fclose(fh);
                    return;
                }
            }
        }
    }
    (void)fclose(fh);
}

#endif

/* ══════════════════════ 对外接口 ══════════════════════ */

int sxcl_sysinfo_query(sxcl_sysinfo *out, char *err, size_t err_len)
{
    if (out == NULL) {
        sysinfo_err(err, err_len, "参数不合法（out 不能为空）");
        return SXCL_SYSINFO_ERR_ARG;
    }
    (void)memset(out, 0, sizeof(*out));

    uint64_t total = 0;
    uint64_t available = 0;
    const int rc = sysinfo_platform_memory(&total, &available);
    out->total_bytes = (rc == 0) ? total : 0;
    out->available_bytes = (rc == 0) ? available : 0;
    out->total_mb = total / SXCL_MB;
    out->available_mb = available / SXCL_MB;
    out->cpu_logical = sxcl_sysinfo_cpu_logical();
    out->cpu_physical = sysinfo_platform_physical_cores();
    sysinfo_platform_cpu_name(out->cpu_name, sizeof(out->cpu_name));

    if (rc != 0 || total == 0) {
        sysinfo_err(err, err_len, "拿不到物理内存信息（这个平台/这台机器查不出来）");
        return SXCL_SYSINFO_ERR_UNKNOWN;
    }
    return SXCL_SYSINFO_OK;
}

uint64_t sxcl_sysinfo_total_mb(void)
{
    uint64_t total = 0;
    uint64_t available = 0;
    if (sysinfo_platform_memory(&total, &available) != 0) {
        return 0;
    }
    return total / SXCL_MB;
}

uint64_t sxcl_sysinfo_available_mb(void)
{
    uint64_t total = 0;
    uint64_t available = 0;
    if (sysinfo_platform_memory(&total, &available) != 0) {
        return 0;
    }
    return available / SXCL_MB;
}

int sxcl_sysinfo_cpu_logical(void)
{
#if defined(_WIN32)
    SYSTEM_INFO info;
    (void)memset(&info, 0, sizeof(info));
    GetSystemInfo(&info);
    return (int)info.dwNumberOfProcessors;
#elif defined(__APPLE__)
    int value = 0;
    size_t len = sizeof(value);
    if (sysctlbyname("hw.logicalcpu", &value, &len, NULL, 0) == 0 && value > 0) {
        return value;
    }
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    return (cores > 0) ? (int)cores : 0;
#else
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    return (cores > 0) ? (int)cores : 0;
#endif
}

int sxcl_sysinfo_cpu_physical(void)
{
    return sysinfo_platform_physical_cores();
}

uint64_t sxcl_sysinfo_heap_for(uint64_t total_mb)
{
    if (total_mb == 0) {
        return 2048u; /* 查不出来时给一个保守默认(和 PCL 的默认差不多) */
    }
    uint64_t half = total_mb / 2u;
    if (half < (uint64_t)SXCL_SYSINFO_HEAP_MIN_MB) {
        return (uint64_t)SXCL_SYSINFO_HEAP_MIN_MB;
    }
    if (half > (uint64_t)SXCL_SYSINFO_HEAP_MAX_MB) {
        return (uint64_t)SXCL_SYSINFO_HEAP_MAX_MB;
    }
    return half;
}

uint64_t sxcl_sysinfo_recommended_heap_mb(void)
{
    return sxcl_sysinfo_heap_for(sxcl_sysinfo_total_mb());
}

int sxcl_sysinfo_workers_for(int cpu_logical)
{
    if (cpu_logical <= 0) {
        return 4;
    }
    if (cpu_logical < SXCL_SYSINFO_WORKERS_MIN) {
        return SXCL_SYSINFO_WORKERS_MIN;
    }
    if (cpu_logical > SXCL_SYSINFO_WORKERS_MAX) {
        return SXCL_SYSINFO_WORKERS_MAX;
    }
    return cpu_logical;
}

int sxcl_sysinfo_recommended_workers(void)
{
    return sxcl_sysinfo_workers_for(sxcl_sysinfo_cpu_logical());
}
