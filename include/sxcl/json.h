/* SXCL-C JSON 解析器 —— 元数据层地基(自己写,零依赖)。
 *
 * 用途:解析 Mojang 的 version_manifest_v2.json / 版本 JSON / 资源索引。
 * 设计:一次性解析成 DOM(字符串在内部缓冲里就地 NUL 结尾,转义已解码),
 * 文档由调用方 sxcl_json_free 释放;所有节点指针在文档释放前有效。
 * 不追求通用库的完备性,只覆盖启动器真正需要的部分:
 * 对象/数组/字符串(含 \uXXXX 转义与代理对)/数字/布尔/null/嵌套。
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

/** 数组元素;越界返回 NULL。 */
const sxcl_json_value *sxcl_json_at(const sxcl_json_value *array, size_t index);

/** 字符串内容(已解码转义,UTF-8,NUL 结尾);非字符串返回 NULL。 */
const char *sxcl_json_string(const sxcl_json_value *value);

/** 数字;非数字返回 0。 */
double sxcl_json_number(const sxcl_json_value *value);

/** 便捷取值:对象成员缺失或类型不符时返回 def。 */
const char *sxcl_json_get_string(const sxcl_json_value *object, const char *key, const char *def);
int64_t sxcl_json_get_int64(const sxcl_json_value *object, const char *key, int64_t def);
int sxcl_json_get_bool(const sxcl_json_value *object, const char *key, int def);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_JSON_H */
