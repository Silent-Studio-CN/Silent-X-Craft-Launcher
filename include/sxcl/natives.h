/* Minecraft 原生库(natives)抽取 —— 把 classifier jar 里的 .dll/.so/.dylib 解到 natives_dir。
 *
 * 为什么必须有这一步:启动参数里的 -Djava.library.path=<natives_dir> 指向的目录如果还是空的,
 * LWJGL 在加载 liblwjgl.so / lwjgl.dll 时必然抛 UnsatisfiedLinkError —— 参数拼得再对也没用。
 * 这些原生库不是散装的,它们躺在 classifier jar 里(LWJGL 2 时代是 <库>-natives-<os>.jar),
 * 所以要"打开 jar -> 按 extract 规则挑条目 -> 解到 natives 目录"。
 *
 * 覆盖两种历史形态(都在真实版本 JSON 里出现过):
 *   老形态(1.12 及更早):library.natives.<os> 指向 downloads.classifiers 里的某个 jar;
 *   新形态(1.19 起):natives 本身就是一条独立库(name 里带 ":natives-<os>" 分类器,
 *                    jar 在 downloads.artifact 里),extract 字段挂在条目上。
 * 两种形态共用同一套规则,调用方不用区分。
 *
 * 承诺(接口契约):
 *   - **幂等**:同一个 jar(大小 + mtime 都没变)上次解出来的文件也都还在时,第二次调用
 *     **不会碰任何文件**(不重写、不改 mtime),返回 0 且计数不变。所以每次启动都直接调它即可;
 *   - **排除**:library.extract.exclude 里的前缀(典型是 "META-INF/")不落盘;目录条目不落盘;
 *   - **相对路径保留**:jar 里的 sub/bar.so 解出来仍然是 <natives_dir>/sub/bar.so
 *     (LWJGL 的某些原生库就靠子目录区分);
 *   - **出错能定位**:错误信息里一定带上是哪个 jar 的哪个条目,或哪个库的 jar 根本不存在;
 *   - **安全**:条目名里出现 ".."、绝对路径、盘符的一律拒绝 —— 恶意 jar 不能写到 natives_dir 外面;
 *   - 只看 <game_dir>/libraries 下**已经存在**的 jar,**不下载**(下载是引擎那边的事);
 *     也不清理"这次用不到的旧文件"(留着的旧 dll 最多占地方,删错一个就是启动失败)。
 *
 * 返回:0 = natives_dir 已就绪(完全可能 0 个文件:这个版本根本没有 natives);<0 = 失败,err 有原因。
 */
#ifndef SXCL_NATIVES_H
#define SXCL_NATIVES_H

#include <stddef.h>

#include "sxcl/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 错误信息/路径缓冲的建议大小。 */
#define SXCL_NATIVES_PATH_MAX 1024

/** 由版本 JSON 文件路径准备 natives(内部自己解析 JSON)。 */
int sxcl_natives_prepare(const char *version_json_path, const char *game_dir,
                         const char *natives_dir, char *err, size_t err_len);

/** 同上,但直接吃已经解析好的版本 JSON(启动驱动手里就有一份,不必重复解析)。 */
int sxcl_natives_prepare_json(const sxcl_json *version_json, const char *game_dir,
                              const char *natives_dir, char *err, size_t err_len);

/** 上一次 sxcl_natives_prepare* 之后 natives_dir 里**可用**的原生库文件数
 *  (本次新解的 + 上次解过且仍然有效的)。失败时是 0。
 *  语义是"就绪了几个",不是"这次写了几个" —— 幂等跳过的那次也会照常报同样的数量,
 *  界面可以拿它显示"原生库 12 个文件已就绪"。
 *  线程安全:**不做任何承诺**(与 settings/options 一致,启动器里这条线是单线程的)。 */
int sxcl_natives_last_count(void);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_NATIVES_H */
