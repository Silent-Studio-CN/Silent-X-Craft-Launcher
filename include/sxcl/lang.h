/* SXCL-C 国际化(i18n)查表 —— 纯 C11,不依赖 Qt。
 *
 * 对应参考实现:
 *   * Python 版 src/core/lang.py(键值表 + 语言文件自动下载/内置回退);
 *   * 语言文件 config/lang/<code>.lang(格式:key = value,# 注释,空行忽略)。
 *
 * 三条硬约束(逐条兑现,不许打折):
 *   1) **格式逐字段兼容** Python 版:.lang 文件按"整行 strip -> 第一个 '=' 切分 ->
 *      键/值各自 strip"解析,与 lang.py:137-150 的 _parse 一字不差 —— 直接拿 Python 版
 *      那份 config/lang/en-us.lang 就能用(测试 tests/lang_test.c 拿真文件逐条对拍)。
 *   2) **找不到的键回落到中文**:查表顺序 = 当前语言(磁盘 -> 内置) -> 简体中文(磁盘 -> 内置)
 *      -> 调用方给的 def。所以英文包少一条也不会显示成空串。
 *   3) **不联网也能用**:中文/英文两份默认文案编译进库(见 lang_table.inc,由 Python 版
 *      的 .lang 生成),磁盘上的 .lang 只是"覆盖/翻译更新"的入口。Python 版首次运行要
 *      去 GitHub 拉语言包(lang.py:117-135);C 版不联网也有完整两份,拉取是纯可选。
 *
 * 语言代码:归一化到小写短横线形式("zh-CN"/"zh_CN"/"zh" -> "zh-cn"),认不出的回落到
 *   zh-cn —— 与 lang.py:164-165(`if lang_code not in _SUPPORTED: lang_code = "zh-cn"`)同语义。
 *
 * 线程契约:句柄本身**不加锁**(与 settings.c 一个口径)。另外提供一个进程级默认表
 *   (sxcl_lang_set_default / sxcl_lang_tr):只在启动与"切语言"时由主线程写,查询只读。
 *   UI 线程之外要用,请自己拿句柄或用锁。
 */
#ifndef SXCL_LANG_H
#define SXCL_LANG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 尺寸上限(缓冲长度都含结尾 NUL) ── */
#define SXCL_LANG_CODE_MAX 16   /**< 语言代码:"zh-cn" / "en-us" */
#define SXCL_LANG_KEY_MAX 96    /**< 键名上限(超长行整条丢弃,不截断成半个键) */
#define SXCL_LANG_TEXT_MAX 512  /**< 单条文案上限(超长截断) */
#define SXCL_LANG_ERR_MAX 256   /**< 人话错误缓冲 */

/* 返回码(负数;0 = 成功) */
#define SXCL_LANG_OK            0
#define SXCL_LANG_ERR_ARG     (-1)  /**< 参数不合法 */
#define SXCL_LANG_ERR_IO      (-2)  /**< 文件打不开/读不出 */
#define SXCL_LANG_ERR_NOMEM   (-3)  /**< 内存不足 */
#define SXCL_LANG_ERR_SPACE   (-4)  /**< 输出缓冲不够 */

/** 语言表句柄。 */
typedef struct sxcl_lang sxcl_lang;

/* ── 语言清单(与 Python 版 _SUPPORTED 一致) ── */

/** 支持的语言数(当前 2)。 */
size_t sxcl_lang_supported_count(void);
/** 第 index 个语言的规范代码("zh-cn"/"en-us");越界返回 NULL。 */
const char *sxcl_lang_supported_code(size_t index);
/** 第 index 个语言的显示名("简体中文"/"English",取自 launcher_config.py:55-60);越界返回 NULL。 */
const char *sxcl_lang_supported_name(size_t index);
/** 这个代码认不认(大小写/下划线都归一化后再比)。1 = 认。 */
int sxcl_lang_is_supported(const char *code);

/** 归一化语言代码:写入 out(含 NUL)。认得的一律写成规范形式,认不出的写 "zh-cn"。
 *  out 不够返回 SXCL_LANG_ERR_SPACE(*out 仍是 NUL 结尾的空串/截断值)。 */
int sxcl_lang_normalize_code(const char *code, char *out, size_t out_len);

/* ── 打开 / 加载 / 查表 ── */

/** 打开语言表。
 *  - code 归一化后作为当前语言;dir 非空 = 只在它下面找 <code>.lang;
 *    dir 为空 = 按 sxcl_lang_default_dirs() 的顺序找第一个存在的 <code>.lang。
 *  - 无论磁盘上有没有,内置默认都先进表(磁盘文件逐键覆盖)。
 *  - 找不到任何文件**不算失败**(内置默认兜底),err 里会写一句说明,返回非 NULL。
 *  - 失败(NULL)只在参数非法或内存不足。 */
sxcl_lang *sxcl_lang_open(const char *code, const char *dir, char *err, size_t err_len);

/** 释放句柄(允许 NULL)。 */
void sxcl_lang_free(sxcl_lang *lang);

/** 当前语言代码(规范形式;lang 为空返回 "zh-cn")。 */
const char *sxcl_lang_code(const sxcl_lang *lang);

/** 查一条。顺序:当前语言 -> 简体中文兜底 -> def(可空)。
 *  返回的指针归句柄所有,句柄释放/重载后失效(要留就自己拷贝)。
 *  键为空或 lang 为空时返回 def。 */
const char *sxcl_lang_get(const sxcl_lang *lang, const char *key, const char *def);

/** 有没有这条键(含中文兜底);1 = 有。 */
int sxcl_lang_has(const sxcl_lang *lang, const char *key);

/** 取一条并做 {占位符} 替换(UI 里 "共 {total} 个版本" 这类文案)。
 *  names/values 是等长数组(count 个);未知占位符**原样保留**(与 Python str.format 的
 *  KeyError 不同,这里选择"不炸"),单个占位符超长直接按字面写出。
 *  写不下返回 SXCL_LANG_ERR_SPACE(保证 NUL 结尾,内容是截断的)。 */
int sxcl_lang_format(const sxcl_lang *lang, const char *key, const char *def,
                     const char *const *names, const char *const *values, size_t count,
                     char *out, size_t out_len);

/** 从文本加载(后加载的键覆盖先加载的;不存在的键追加)。返回写入条数,负数 = 错误码。 */
int sxcl_lang_load_text(sxcl_lang *lang, const char *text, size_t len, char *err, size_t err_len);

/** 从文件加载(UTF-8;Windows 走 UTF-8 路径转换,中文路径可用)。返回写入条数,负数 = 错误码。 */
int sxcl_lang_load_file(sxcl_lang *lang, const char *path, char *err, size_t err_len);

/** 当前语言的条目数(不含中文兜底)。 */
size_t sxcl_lang_count(const sxcl_lang *lang);
/** 第 index 条的键 / 文案(插入顺序);越界返回 NULL。 */
const char *sxcl_lang_key_at(const sxcl_lang *lang, size_t index);
const char *sxcl_lang_text_at(const sxcl_lang *lang, size_t index);

/* ── 默认搜索目录 / 进程级默认表 ── */

/** 语言文件搜索目录(按优先级)。写法与 settings.c 的平台分支一致:
 *  1) $SXCL_LANG_DIR(显式指定)
 *  2) <配置目录>/lang(用户把语言包丢这儿;配置目录见 sxcl_settings_default_dir)
 *  3) <exe 所在目录>/lang(便携版)
 *  写进 out(二维数组,每行 SXCL_LANG_PATH_MAX 字节),返回条数(最多 cap 条)。 */
#define SXCL_LANG_PATH_MAX 512
size_t sxcl_lang_default_dirs(char (*out)[SXCL_LANG_PATH_MAX], size_t cap);

/** 设置进程级默认表(启动时一次 / 切语言时一次)。
 *  code 归一化;dir 可空(按搜索目录找)。成功返回 0;失败返回负错误码,此时旧的默认表
 *  **保持不动**(不会出现"切一半"的态)。 */
int sxcl_lang_set_default(const char *code, const char *dir, char *err, size_t err_len);
/** 进程级默认表;没设过返回 NULL。 */
const sxcl_lang *sxcl_lang_default(void);
/** = sxcl_lang_get(sxcl_lang_default(), key, def);没设过默认表时返回 def。 */
const char *sxcl_lang_tr(const char *key, const char *def);
/** 释放进程级默认表(测试收尾用;正式程序不用调)。 */
void sxcl_lang_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_LANG_H */
