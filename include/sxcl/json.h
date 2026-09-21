/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_JSON_H
#define SXCL_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sxcl_json_type {
    SXCL_JSON_NULL = 0,
    SXCL_JSON_BOOL,
    SXCL_JSON_NUMBER,
    SXCL_JSON_STRING,
    SXCL_JSON_ARRAY,
    SXCL_JSON_OBJECT
} sxcl_json_type;

typedef struct sxcl_json sxcl_json;              /**< 文档句柄 */
typedef struct sxcl_json_value sxcl_json_value;  /**< 节点(属于某个文档) */

/** 解析 UTF-8 JSON。失败返回 NULL,并把原因写进 err(可空)。 */
sxcl_json *sxcl_json_parse(const char *text, size_t len, char *err, size_t err_len);

/** 解析文件(内部按需读入,UTF-8)。失败返回 NULL 并写 err。 */
sxcl_json *sxcl_json_parse_file(const char *path, char *err, size_t err_len);

/** 释放文档及其全部节点。传 NULL 安全。 */
void sxcl_json_free(sxcl_json *doc);

/** 根节点;空文档返回 NULL。 */
const sxcl_json_value *sxcl_json_root(const sxcl_json *doc);

/** 节点类型。 */
sxcl_json_type sxcl_json_type_of(const sxcl_json_value *value);

/** 取对象成员;不存在返回 NULL。 */
const sxcl_json_value *sxcl_json_get(const sxcl_json_value *object, const char *key);

/** 数组长度;非数组返回 0。 */
size_t sxcl_json_size(const sxcl_json_value *array);

/** 对象成员数;非对象返回 0。(资源索引的 objects 有 5000+ 成员,必须能遍历) */
size_t sxcl_json_member_count(const sxcl_json_value *object);

/** 第 index 个成员的键;越界或非对象返回 NULL。 */
const char *sxcl_json_member_key(const sxcl_json_value *object, size_t index);

/** 第 index 个成员的值;越界或非对象返回 NULL。 */
const sxcl_json_value *sxcl_json_member_value(const sxcl_json_value *object, size_t index);

/** 数组元素;越界返回 NULL。 */
const sxcl_json_value *sxcl_json_at(const sxcl_json_value *array, size_t index);

/** 字符串内容(已解码转义,UTF-8,NUL 结尾);非字符串返回 NULL。 */
const char *sxcl_json_string(const sxcl_json_value *value);

/** 数字;非数字返回 0。 */
double sxcl_json_number(const sxcl_json_value *value);

/** 布尔节点的值;非布尔返回 0。数组里/根位置的布尔必须用这个读(键级接口读不到)。 */
int sxcl_json_bool(const sxcl_json_value *value);

/** 便捷取值:对象成员缺失或类型不符时返回 def。 */
const char *sxcl_json_get_string(const sxcl_json_value *object, const char *key, const char *def);
int64_t sxcl_json_get_int64(const sxcl_json_value *object, const char *key, int64_t def);
int sxcl_json_get_bool(const sxcl_json_value *object, const char *key, int def);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_JSON_H */
