/* SXCL-C 机器信息(物理内存 / CPU 核心)—— 设置页的内存滑块与线程数建议都靠它。
 *
 * 各平台实现(都只用系统自带能力,零第三方依赖):
 *    Windows  GlobalMemoryStatusEx(内存)+ GetSystemInfo / GetLogicalProcessorInformationEx(CPU)
 *    Linux    /proc/meminfo 的 MemTotal / MemAvailable(拿不到就用 sysconf 兜底)
 *    Android  同上(/proc/meminfo 一直有;不依赖 glibc 的 sysinfo())
 *    macOS    sysctl hw.memsize + hw.physicalcpu + sysconf
 *
 * 约定:
 *   - 全部返回 MB(uint64),算不出来返回 0;查询失败给 SXCL_SYSINFO_ERR_* + 人话 err。
 *   - 不抛异常、不 abort;拿不到就是拿不到(界面显示"未知"),不要用假数字糊弄。
 *   - 推荐堆 = 物理内存的 1/2,夹在 1024 MB ~ 8192 MB 之间(PCL 的口径:太小不够用,太大 JVM 反而崩);
 *     这正是设置页内存滑块的默认值来源。
 */
#ifndef SXCL_SYSINFO_H
#define SXCL_SYSINFO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SXCL_SYSINFO_OK            0
#define SXCL_SYSINFO_ERR_ARG     (-1)
#define SXCL_SYSINFO_ERR_UNKNOWN (-2)  /* 这台机器/这个平台查不出来 */
#define SXCL_SYSINFO_ERROR_MAX     224

/* 推荐堆内存的夹取范围(MB),与设置页滑块一致 */
#define SXCL_SYSINFO_HEAP_MIN_MB      1024u
#define SXCL_SYSINFO_HEAP_MAX_MB      8192u
/* 推荐下载/解压线程数的夹取范围 */
#define SXCL_SYSINFO_WORKERS_MIN       2
#define SXCL_SYSINFO_WORKERS_MAX      16

#define SXCL_SYSINFO_CPU_NAME_MAX     128

/** 一次查询的全部结果。字段拿不到时是 0 / 空串,由调用方决定怎么显示。 */
typedef struct sxcl_sysinfo {
    uint64_t total_bytes;      /**< 物理内存总量(字节;拿不到 0) */
    uint64_t available_bytes;  /**< 当前可用物理内存(字节;拿不到 0) */
    uint64_t total_mb;         /**< 物理内存总量(MB,向下取整) */
    uint64_t available_mb;     /**< 可用物理内存(MB) */
    int cpu_logical;           /**< 逻辑核心数(拿不到 0) */
    int cpu_physical;          /**< 物理核心数(拿不到 0;Windows/Linux/macOS 尽力而为) */
    char cpu_name[SXCL_SYSINFO_CPU_NAME_MAX]; /**< CPU 名字(拿不到空串;Windows 取环境变量 PROCESSOR_IDENTIFIER) */
} sxcl_sysinfo;

/** 查询一次(结果全在结构里,不涉及生命周期/分配)。out 必填。
 *  连物理内存总量都拿不到返回 SXCL_SYSINFO_ERR_UNKNOWN(其余字段照样填能填的)。 */
int sxcl_sysinfo_query(sxcl_sysinfo *out, char *err, size_t err_len);

/** 物理内存总量(MB);拿不到返回 0。 */
uint64_t sxcl_sysinfo_total_mb(void);
/** 当前可用物理内存(MB);拿不到返回 0。 */
uint64_t sxcl_sysinfo_available_mb(void);
/** 逻辑核心数;拿不到返回 0。 */
int sxcl_sysinfo_cpu_logical(void);
/** 物理核心数;拿不到返回 0。 */
int sxcl_sysinfo_cpu_physical(void);

/** 推荐最大堆内存(MB)= 物理内存的 1/2,夹在 [1024, 8192];拿不到内存时给 2048(保守默认)。 */
uint64_t sxcl_sysinfo_recommended_heap_mb(void);

/** 从"物理内存总量 + 逻辑核心数"直接算推荐堆(纯函数,便于单测与"用户手动填了内存"的场景):
 *  total_mb 为 0 -> 2048;否则 clamp(total_mb / 2, 1024, 8192)。 */
uint64_t sxcl_sysinfo_heap_for(uint64_t total_mb);

/** 推荐下载/解压线程数 = clamp(逻辑核心数, 2, 16);核心数拿不到时给 4。 */
int sxcl_sysinfo_recommended_workers(void);

/** 同上但是纯函数:cores <= 0 -> 4。 */
int sxcl_sysinfo_workers_for(int cpu_logical);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_SYSINFO_H */
