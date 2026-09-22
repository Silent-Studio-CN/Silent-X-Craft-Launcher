#ifndef SXCL_PROCESSOR_H
#define SXCL_PROCESSOR_H

/** install_profile.json 的 processors[] 重放（Forge / NeoForge 的新式安装，docs/22 的 A3）。
 *
 * 为什么要它：1.13 以后的 Forge 安装器**不再自己装东西** —— install_profile.json 里列了一串
 * processor（installertools / jarsplitter / ForgeAutoRenamingTool / binarypatcher），
 * "把原版 jar 拆成 slim + extra、按 SRG 重命名、打二进制补丁"这些活是**跑那些 Java 工具**做出来的。
 * 不重放它们，装出来的实例要么没有客户端主 jar，要么那份主 jar 是没打过补丁的原版。
 *
 * 参考实现：FCL（FCL/fclcore/download/forge/ForgeNewInstallTask.java 的 ProcessorTask +
 * ForgeNewInstallProfile.java）与 HMCL 同源代码。逐条对齐的地方写在下面各自的注释里。
 *
 * 分工（这个模块刻意不碰进程与网络）：
 *   * 起进程走 sxcl_processor_ctx.run 回调 —— 调用方拿 sxcl/process.h 的 sxcl_process_opts 实现，
 *     于是日志、取消、超时全都是安装流程那一套；
 *   * 处理器要用的 jar / classpath **必须先下好**（sxcl/loader.h 的 collect_libraries 会把它们
 *     一起列出来），本模块只检查"在不在"，不在就报错，不自己去下；
 *   * 纯函数（下面那些 accessor 与 sxcl_processor_eval）单独暴露，便于单测。
 */

#include <stddef.h>

#include "sxcl/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一份 install_profile.json 里 processors 的条数上限（真安装器最多 10 条上下，留足余量）。 */
#define SXCL_PROCESSOR_MAX 48
/** 每条处理器 argv 的条数上限（实测最长 28 条：EXTRACT_FILES）。 */
#define SXCL_PROCESSOR_MAX_ARGS 64
/** 一条 argv / 一个求值结果的字节上限（classpath 串单独见 sxcl_processor_classpath 的说明）。 */
#define SXCL_PROCESSOR_ARG_MAX 512
/** data{} 的键值上限。 */
#define SXCL_PROCESSOR_MAX_VARS 32
#define SXCL_PROCESSOR_VAR_KEY_MAX 48
#define SXCL_PROCESSOR_VAR_VALUE_MAX 512
/** 一条处理器 outputs 的条数上限（实测 2 条）。 */
#define SXCL_PROCESSOR_MAX_OUTPUTS 8
/** 错误文本上限（与 sxcl/loader.h 的 SXCL_LOADER_ERROR_MAX 对齐，避免调用方两套缓冲）。 */
#define SXCL_PROCESSOR_ERR_MAX 256

/* ── 纯访问器：把 processors[] 扒开，不做任何求值 ── */

/** processors[] 的条数（不按 side 过滤；没解析出 JSON 时为 0）。 */
size_t sxcl_processor_count(const sxcl_json *profile);

/** 这一条要不要在 side 上跑。FCL：ForgeNewInstallProfile.Processor.isSide —— 
 *  sides 缺省/为空 = 两条边都跑；含 side（"client" / "server" / "extract"）= 跑。 */
int sxcl_processor_wants(const sxcl_json *profile, size_t index, const char *side);

/** 第 index 条的 jar 坐标（Maven 坐标，如 net.minecraftforge:installertools:1.3.0）。 */
int sxcl_processor_jar(const sxcl_json *profile, size_t index, char *out, size_t cap);

/** classpath 条数 / 第 k 条坐标（都是 Maven 坐标，调用方自己转路径）。 */
size_t sxcl_processor_classpath_count(const sxcl_json *profile, size_t index);
int sxcl_processor_classpath_at(const sxcl_json *profile, size_t index, size_t k, char *out, size_t cap);

/** args 条数 / 第 k 条**原文**（求值见 sxcl_processor_eval）。 */
size_t sxcl_processor_arg_count(const sxcl_json *profile, size_t index);
const char *sxcl_processor_arg_at(const sxcl_json *profile, size_t index, size_t k);

/** outputs 条数 / 第 k 条的键与值**原文**（键是路径，值是 SHA-1）。 */
size_t sxcl_processor_output_count(const sxcl_json *profile, size_t index);
const char *sxcl_processor_output_key_at(const sxcl_json *profile, size_t index, size_t k);
const char *sxcl_processor_output_value_at(const sxcl_json *profile, size_t index, size_t k);

/* ── data{} → 变量表 ── */

/** "data 里的裸值"要从安装器 zip 里取出来落到临时文件，这里把临时文件的绝对路径写进 out。
 *  返回 0 = 拿到了；非 0 = 这条路取不到（不在 zip 里 / 写不进去），**如实报错**。
 *  单测传 NULL 时裸值原样通过（不取文件），用来单独验证字符串处理。 */
typedef int (*sxcl_processor_take_file_fn)(void *ud, const char *entry, char *out, size_t cap);

typedef struct sxcl_processor_var {
    char key[SXCL_PROCESSOR_VAR_KEY_MAX];
    char value[SXCL_PROCESSOR_VAR_VALUE_MAX];
} sxcl_processor_var;

typedef struct sxcl_processor_vars {
    sxcl_processor_var items[SXCL_PROCESSOR_MAX_VARS];
    size_t count;
} sxcl_processor_vars;

void sxcl_processor_vars_init(sxcl_processor_vars *vars);

/** 放一对键值（重复的键覆盖）。满了/参数非法返回 -1。 */
int sxcl_processor_vars_put(sxcl_processor_vars *vars, const char *key, const char *value);

/** 查一对键值；没有返回 NULL。 */
const char *sxcl_processor_vars_get(const sxcl_processor_vars *vars, const char *key);

/** 把 install_profile.json 的 data{} 求值进变量表（FCL: ForgeNewInstallTask.execute 的前半段）。
 *
 *  每个值三种写法，**先按写法分档，再做 token 替换**（顺序不能反：FCL 的 parseLiteral 就是这样）：
 *    * `[de.oceanlabs.mcp:mcp_config:1.20.1@zip]` → `<game_dir>/libraries/<maven 路径>`（绝对）；
 *    * `'20230612.114412'` → 去掉引号的字面量；
 *    * 其余（`/data/client.lzma` 这种）→ 交给 take_file 取出来，用临时文件路径。
 *
 *  data 的值有两个坑，都按 FCL 处理：
 *    1. 新格式里每个值是个**按 side 分档的对象** `{"client": ..., "server": ...}` ——
 *       FCL 的 ForgeNewInstallProfile.Datum 只取 client；我们同样只取 client，
 *       兼容"直接是字符串"的老格式（那种没有分档）；
 *    2. 值里出现 `{KEY}` 时，FCL 传的是**空表**（即 data 之间不许互相引用），
 *       我们照办 —— 缺键就是错，宁可报错也别猜。 */
int sxcl_processor_vars_from_data(const sxcl_json *profile, const char *game_dir,
                                  sxcl_processor_take_file_fn take, void *ud,
                                  sxcl_processor_vars *vars, char *err, size_t err_len);

/** 求值一个 literal（FCL: ForgeNewInstallTask.parseLiteral + replaceTokens）。
 *
 *  分档（与上面 data 的三档一致，只是第四档不再取文件而是原样）：
 *    `[坐标]`  → `<game_dir>/libraries/<路径>`；`'字面量'` → 字面量；`{键}` → 查表；
 *    其余 → replaceTokens（\\ 转义、内联 \`{键}\` 与 \`'字面量'\`；缺键报错、括号不闭合报错）。
 *
 *  take 非空时，最后那一档的结果再交给 take（data 的用法）；传 NULL 就是原样（args/outputs 的用法）。
 *  返回 0 成功；-1 参数非法；-2 结果装不下；-3 求值失败（缺键 / 不闭合 / 转义断了）。 */
int sxcl_processor_eval(const char *literal, const sxcl_processor_vars *vars, const char *game_dir,
                        sxcl_processor_take_file_fn take, void *ud, char *out, size_t cap,
                        char *err, size_t err_len);

/* ── 重放 ── */

/** 跑一个外部进程。返回 0 = 跑完了（*exit_code 有效）；非 0 = 压根没启动起来。
 *  失败时把人话原因（含最后几行输出）写进 err —— 处理器失败时那句话就是给用户看的全部解释。 */
typedef int (*sxcl_processor_run_fn)(void *ud, const char *program, const char *const *argv,
                                     size_t argc, const char *work_dir, int timeout_ms,
                                     int *exit_code, char *err, size_t err_len);

/** "这条处理器其实是个下载任务"的替代实现（FCL: patchDownloadMojangMappingsTask）。
 *
 *  1.14~1.20 的安装器里有一条 `--task DOWNLOAD_MOJMAPS --side client --version X --output Y`：
 *  它会自己去 piston-data 抓 client_mappings（**官方站点在国内经常连不上**，FCL 专门为它打了补丁）。
 *  version/output 已经把字面量求好了；返回 0 = 下好了（processors 就跳过这条处理器），
 *  非 0 = 没下成（**照常跑处理器**，让它自己再试一次 —— 有官方网络的环境照样能装完）。 */
typedef int (*sxcl_processor_mappings_fn)(void *ud, const char *version, const char *output,
                                         char *err, size_t err_len);

typedef struct sxcl_processor_ctx {
    const char *game_dir;        /**< 必填：`[坐标]` / `{ROOT}` / `{LIBRARY_DIR}` 的基准 */
    const char *installer_jar;   /**< 必填：{INSTALLER}，也是 data 里相对路径的来源 */
    const char *minecraft_jar;   /**< 可空：{MINECRAFT_JAR} */
    const char *minecraft_version; /**< 可空：{MINECRAFT_VERSION} */
    const char *library_dir;     /**< 可空：{LIBRARY_DIR}（空则用 <game_dir>/libraries） */
    const char *temp_dir;        /**< 可空：data 里的裸值取出来放这儿；空 = 遇到就报错 */
    const char *java_path;       /**< 必填：首选 java；另有 JAVA_HOME / PATH 两条兜底（见实现） */
    int timeout_ms;              /**< <=0 = 30 分钟（与 sxcl/loader.h 的默认一致） */
    int dry_run;                 /**< 1 = 只算出"要跑什么"并如实报告，不真跑、不校验产物 */
    sxcl_processor_take_file_fn take_file; /**< 可空：data 里的裸值从安装器 zip 取出来的实现（见上）。
                                            *  空 = 遇到裸值就报错（命令行/单测常用）。 */
    sxcl_processor_run_fn run;            /**< 必填（dry_run 时可不给） */
    sxcl_processor_mappings_fn download_mappings; /**< 可空，见上面 */
    int (*is_cancelled)(void *ud);        /**< 可空 */
    void (*report)(void *ud, const char *text);  /**< 可空：进度一行（人话） */
    void *ud;                    /**< 上面三个回调的 userdata */
} sxcl_processor_ctx;

typedef struct sxcl_processor_stats {
    int considered;   /**< 按 side 过滤之后剩几条（= 要管事的总数） */
    int ran;          /**< 真跑了进程的条数 */
    int skipped;      /**< outputs 全在且 SHA-1 都对，跳过的条数（幂等，FCL 同款） */
    int downloaded;   /**< 走了 download_mappings 替代实现的条数 */
} sxcl_processor_stats;

/** 重放 processors[]。返回 0 = 全部跑完（stats 里有件数）；1 = 有处理器失败；2 = 用户取消；
 *  -1 = 参数不合法（看 err）；-2 = 处理器要用的文件不在（安装器自带 jar / classpath 缺），
 *  这一档**必须报错**，因为再往下跑必然出错，不如在这里点名。 */
int sxcl_processors_run(const sxcl_json *profile, const char *side, const sxcl_processor_ctx *ctx,
                        sxcl_processor_stats *stats, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* SXCL_PROCESSOR_H */
