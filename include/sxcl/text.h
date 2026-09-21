/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_TEXT_H
#define SXCL_TEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 文本编码探测与转换:游戏自己写的 latest.log / crash-report **不保证**是 UTF-8。
 *
 * 为什么必须做:中文 Windows 上的 Minecraft(以及大量整合包的日志)按系统 ANSI 代码页
 * (GBK/CP936)落盘,直接按 UTF-8 读会得到一串"锟斤拷";UTF-16 的日志也有(带 BOM 或
 * 零字节分布能认出来)。我们的界面、日志管线、导出包只吃 UTF-8,所以读游戏文件之前
 * **先探编码再转**,而不是指望它一定是 UTF-8。
 *
 * 编码键(稳定英文小写,可直接进日志/断言):
 *   "empty"      空文件(0 字节)
 *   "ascii"      全部是 7 位 ASCII
 *   "utf-8"      合法 UTF-8(无 BOM)
 *   "utf-8-bom"  带 UTF-8 BOM(EF BB BF)
 *   "utf-16le" / "utf-16be"  带 BOM,或零字节稳定落在奇/偶位的启发式判定
 *   "gbk"        不是合法 UTF-8,但高字节全部符合 GBK 双字节结构(中文 Windows 的 ANSI)
 *   "ansi"       不是合法 UTF-8,也认不出 GBK(按系统 ANSI / Latin-1 兜底)
 *   "binary"     含 NUL 且不像 UTF-16(真二进制,不该按文本分析)
 */

/** 编码键缓冲长度(含 NUL)。 */
#define SXCL_TEXT_ENCODING_MAX 16

/** 只看字节、不做转换。永远返回非空稳定键(见上面的表)。data 为 NULL 时返回 "empty"。 */
const char *sxcl_text_detect_encoding(const void *data, size_t len);

/** 是不是合法 UTF-8:严格拒绝超长编码、UTF-16 代理区、5/6 字节序列、被截断的序列。
 *  len == 0 视为合法。 */
int sxcl_text_is_valid_utf8(const void *data, size_t len);

/** 数行数:换行符个数;最后一行没有换行符时也算一行;空内容 = 0 行。
 *  行尾的 CR 不算独立一行(CRLF 与 LF 同口径)。 */
size_t sxcl_text_count_lines(const void *data, size_t len);

/** 取下一行(不拷贝缓冲,只前进游标):入参 *cursor 指向当前位置,返回后指向下一行开头。
 *  行尾的 CR/LF 去掉;out 不够长时截断(但仍会走到行尾)。
 *  返回 1 = 取到一行;0 = 没有更多(*cursor 为 NULL 或已到末尾)。 */
int sxcl_text_next_line(const char **cursor, char *out, size_t out_cap);

/** 转成 UTF-8(自动探测编码)。写入 out,并保证 NUL 结尾(空内容写空串)。
 *  缓冲不够时截断在**合法 UTF-8 边界**上,truncated(可空)置 1。
 *  返回写入的字节数(不含结尾 NUL)。 */
size_t sxcl_text_to_utf8(const void *data, size_t len, char *out, size_t out_cap, int *truncated);

/** GBK/ANSI 兜底解码用到的代码页:Windows 上是 GetACP()(中文机器 = 936)。
 *  0 = 本平台没有系统 ANSI 解码器(转换会退回 Latin-1 直通)。 */
unsigned int sxcl_text_ansi_codepage(void);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_TEXT_H */
