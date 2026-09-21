/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_INSTANCE_IO_INTERNAL_H
#define SXCL_INSTANCE_IO_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SXCL_DIR_NAME_MAX 320      /* 单个目录项名字的字节数上限 */
#define SXCL_DIR_ERROR_MAX 224     /* 人话错误 */

typedef struct sxcl_dir_entry {
    char name[SXCL_DIR_NAME_MAX];  /**< 文件名/目录名(UTF-8,不含路径) */
    int is_dir;                    /**< 1 = 目录 */
    int64_t size;                  /**< 普通文件的大小;目录为 0 */
} sxcl_dir_entry;

typedef struct sxcl_dir_list {
    sxcl_dir_entry *items;         /**< malloc 出来的数组,用 sxcl_dir_list_free 释放 */
    size_t count;
} sxcl_dir_list;

/** 拼路径:a + "/" + b(自动处理 a 末尾的 / 与 \,不解析 "..")。返回写入长度。 */
int sxcl_dir_join(char *out, size_t out_len, const char *a, const char *b);

/** 列目录(只要名字与类型;不含 "."/"..")。成功返回 0(count 可能为 0);
 *  不是目录/打不开返回 -2 并写人话 err。名字超过 SXCL_DIR_NAME_MAX 的项会被跳过。 */
int sxcl_dir_list_open(const char *path, sxcl_dir_list *out, char *err, size_t err_len);

/** 释放目录列表(允许传 NULL)。 */
void sxcl_dir_list_free(sxcl_dir_list *list);

/** 递归删除目录(先删内容再删自己;不存在视为成功)。返回 0 成功,-2 有东西删不掉。
 *  给测试准备夹具用(夹具写在构建目录下,仓库里不留二进制)。 */
int sxcl_dir_remove_tree(const char *path);

/** 把整个文件读进内存(malloc,末尾补 NUL;*out_len 是真实字节数,不含补的 NUL)。
 *  成功返回 0;打不开返回 -2;内存不足返回 -4;读一半出错返回 -7。 */
int sxcl_dir_read_file(const char *path, char **out, size_t *out_len, char *err, size_t err_len);

/** 小工具:大小写不敏感的 ASCII 比较(与 Python 的 str.lower() 对 ASCII 的行为一致;
 *  非 ASCII 字节按原样比较——中文目录名不需要大小写折叠)。返回 <0/0/>0。 */
int sxcl_dir_name_compare(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_INSTANCE_IO_INTERNAL_H */
