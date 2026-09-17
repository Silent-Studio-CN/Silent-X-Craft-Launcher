/* 键位映射模块内部共用的小工具(keymap.c / keymap_presets.c / keymap_fcl.c 之间)。
 * 不进 include/sxcl/,外面不该 include 它。 */
#ifndef SXCL_KEYMAP_INTERNAL_H
#define SXCL_KEYMAP_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>

#include "sxcl/keymap.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 定长拷贝:能装下返回 0;装不下按 cap 截断(始终 NUL 结尾)并返回 -1。 */
int sxcl_kp_copy(char *dst, size_t cap, const char *src);
int sxcl_kp_copy_range(char *dst, size_t cap, const char *begin, size_t len);
/** 小写化(就地把 ASCII 转小写)。 */
void sxcl_kp_lower(char *text);
double sxcl_kp_clamp(double value, double low, double high);
/** 对象里取数字;不是数字/没有就返回 def。 */
double sxcl_kp_number(const sxcl_json_value *object, const char *key, double def);
/** 对象里取字符串;没有就返回 def。 */
const char *sxcl_kp_string(const sxcl_json_value *object, const char *key, const char *def);

/** 往 issues 里记一条(issues 为空就什么都不做;满了记在 dropped 里)。
 *  control_id 可空。 */
void sxcl_kp_issue(sxcl_keymap_issues *issues, int level, const char *control_id,
                   const char *fmt, ...);

/* ── 动态字符串缓冲(内部用;data 永远是 NUL 结尾) ── */
typedef struct sxcl_kp_buf {
    char *data;
    size_t len;
    size_t cap;
} sxcl_kp_buf;

void sxcl_kp_buf_init(sxcl_kp_buf *buf);
void sxcl_kp_buf_free(sxcl_kp_buf *buf);
int sxcl_kp_buf_putc(sxcl_kp_buf *buf, char ch);
int sxcl_kp_buf_puts(sxcl_kp_buf *buf, const char *text);
int sxcl_kp_buf_printf(sxcl_kp_buf *buf, const char *fmt, ...);
/** 换行 + indent 层缩进(每层两个空格)。 */
int sxcl_kp_buf_indent(sxcl_kp_buf *buf, int indent);
/** 带引号的 JSON 字符串(转义 " \ 与 <0x20)。 */
int sxcl_kp_buf_json_string(sxcl_kp_buf *buf, const char *text);
/** JSON 数字(%g;非法值按 0)。 */
int sxcl_kp_buf_json_number(sxcl_kp_buf *buf, double value);
/** 把已解析的 JSON 子树原样写回去(meta 原样保留靠它)。indent 是当前缩进层数。 */
int sxcl_kp_json_dump(sxcl_kp_buf *buf, const sxcl_json_value *value, int indent);

/** 追加按钮/方向(容量不足自动扩容)。 */
int sxcl_kp_push_button(sxcl_keymap_layout *layout, const sxcl_keymap_button *button);
int sxcl_kp_push_direction(sxcl_keymap_layout *layout, const sxcl_keymap_direction *direction);

/** 序号 -> 方向键名("up"/"down"/"left"/"right");越界返回 ""。 */
const char *sxcl_kp_direction_key_name(size_t index);
/** 方向键名 -> 结构体里的字段(取/写都走它,省得四份重复代码)。 */
char *sxcl_kp_direction_key_slot(sxcl_keymap_direction *direction, const char *name);
const char *sxcl_kp_direction_key_slot_const(const sxcl_keymap_direction *direction, const char *name);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_KEYMAP_INTERNAL_H */
