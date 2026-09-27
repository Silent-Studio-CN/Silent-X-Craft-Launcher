/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 模组中文名表（随包 + 云端可更新）—— 数据来源与许可见 docs/29，云端契约见 docs/28。
//
// 这件事的**唯一口径**（用户 2026-09-27 点名）：界面上"某些模组有中文名"不是运行时翻译，
// 是**一张随包带的本地表**（PCL 的做法：PCLCS/Resource/WikiEntries.txt 一次读进内存，
// ResourceProject.vb 里 "有就用中文名、没有就用原名"）。我们照这个形态做，但**不抄 PCL 的表**
// —— 那张表的许可证说不清；我们这份表的来源、许可与生成命令写在 assets/data/modnames.NOTICE.md。
//
// 三条铁律：
//   1. **绝不猜**：查不到就是空（返回 NULL / 一个字都不写），**绝不**用拼音、机翻、"看起来像"
//      的东西冒充中文名；
//   2. **不崩**：空表、坏行、缺列、超长行、半个文件、非 UTF-8 都不许崩 —— 坏行**数出来**
//      （sxcl_modnames_bad_lines），不静默；
//   3. **查表 O(1)**：建表时一次性建哈希（开放寻址 + 线性探测），键是
//      slug / cf_id / cf_slug / modid / 项目名（中英文都能按名字查）。
//      sxcl_modnames_last_probes() 就是这条的证据：大表上命中/未命中都只走个位数个槽位。
//
// 这一层**只做纯逻辑**：解析、查找、摘要校验、原子替换、按顺序挑"缓存还是随包"。
// 网络那一跳由调用方给一个 sxcl_transport（核心库里现成的 sxcl_http_get_text），
// 所以不联网也能把整条更新路径单测完（假 transport 喂真文本）。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sxcl/net.h" /* sxcl_transport：更新路径要用它去取云端那张表 */

#ifdef __cplusplus
extern "C" {
#endif

/* 返回码（负数为错，与 fs/net/loader 的约定一致） */
#define SXCL_MODNAMES_OK              0
#define SXCL_MODNAMES_ERR_ARG       (-1)  /* 参数不合法（空指针/空串/缓冲不够） */
#define SXCL_MODNAMES_ERR_FORMAT    (-2)  /* 不是我们的表（缺表头魔数） */
#define SXCL_MODNAMES_ERR_IO        (-3)  /* 文件读不出来 */
#define SXCL_MODNAMES_ERR_NOMEM     (-4)  /* 内存不足 */
#define SXCL_MODNAMES_ERR_REJECTED  (-5)  /* 云端那份被拒（摘要/条数/版本不过关），缓存**原样不动** */

/* 表头魔数：第一行必须是 "# sxcl-modnames<TAB>1"。云端与随包用同一个格式。 */
#define SXCL_MODNAMES_MAGIC   "sxcl-modnames"
#define SXCL_MODNAMES_FORMAT  1

#define SXCL_MODNAMES_ERR_MAX   256
#define SXCL_MODNAMES_NOTE_MAX  256
#define SXCL_MODNAMES_SHA256_MAX 65  /* 64 位十六进制 + NUL */
#define SXCL_MODNAMES_TEXT_MAX   192 /* 表头里 source/license 这类单行文本 */
#define SXCL_MODNAMES_LINE_MAX   8192 /* 单行上限；超了当坏行（不静默） */
#define SXCL_MODNAMES_MAX_ENTRIES 400000u /* 一次最多认这么多条（防一个坏文件把内存吃光） */

/** 一行的七个字段（全部指向表内部的字符串池，**表活着就一直有效**，不要 free）。
 *  任何一格都可能是空串（缺列/上游没给）。中文名 name_zh 非空才算一条有效记录。 */
typedef struct sxcl_modname_entry {
    const char *slug;    /**< Modrinth slug（或上游给的工程 slug） */
    const char *cf_id;   /**< CurseForge 的**数字** id（十进制文本） */
    const char *cf_slug; /**< CurseForge 的 slug（不是数字 id 的那个键） */
    const char *modid;   /**< Minecraft 的 mod id（fabric.mod.json / mods.toml 里那个） */
    const char *name_zh; /**< 中文名（**唯一真正要用的字段**） */
    const char *name_en; /**< 原名（用于按项目名查） */
    const char *source;  /**< 这一条的出处键（如 "cfpa"/"hmcl"），可空 */
} sxcl_modname_entry;

/** 表头里声明的元数据（写表的人填；读表的人用来判断"这份能不能用"）。 */
typedef struct sxcl_modname_meta {
    int format;                     /**< 格式版本（缺省 1） */
    long version;                   /**< 版本号（单调递增；0 = 表里没写） */
    size_t count;                   /**< 表头声明的**数据行条数**（0 = 没写） */
    char sha256[SXCL_MODNAMES_SHA256_MAX]; /**< 表头声明的正文摘要（空 = 没写） */
    char source[SXCL_MODNAMES_TEXT_MAX];
    char license[SXCL_MODNAMES_TEXT_MAX];
    char generated[SXCL_MODNAMES_TEXT_MAX];
    size_t entries_lines;           /**< 解析时真正读到的数据行条数（含被跳过的坏行） */
} sxcl_modname_meta;

typedef struct sxcl_modnames sxcl_modnames; /* 不透明：内部是字符串池 + 哈希槽 */

/* ── 建表 ── */

/** 解析一段 UTF-8 文本。空文本 = **合法的空表**（0 条，查什么都返回 NULL），不是错误。
 *  坏行（列不够 / 中文名为空 / 没有任何可查键 / 超长）**跳过并计数**，不影响其它行。
 *  成功返回 SXCL_MODNAMES_OK 并写出 *out（用完 sxcl_modnames_free）；
 *  不是我们的表返回 ERR_FORMAT；out 为 NULL 之类返回 ERR_ARG。 */
int sxcl_modnames_parse(const char *text, size_t len, sxcl_modnames **out, char *err, size_t err_len);

/** 从磁盘读一份表（UTF-8）。读不到返回 ERR_IO。 */
int sxcl_modnames_load_file(const char *path, sxcl_modnames **out, char *err, size_t err_len);

/** 释放。NULL 安全。 */
void sxcl_modnames_free(sxcl_modnames *table);

/** 只读访问器。table 为 NULL 时都返回"空"（0 / NULL），不崩。 */
const sxcl_modname_meta *sxcl_modnames_meta(const sxcl_modnames *table);
size_t sxcl_modnames_count(const sxcl_modnames *table);      /**< 有效条数 */
size_t sxcl_modnames_bad_lines(const sxcl_modnames *table);  /**< 跳过的坏行数 */
const sxcl_modname_entry *sxcl_modnames_entry(const sxcl_modnames *table, size_t index);

/** 表头声明的 sha256 / 条数与正文对不对得上（0/1）。表头没写这两样时按"对得上"算（1）。
 *  **这是"下载回来的那张表能不能信"的判据**，不是解析错误。 */
int sxcl_modnames_self_consistent(const sxcl_modnames *table);

/** 正文（表头之后第一个数据字节到结尾）的 SHA-256，十六进制小写。写表与校验用**同一套**规则，
 *  这样"表头里的 sha256"永远不会自指。成功返回 SXCL_MODNAMES_OK。 */
int sxcl_modnames_body_sha256(const char *text, size_t len, char *out, size_t out_len);

/* ── 查表（命中返回**表内部**的中文名；查不到返回 NULL —— 绝不给替代品） ── */

const char *sxcl_modnames_lookup_slug(const sxcl_modnames *table, const char *slug);
const char *sxcl_modnames_lookup_cf_id(const sxcl_modnames *table, const char *cf_id);
const char *sxcl_modnames_lookup_cf_slug(const sxcl_modnames *table, const char *cf_slug);
const char *sxcl_modnames_lookup_modid(const sxcl_modnames *table, const char *modid);
/** 按项目名查。英文原名与中文名都建了索引：传 "Sodium" 或 "钠" 都能命中。
 *  匹配是**规范化后整串相等**（大小写、空格、- _ . ' : 等标点不参与比较），**不做模糊匹配**。 */
const char *sxcl_modnames_lookup_name(const sxcl_modnames *table, const char *name);

/** 三键依次查：slug -> cf_id -> name（空的键直接跳过）。全都不中就返回 NULL。 */
const char *sxcl_modnames_lookup(const sxcl_modnames *table, const char *slug, const char *cf_id,
                                 const char *name);

/** 给一条搜索结果填中文名：命中写进 out（NUL 结尾）返回 1；
 *  没命中**一个字都不写**（out 保持调用方传进来的原样，通常就是原名）返回 0；参数不合法返回 -1。 */
int sxcl_modnames_apply(const sxcl_modnames *table, const char *slug, const char *cf_id,
                        const char *name, char *out, size_t out_len);

/** 上一次查询走过的哈希槽位数（诊断/单测用：**O(1) 的证据** —— 20 万条的表上仍是 1~3）。 */
unsigned sxcl_modnames_last_probes(const sxcl_modnames *table);
/** 哈希槽总数（单测断言"槽数 >= 2×条数"，即负载因子 <= 0.5）。 */
size_t sxcl_modnames_bucket_count(const sxcl_modnames *table);

/* ── 随包那份 + 云端缓存（路径口径） ── */

/** 随包表路径：<asset_root>/data/modnames.tsv。
 *  环境变量 SXCL_MODNAMES_FILE 指到别处时**优先用它**（取证/验收用，和 SXCL_ICON_DIR 同一套口径）。
 *  返回 OK；参数错返回 ERR_ARG。 */
int sxcl_modnames_bundled_path(const char *asset_root, char *out, size_t out_len);

/** 云端缓存的落点：<cache_dir>/meta/modnames.tsv（cache_dir 给的是我们自己的数据目录）。 */
int sxcl_modnames_cache_path(const char *cache_dir, char *out, size_t out_len);

/** 云端取表的 URL：<base>/meta/modnames.tsv（base 末尾多余的斜杠会被去掉）。
 *  base 为空返回 ERR_ARG —— 我们**不编一个默认域名出来**。 */
int sxcl_modnames_remote_url(const char *base, char *out, size_t out_len);

/** 把一段表文本原子落到缓存（先写 .part，再改名覆盖）。
 *  out_path 非空时写出最终路径。**失败时缓存里那份原样不动。** */
int sxcl_modnames_cache_write(const char *cache_dir, const char *text, size_t len, char *out_path,
                              size_t out_path_len, char *err, size_t err_len);

/* ── 组装：先缓存（校验过的新版）再随包（永远在） ── */

typedef struct sxcl_modnames_source {
    int from_cache;     /**< 1 = 用的是缓存里那份（云端更新过的） */
    int from_bundled;   /**< 1 = 用的是随包那份 */
    int cache_rejected; /**< 1 = 缓存那份在、但被否掉了（坏 / 比随包旧），已忽略 */
    int empty;          /**< 1 = 两份都没有 -> 空表（查什么都返回 NULL，不是错误） */
    char note[SXCL_MODNAMES_NOTE_MAX]; /**< 人话：这次用的是哪份、为什么 */
} sxcl_modnames_source;

/** 按顺序挑一份表：缓存（sha256/条数对得上，且 version 比随包那份**更新**）-> 随包。
 *  两份都没有 = 空表（返回 OK，*out 是一张 0 条的表，source->empty = 1）——**绝不报失败阻塞启动**。
 *  bundled_path 可空（那就只有缓存这条路）。 */
int sxcl_modnames_load_cached(const char *bundled_path, const char *cache_dir, sxcl_modnames **out,
                              sxcl_modnames_source *source, char *err, size_t err_len);

/* ── 云端更新 ── */

typedef struct sxcl_modnames_update {
    int updated;   /**< 1 = 换上了云端那份（缓存已原子替换）；0 = 没换（原因在 note） */
    int status;    /**< HTTP 状态码；0 = 连响应都没拿到 */
    long version;  /**< 换上的版本号（没换时为 0） */
    size_t count;  /**< 换上的条数（没换时为 0） */
    char sha256[SXCL_MODNAMES_SHA256_MAX];
    char note[SXCL_MODNAMES_NOTE_MAX];
} sxcl_modnames_update;

/** 从云端那张表（基址 + /meta/modnames.tsv）更新一份到缓存。
 *
 *  接受条件（三条全过才换，**缺一条都不换**）：
 *    1) 是合法的表（表头魔数在）；
 *    2) 表头声明的 sha256 / 条数与正文对得上（没写这两样 = 直接拒 —— 云端那张表**必须**带）；
 *    3) version > current 的 version（current 为空或 0 时只要 version >= 1 就收）。
 *
 *  **失败就是失败**：缓存里那份原样不动，返回值如实报出来，绝不半截替换、绝不拿随包那份去冒充。
 *  tr 为空 = 这次不联网（updated=0，note 写清"没有传输后端"）。
 *  返回 SXCL_MODNAMES_OK（含"没更新"这种正常结果）或负数错误码。 */
int sxcl_modnames_update_from_cloud(sxcl_transport *tr, const char *base, const char *cache_dir,
                                    const sxcl_modnames *current, sxcl_modnames_update *result,
                                    char *err, size_t err_len);

/** 更新可选的第一步：只取云端那份的**表头**（Range: bytes=0-4095）看 version 有没有前进，
 *  省一次整表下载。拿不到 / 不是我们的表 -> 返回负数，调用方就当"不知道"，照旧走整表那条路。
 *  这是可选优化，不是必经之路（云端不一定兑现 Range）。 */
int sxcl_modnames_probe_remote(sxcl_transport *tr, const char *base, const sxcl_modnames *current,
                               long *out_version, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
