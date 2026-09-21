/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_OPTIONS_H
#define SXCL_OPTIONS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sxcl_options sxcl_options;

/** 读取文件。文件不存在视为空表(返回可用句柄,不算失败);其它 IO 错误返回 NULL。 */
sxcl_options *sxcl_options_load(const char *path);

/** 原子写回(tmp + 改名)。返回 0 成功。 */
int sxcl_options_save(const sxcl_options *options, const char *path);

/** 取键值;不存在返回 NULL。bare 行(无冒号)返回空串。 */
const char *sxcl_options_get(const sxcl_options *options, const char *key);

/** 设值(已存在则改,不存在则追加到末尾)。返回 0 成功。 */
int sxcl_options_set(sxcl_options *options, const char *key, const char *value);

/** 删除键(不存在不算错误)。返回 0 成功。 */
int sxcl_options_remove(sxcl_options *options, const char *key);

/** 条目数(含 bare 行)。 */
size_t sxcl_options_count(const sxcl_options *options);

/** 按顺序遍历:第 index 条的键与值;越界返回 NULL。bare 行的值为 ""。 */
const char *sxcl_options_key_at(const sxcl_options *options, size_t index);
const char *sxcl_options_value_at(const sxcl_options *options, size_t index);

void sxcl_options_free(sxcl_options *options);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_OPTIONS_H */
