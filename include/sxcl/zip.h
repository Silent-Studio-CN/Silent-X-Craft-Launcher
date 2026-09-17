/* SXCL-C ZIP 读取模块(只读,不写 zip)。
 *
 * 纯 C11 自研实现,零第三方依赖(不用 libzip / minizip / Info-ZIP),
 * 解压走本工程的 sxcl/inflate.h(自带 raw DEFLATE 解压)。
 *
 * 用途:Forge / NeoForge / OptiFine 安装器都是 jar(ZIP),启动器要静默从里面
 * 取 install_profile.json、version.json、maven/* 依赖并自己完成安装。
 *
 * 实现约定:
 *   - 一律以中央目录为准(PK\x01\x02):流式写入的 zip 本地头里的大小字段可能是 0,
 *     只有中央目录是权威的;EOCD(PK\x05\x06)从文件尾部回扫定位;
 *   - 文件名:UTF-8 标志(通用位 11)置位时按 UTF-8 原样使用,否则按 CP437 转成 UTF-8,
 *     纯 ASCII 名直接跳过转换;名字比较大小写敏感;
 *   - 压缩方法 0(stored)与 8(deflate)支持,其它视为不支持(返回 -3);
 *   - ZIP64 条目(大小/偏移为 0xFFFFFFFF)明确报"不支持";整包 ZIP64(>4 GiB 或
 *     > 65535 个条目)在 open 阶段直接判为打不开;
 *   - 条目只在 sxcl_zip_open 时读一次中央目录:几千个条目的 jar 也只占几百 KB 内存,
 *     后续解压按需 seek 本地头,不把整包读进内存。
 */
#ifndef SXCL_ZIP_H
#define SXCL_ZIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 返回值约定(解压类接口共用)。 */
#define SXCL_ZIP_OK           0    /**< 成功 */
#define SXCL_ZIP_ERR         (-1)  /**< 没有这个条目 / 打不开 / 数据损坏 / 落盘失败 */
#define SXCL_ZIP_ERR_SPACE   (-2)  /**< 缓冲不够(只有 extract_memory 会返回) */
#define SXCL_ZIP_ERR_UNSUP   (-3)  /**< 不支持:压缩方法、加密、ZIP64、目录条目 */
#define SXCL_ZIP_ERR_ABORT   (-4)  /**< sink 返回非 0 主动中止 */

/** 不透明句柄。 */
typedef struct sxcl_zip sxcl_zip;

/** 打开 zip 文件(只读)。打不开 / 不是 zip 返回 NULL。
 *  路径按 UTF-8 处理(Windows 侧内部转 UTF-16,不受代码页影响)。 */
sxcl_zip *sxcl_zip_open(const char *path);

/** 关闭句柄(允许传 NULL)。 */
void sxcl_zip_close(sxcl_zip *zip);

/** 条目总数(含目录条目,按中央目录顺序)。 */
size_t sxcl_zip_count(const sxcl_zip *zip);

/** 第 index 个条目的名字(UTF-8,指向内部存储,不要 free;越界返回 NULL)。 */
const char *sxcl_zip_name_at(const sxcl_zip *zip, size_t index);

/** 第 index 个条目解压后的大小(字节;越界返回 -1)。 */
int64_t sxcl_zip_size_at(const sxcl_zip *zip, size_t index);

/** 第 index 个条目的压缩方法(0 = stored,8 = deflate;越界返回 -1)。 */
int sxcl_zip_method_at(const sxcl_zip *zip, size_t index);

/** 按名字找条目(大小写敏感)。返回下标;-1 = 没有(同时是名字非法/zip 为 NULL 的返回)。 */
int sxcl_zip_find(const sxcl_zip *zip, const char *name);

/** 解到内存。成功返回 0 并把实际字节数写进 *out_len。
 *  缓冲不够返回 -2,*out_len 写入"实际需要多少字节"(条目原始大小)。
 *  其它失败返回 -1 / -3。 */
int sxcl_zip_extract_memory(sxcl_zip *zip, const char *name, void *out, size_t out_cap, size_t *out_len);

/** 解到文件:自动建父目录,先写 <dest>.tmp 再原子改名,失败不留 .tmp。
 *  成功返回 0,失败返回 -1 / -3。 */
int sxcl_zip_extract_file(sxcl_zip *zip, const char *name, const char *dest);

/** 逐块解出(大条目用,内存里只占一个解压窗口 + 一块读缓冲):
 *  每解出一块就调用 sink,sink 返回非 0 中止并让本函数返回 -4。
 *  成功返回 0,失败返回 -1 / -3。 */
int sxcl_zip_extract_stream(sxcl_zip *zip, const char *name,
                            int (*sink)(void *ud, const void *data, size_t len), void *ud);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_ZIP_H */
