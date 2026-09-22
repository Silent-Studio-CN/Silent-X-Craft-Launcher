/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_MANIFEST_H
#define SXCL_MANIFEST_H

#include <stdint.h>

#include "sxcl/engine.h"
#include "sxcl/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 版本清单(version_manifest_v2.json) ── */

typedef struct sxcl_version_entry {
    const char *id;
    const char *type;   /* release / snapshot / old_beta / old_alpha */
    const char *url;
    const char *sha1;
    int64_t size;
} sxcl_version_entry;

typedef struct sxcl_version_list sxcl_version_list;

/** 从已解析的清单 JSON 构建索引(不接管 doc,调用方仍需释放它)。 */
sxcl_version_list *sxcl_version_list_build(const sxcl_json *doc);
void sxcl_version_list_free(sxcl_version_list *list);

const char *sxcl_version_list_latest_release(const sxcl_version_list *list);
const char *sxcl_version_list_latest_snapshot(const sxcl_version_list *list);
size_t sxcl_version_list_count(const sxcl_version_list *list);
const sxcl_version_entry *sxcl_version_list_at(const sxcl_version_list *list, size_t index);
const sxcl_version_entry *sxcl_version_list_find(const sxcl_version_list *list, const char *id);

/* ── 下载计划 ── */

typedef struct sxcl_version_plan sxcl_version_plan;

/** 由版本 JSON 生成下载计划。
 *  game_dir 形如 "C:/Users/x/.minecraft" 或 "/home/x/.minecraft";
 *  version_id 用于定位 versions/<id>/<id>.jar。失败返回 NULL 并写 err。 */
sxcl_version_plan *sxcl_version_plan_build(const sxcl_json *version_json, const char *game_dir,
                                           const char *version_id, char *err, size_t err_len);

void sxcl_version_plan_free(sxcl_version_plan *plan);

/** 计划里的任务数(数组,不是链表;供引擎批量提交)。 */
size_t sxcl_version_plan_count(const sxcl_version_plan *plan);

/** 第 index 个任务(归计划所有,计划释放前有效)。
 *  契约:任务地址**稳定** —— 后续再往计划里追加条目(例如展开资源对象)不会搬动已取出的指针,
 *  所以可以把任务指针直接交给引擎,之后再继续追加。 */
sxcl_task *sxcl_version_plan_task(sxcl_version_plan *plan, size_t index);

/** 计划里所有任务的期望字节总数(进度显示用)。 */
int64_t sxcl_version_plan_total_bytes(const sxcl_version_plan *plan);

/* ── 把计划跑一遍(启动前"补全文件"的核心) ── */

#define SXCL_FETCH_OK        0   /**< 全部就绪(含"本来就都在",一个字节都没下) */
#define SXCL_FETCH_PARTIAL   1   /**< 有文件没下成(调用方通常只报告,不拦流程) */
#define SXCL_FETCH_CANCELLED 2   /**< 被回调叫停 */
#define SXCL_FETCH_ERR_ARG  (-1) /**< 参数不合法(典型:没有传输后端) */
#define SXCL_FETCH_ERR_IO   (-2) /**< 引擎建不起来 / 任务入不了队 */

/** 跑完一遍之后的统计 —— **全是结构化字段**,不要去比 error 里的中文文案
 *  (改一次文案就会静默把统计打回 0,这是引擎层已经踩过的坑)。 */
typedef struct sxcl_fetch_stats {
    int total;          /**< 计划里的文件数 */
    int downloaded;     /**< 真的下下来的 */
    int failed;         /**< 下失败的 */
    int skipped;        /**< 命中"已存在且校验通过"、一个字节都没下的 */
    int64_t bytes_done; /**< 这次真的写下去的字节数(命中的不算) */
} sxcl_fetch_stats;

/** 把计划里的所有任务交给下载引擎跑一遍。
 *
 *  为什么要有它:PCL 的"补全文件"就是"把需要哪些文件算出来交给下载器";而我们的下载器
 *  (engine.h)已经在安装流程里跑了很久 —— 镜像候选 / 多路重试 / 分片 / 限速 / 断点续传 /
 *  哈希缓存都在里面。启动前补全只是**同一套东西跑另一份计划**,不该再写第二份实现。
 *  "齐了一个字节都不下"这条性质由引擎的"已存在且校验通过"快路径保证,统计看 skipped。
 *
 *  opts 必须有 transport_factory(见 engine.h:75-92);opts->on_progress 会被**转发**
 *  (调用方靠它显示进度;从工作线程调用,实现里别做重活)。
 *  is_cancelled 可空:每落定一个文件问一次,非 0 → 叫停引擎并返回 SXCL_FETCH_CANCELLED。
 *  返回 SXCL_FETCH_*;人话原因写 err(可空);stats 可空。 */
int sxcl_version_plan_fetch(sxcl_version_plan *plan, const sxcl_engine_opts *opts,
                            int (*is_cancelled)(void *ud), void *cancel_ud, sxcl_fetch_stats *stats,
                            char *err, size_t err_len);

/* ── 资源对象(assets/objects) ── */

/** 官方资源对象 CDN:路径规则 <base>/<哈希前2位>/<哈希>。
 *  这个"文件名即内容哈希"的设计意味着校验不需要任何额外元数据。 */
#define SXCL_ASSET_OBJECTS_BASE "https://resources.download.minecraft.net"

/** 资源对象的任务优先级:排在客户端 jar 与依赖库之后。 */
#define SXCL_ASSET_OBJECTS_PRIORITY 20

/** 把资源索引里的 objects 追加进已有计划(按哈希去重;目标 <game_dir>/assets/objects/xx/hash)。
 *  base_url 为空则用官方 CDN;mirror_base 非空时作为第二候选路(镜像站)。
 *  返回新增条目数,<0 表示失败(此时 err 有原因)。 */
int sxcl_version_plan_add_asset_objects(sxcl_version_plan *plan, const sxcl_json *asset_index,
                                        const char *game_dir, const char *base_url,
                                        const char *mirror_base, char *err, size_t err_len);

/** 同上,但**自己选校验强度**(装的时候是强校验,启动前补全是弱校验):
 *  verify_hash != 0 -> 带 sha1(引擎会读文件算哈希;有哈希缓存时第二次很快);
 *  verify_hash == 0 -> **只比大小**(文件名的哈希就是内容哈希,想强校验随时可以再算)。
 *
 *  为什么要有这个开关(PCL 的口径):启动前要核对的是 **5000+ 个**资源文件、几百 MB,
 *  逐个算 SHA-1 在机械盘上要几十秒 —— PCL 启动时只查"存在 + 大小",哈希留给下载器/手动补全。
 *  我们默认按"只比大小"跑启动前那一遍(设置里可开到强校验),装的时候仍是强校验。 */
int sxcl_version_plan_add_asset_objects_ex(sxcl_version_plan *plan, const sxcl_json *asset_index,
                                           const char *game_dir, const char *base_url,
                                           const char *mirror_base, int verify_hash, char *err,
                                           size_t err_len);

/* ── 镜像(第二路来源)── */

/** BMCLAPI 镜像根。官方清单/版本 JSON/客户端 jar/依赖库/资源对象它都透传。 */
#define SXCL_MIRROR_BMCLAPI_BASE "https://bmclapi2.bangbang93.com"

/** 把官方 URL 映射成镜像 URL(纯字符串,不联网)。
 *  前缀映射(认不出的返回 -1,out 置空 —— 调用方就当"这条路没有"):
 *    launchermeta.mojang.com / piston-meta.mojang.com / piston-data.mojang.com -> <base>
 *    libraries.minecraft.net                                                  -> <base>/maven
 *    resources.download.minecraft.net                                         -> <base>/assets
 *  mirror_base 为空则用 SXCL_MIRROR_BMCLAPI_BASE。
 *  为什么要有这个:清单与每一个下载文件都要有"官方不通时走镜像"的第二条路,
 *  否则镜像站配了也只是摆设(用户报过"设了镜像还是下不动")。 */
int sxcl_manifest_mirror_url(const char *url, const char *mirror_base, char *out, size_t out_len);

/** 给计划里**还没有第二候选**的任务补一条镜像 URL(用 sxcl_manifest_mirror_url 映射)。
 *  官方不通时引擎会自己换到这条路。返回补上的条数,<0 表示失败(err 有原因)。
 *  理由:清单里每个文件的 URL 都是官方域名;用户配了镜像却只作用于清单本身,等于没生效
 *  (用户报过"设了镜像还是下不动")。plan 会接管这些字符串的生命周期(与其它任务字段一致)。 */
int sxcl_version_plan_add_mirror(sxcl_version_plan *plan, const char *mirror_base, char *err,
                                 size_t err_len);

/** 把每个任务的候选顺序改成"镜像在前、官方在后"(只有一条候选的任务不动)。
 *  为什么要:设置里的下载源以前只作用于清单;download.source=bmclapi 时游戏文件(版本 JSON/
 *  客户端 jar/依赖库/资源对象)仍然优先走官方,镜像要等官方失败才用得上。
 *  返回被调换的任务数(被调换 = 本来就有第二条候选),<0 表示 plan 为空(参数错误)。
 *  **只应调用一次**:它只做一次交换、不记状态,再调一次就把顺序换回"官方在前"了。 */
int sxcl_version_plan_prefer_mirror(sxcl_version_plan *plan);

/** 本机平台名,与 Mojang rules 里的 os.name 一致:"windows" / "linux" / "osx"。 */
const char *sxcl_platform_os_name(void);

/** 本机架构名,与 rules 里的 os.arch 一致:"x86" / "x64" / "arm64"。 */
const char *sxcl_platform_arch_name(void);

/** Mojang rules 求值(rules 缺失 = 允许)。 */
int sxcl_rules_allow(const sxcl_json_value *rules_value, const char *os_name, const char *arch_name);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_MANIFEST_H */
