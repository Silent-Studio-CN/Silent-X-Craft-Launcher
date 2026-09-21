/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_DIAG_INTERNAL_H
#define SXCL_DIAG_INTERNAL_H

#include <stddef.h>

/* 诊断/取证模块(src/core/diag)内部共用件:目录列举、路径拼接、按已知编码转换。
 * 不是公共契约(include/sxcl/ 里没有它),别的模块不要包含。 */

#define SXCL_DIAG_NAME_MAX 256
#define SXCL_DIAG_MAX_FILES 128

/** 一次目录列举的结果(只收**普通文件**的名字,不含 . / ..)。 */
typedef struct sxcl_diag_dir {
    char names[SXCL_DIAG_MAX_FILES][SXCL_DIAG_NAME_MAX];
    size_t count;
    int truncated; /**< 1 = 目录里的文件比 SXCL_DIAG_MAX_FILES 多,只收到了前一批 */
} sxcl_diag_dir;

/** 列目录(UTF-8 路径)。返回 0 成功(目录为空也算成功),-1 = 打不开/不是目录。 */
int sxcl_diag_dir_list(const char *dir, sxcl_diag_dir *out);

/** 拼路径(用平台分隔符)。返回 0 成功,-1 = 缓冲不够(不写半个路径)。 */
int sxcl_diag_join(char *out, size_t cap, const char *a, const char *b);

/** 大小写不敏感的后缀判断("x.TXT" 对 ".txt" 成立)。 */
int sxcl_diag_ends_with_ci(const char *text, const char *suffix);

/** 这个文件里有没有"看得见的内容"(至少一个非空白字节)。0 字节 = 0。
 *  存在的意义:崩溃目录里常留着一份 0 字节/只有空白的 crash-*.txt,
 *  拿它去分析会得到"什么都没看出来",不如按"空报告"跳过。 */
int sxcl_diag_has_visible_text(const char *path);

/** 按**已知**编码转 UTF-8(探测在别处做过;读大文件的尾部时不能重新探)。
 *  语义与 sxcl_text_to_utf8 一致。 */
size_t sxcl_diag_text_to_utf8_as(const char *encoding, const void *data, size_t len, char *out,
                                 size_t out_cap, int *truncated);

#endif /* SXCL_DIAG_INTERNAL_H */
