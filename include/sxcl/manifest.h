/* SXCL-C 元数据层 —— 把 Mojang 的清单/版本 JSON 翻译成"可下载可校验的任务清单"。
 *
 * 只做启动器真正需要的事:
 *   - 版本清单:版本号 -> 版本 JSON 的 url/sha1/size,以及 latest.release
 *   - 版本 JSON:客户端 jar、资源索引、依赖库(含 rules 平台过滤与 natives 分类器)
 *   - 产出 sxcl_task 数组,可直接丢给引擎;每个 Mojang 给了 sha1 的条目都带强校验
 *
 * 不做:资源对象(assets/objects)的展开 —— 需要先下 assetIndex 再解析,属于下一层。
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
