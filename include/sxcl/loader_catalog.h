/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_LOADER_CATALOG_H
#define SXCL_LOADER_CATALOG_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/http.h"
#include "sxcl/loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 返回码(与 loader.h 同一套口径,负数 = 失败) ── */
#define SXCL_CATALOG_OK          0
#define SXCL_CATALOG_ERR_ARG    (-1)   /* 参数不合法 */
#define SXCL_CATALOG_ERR_FORMAT (-3)   /* 文本不是这个来源的格式(一个条目都认不出来) */
#define SXCL_CATALOG_ERR_NOMEM  (-4)   /* 内存不足 */
#define SXCL_CATALOG_ERR_SPACE  (-5)   /* 输出缓冲不够 */
#define SXCL_CATALOG_ERR_NET    (-2)   /* 取文本失败(只有 fetch 会返回它) */

/* ── 尺寸上限(定长字段,超长截断;版本串本身不会这么长) ── */
#define SXCL_CATALOG_VERSION_MAX 80
#define SXCL_CATALOG_LOADER_MAX  48
#define SXCL_CATALOG_MC_MAX      24
#define SXCL_CATALOG_DATE_MAX    24
#define SXCL_CATALOG_FORGE_MAX   32
#define SXCL_CATALOG_DISPLAY_MAX 96
#define SXCL_CATALOG_FILE_MAX    96
#define SXCL_CATALOG_SOURCE_MAX  64

/* ── 最小 XML 扫描器 ──────────────────────────────────────────────
 * 只做 maven-metadata.xml 与 OptiFine 网页行需要的那点事:认出开始/结束标签、
 * 把标签原文给出来(属性从原文里扫)、把标签之后的文本给出来。
 * 明确**不是**通用 XML 解析器:不建树、不做命名空间、不校验良构。
 * 坏 XML(标签没闭合、引号不配对、半截文件)一律"停下来",绝不越界、绝不崩。
 */

typedef struct sxcl_xml_cursor {
    const char *p;     /* 当前位置(调用方不用管) */
    const char *end;
} sxcl_xml_cursor;

typedef struct sxcl_xml_tag {
    char name[32];        /* 元素名(原样大小写) */
    int is_end;           /* 1 = </name> */
    int self_closing;     /* 1 = <name/> */
    const char *raw;      /* 标签原文,从 '<' 到 '>'(含);属性从它里面扫 */
    size_t raw_len;
    const char *text;     /* 标签之后、下一个 '<' 之前的文本(可能为空;含空白与实体原样) */
    size_t text_len;
} sxcl_xml_tag;

/** 初始化游标(不复制文本,text 必须活到扫描结束)。 */
void sxcl_xml_init(sxcl_xml_cursor *cur, const char *text, size_t len);

/** 取下一个标签。返回 1 = 有,0 = 到末尾/坏到不能再走。注释、<?xml?>、<!DOCTYPE>、CDATA 会被跳过。 */
int sxcl_xml_next(sxcl_xml_cursor *cur, sxcl_xml_tag *out);

/** 标签名比较(大小写不敏感;HTML 那边 <TD> 也认)。 */
int sxcl_xml_tag_is(const sxcl_xml_tag *tag, const char *name);

/** 取标签属性(单/双引号都认,无引号也认)。找到返回 1 并写 out(始终 NUL 结尾,超长截断)。 */
int sxcl_xml_attr(const sxcl_xml_tag *tag, const char *name, char *out, size_t out_len);

/** 文本归一:去首尾空白 + 解 &amp; &lt; &gt; &quot; &apos; &#NN; &#xHH;。
 *  返回 1 = 完整写下;0 = 超长被截断(out 仍是合法 NUL 结尾串,不会崩)。 */
int sxcl_xml_text_plain(const char *text, size_t len, char *out, size_t out_len);

/* ── 一个可用版本 ── */

typedef struct sxcl_catalog_entry {
    char version[SXCL_CATALOG_VERSION_MAX];  /**< 版本串(源里怎么写就怎么存,如 "1.20.1-47.2.0") */
    char loader[SXCL_CATALOG_LOADER_MAX];    /**< 加载器自己的版本("47.2.0"/"21.1.72"/"0.19.5") */
    char mc[SXCL_CATALOG_MC_MAX];            /**< 它要求的 MC 版本("1.20.1";认不出来是空串) */
    char released[SXCL_CATALOG_DATE_MAX];    /**< 发布时间(能拿到才给,ISO "2026-02-05";否则空串) */
    char forge[SXCL_CATALOG_FORGE_MAX];      /**< OptiFine 专用:配套 Forge 版本("47.2.18");其它为空 */
    char display[SXCL_CATALOG_DISPLAY_MAX];  /**< 给人看的一行(OptiFine 是 "OptiFine HD U J9";其它=version) */
    char file[SXCL_CATALOG_FILE_MAX];        /**< 安装器/产物文件名(OptiFine 的 jar;别的为空) */
    int is_latest;        /**< 1 = 这个源标的最新(<latest> / 源里没有标记时排序后第一条非 Beta) */
    int is_recommended;   /**< 1 = 源标的推荐(<release> / Fabric 的 stable=true / 同上兜底) */
    int is_beta;          /**< 1 = Beta/预览版(版本串带 beta/alpha/rc/snapshot/预览前缀) */
} sxcl_catalog_entry;

/** 解析出来的"文档级"信息(界面上不一定用,排查和测试要用)。 */
typedef struct sxcl_catalog_doc_info {
    char latest[SXCL_CATALOG_VERSION_MAX];       /**< maven 的 <latest> 原样 */
    char release[SXCL_CATALOG_VERSION_MAX];      /**< maven 的 <release> 原样 */
    char last_updated[SXCL_CATALOG_DATE_MAX];    /**< maven 的 <lastUpdated>,格式化成 "2026-08-27 04:59:25" */
    char group[SXCL_CATALOG_SOURCE_MAX];         /**< maven 的 <groupId> */
    char artifact[SXCL_CATALOG_SOURCE_MAX];      /**< maven 的 <artifactId> */
    size_t parsed;                               /**< 源里一共认出多少条(过滤前) */
    size_t matched;                              /**< 过滤后还剩多少条 */
} sxcl_catalog_doc_info;

/* ── 解析(纯函数:调用方喂文本,不联网) ──
 * 惯例是"先问条数再填表":out 为空(out_cap=0)时只返回**条数**;
 * 否则最多填 out_cap 条,返回**条数**(可能大于 out_cap,多余的就是没填)。
 * mc:要查的 MC 版本。
 *   * 源里自带 MC 版本的(Forge/NeoForge/OptiFine)maven 的版本串就是权威,
 *     解析出来的 mc 与它不同的条目**在解析过程中就被丢掉**(NeoForge 的 maven 有 2000+ 条,
 *     全填进数组太浪费;这样 out_cap=64 也够);
 *   * Fabric/Quilt 的接口本身就是按 MC 查的,返回体里不写 MC,就用这个参数填进 entry.mc;
 *   * 传 NULL/空串 = 不过滤、全都收。
 */
size_t sxcl_catalog_parse_maven_xml(const char *xml, size_t len, sxcl_loader_kind kind,
                                    const char *mc, sxcl_catalog_doc_info *info,
                                    sxcl_catalog_entry *out, size_t out_cap);

size_t sxcl_catalog_parse_fabric_json(const char *json, size_t len, const char *mc,
                                      sxcl_catalog_entry *out, size_t out_cap);

size_t sxcl_catalog_parse_quilt_json(const char *json, size_t len, const char *mc,
                                     sxcl_catalog_entry *out, size_t out_cap);

/** BMCLAPI 的 OptiFine 列表(JSON,带 forge 字段)。 */
size_t sxcl_catalog_parse_optifine_json(const char *json, size_t len, const char *mc,
                                        sxcl_catalog_entry *out, size_t out_cap);

/** OptiFine 网页(optifine.net/downloads)里的下载行;也吃镜像给的
 *  <downloads><download mcversion=… type=… patch=… forge=… /> 这类 XML 形态。 */
size_t sxcl_catalog_parse_optifine_html(const char *html, size_t len, const char *mc,
                                        sxcl_catalog_entry *out, size_t out_cap);

/** 按加载器挑解析器(OptiFine 会自己看返回体是 JSON 还是网页)。 */
size_t sxcl_catalog_parse(sxcl_loader_kind kind, const char *text, size_t len, const char *mc,
                          sxcl_catalog_doc_info *info, sxcl_catalog_entry *out, size_t out_cap);

/* ── 过滤 / 排序 / 标记(纯逻辑,可以单独测) ── */

/** 只留下 mc 与 wanted 相同的条目(wanted 为空 = 全都留下)。返回写了几条。 */
size_t sxcl_catalog_filter_mc(const sxcl_catalog_entry *in, size_t count, const char *wanted,
                              sxcl_catalog_entry *out, size_t out_cap);

/** 新的排前面(按版本串里的数字段比大小,与 Python 版 _version_key 一致);
 *  数字一样时非 Beta 在前,再一样时按字符串倒序。就地排序。 */
void sxcl_catalog_sort_desc(sxcl_catalog_entry *items, size_t count);

/** 补 latest/recommended 标记:源里没给标记时,排序后**第一条非 Beta** 顶上(界面默认选中它)。 */
void sxcl_catalog_mark_flags(sxcl_catalog_entry *items, size_t count);

/** 一把梭:解析 -> 按 mc 过滤 -> 排序 -> 补标记。返回**过滤后**的条数。
 *  这是给界面用的主入口;想分步(比如换排序口径)就用上面那几个。 */
size_t sxcl_catalog_prepare(sxcl_loader_kind kind, const char *text, size_t len, const char *mc,
                            sxcl_catalog_doc_info *info, sxcl_catalog_entry *out, size_t out_cap);

/* ── 地址(纯字符串拼接,不联网) ── */

typedef enum sxcl_catalog_source {
    SXCL_CATALOG_SRC_OFFICIAL = 0,   /**< 官方源 */
    SXCL_CATALOG_SRC_MIRROR          /**< BMCLAPI 镜像(实测可用的那几个;OptiFine 只有它有接口) */
} sxcl_catalog_source;

/** 这个加载器从这个源拿到的是什么格式(决定用哪个解析器)。 */
typedef enum sxcl_catalog_format {
    SXCL_CATALOG_FMT_NONE = 0,
    SXCL_CATALOG_FMT_MAVEN_XML,      /**< maven-metadata.xml */
    SXCL_CATALOG_FMT_META_JSON,      /**< Fabric/Quilt 的 meta JSON 数组 */
    SXCL_CATALOG_FMT_OPTIFINE_JSON,  /**< BMCLAPI 的 OptiFine JSON */
    SXCL_CATALOG_FMT_OPTIFINE_HTML   /**< optifine.net/downloads 的网页表格 */
} sxcl_catalog_format;

sxcl_catalog_format sxcl_catalog_format_of(sxcl_loader_kind kind, sxcl_catalog_source source);

/** 拼 URL。mc 为空的加载器(Forge/NeoForge/OptiFine 官方源)不需要它。
 *  返回 SXCL_CATALOG_OK 并写 out(始终 NUL 结尾);缓冲不够返回 ERR_SPACE。 */
int sxcl_catalog_url(sxcl_loader_kind kind, sxcl_catalog_source source, const char *mc,
                     char *out, size_t out_len);

/* ── 一步到位:取文本 + 解析 + 过滤 + 排序 + 标记 ── */

/** 内部用 sxcl_http_get_text();失败返回负数,err 里是人话(沿用 http.h 的措辞)。
 *  out 为空时只做"取文本 + 数条数"(不常这么用,省一次解析)。 */
int sxcl_catalog_fetch(sxcl_transport *tr, sxcl_loader_kind kind, sxcl_catalog_source source,
                       const char *mc, const char *const *headers, sxcl_catalog_doc_info *info,
                       sxcl_catalog_entry *out, size_t out_cap, size_t *count,
                       char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_LOADER_CATALOG_H */
