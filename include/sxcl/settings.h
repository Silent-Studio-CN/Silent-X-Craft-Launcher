#ifndef SXCL_SETTINGS_H
#define SXCL_SETTINGS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* SXCL-C 设置存储:全局设置 + 每实例设置,UTF-8 文本持久化,零第三方依赖。
 *
 * 磁盘格式(每行一条,UTF-8,允许 CRLF):
 *   key=value
 *   - 只按**第一个** '=' 切分,所以值里可以带 '=';空值合法(key=)。
 *   - 以 '#' 开头的行(前面允许空白)是注释;空行忽略。
 *   - 没有 '=' 的行、键名含 [A-Za-z0-9._-] 与 UTF-8 高位字节以外字符的行一律跳过,不报错。
 *   - 同名键以**最后**一次出现的值为准(与 json.c 的重复键语义一致),位置沿用第一次出现的位置。
 *   - 文件若以 UTF-8 BOM 开头,BOM 会被忽略。
 *   - 打不开文件(不存在/无权限)一律当作"全新",得到默认值,不算失败。
 *
 * 写回(sxcl_settings_save):
 *   - 先写 <path>.tmp 再原子改名覆盖目标;父目录不存在会自动建。
 *   - 写回时**不保留**原文件的注释、空行与重复键(只写当前内存里的键值,按插入顺序)。
 *     —— 也就是说注释在第一次保存后就没了,注释只用于"给人看的人肉编辑"。
 *   - 换行固定 LF,不写 CR。
 *
 * 顺序:保留插入顺序;改值不换位置;新增追加到末尾;删除整行移除。
 *
 * 线程安全:**不做任何承诺**。设置是主线程读写的(启动器 UI 线程),句柄本身没有锁,
 *   多线程同时读写同一个 sxcl_settings 是数据竞争。要在别的线程读,请自己抄一份或用锁。
 *
 * 生命周期:sxcl_settings_get / _key_at / _value_at 返回的指针指向句柄内部,
 *   在下一次 set/remove 该键或 sxcl_settings_free 之后失效;要留就自己 strdup。
 */

typedef struct sxcl_settings sxcl_settings;

/** 打开设置文件。文件不存在 = 全新(使用默认值),不算失败。 */
sxcl_settings *sxcl_settings_open(const char *path);
/** 原子写回(tmp + 改名)。返回 0 成功。 */
int sxcl_settings_save(sxcl_settings *settings, const char *path);
void sxcl_settings_free(sxcl_settings *settings);

/** 取值:不存在返回 def。键名允许 [A-Za-z0-9._-]。 */
const char *sxcl_settings_get(sxcl_settings *settings, const char *key, const char *def);
/** 设值(已存在则改,不存在则追加到末尾)。返回 0 成功。 */
int sxcl_settings_set(sxcl_settings *settings, const char *key, const char *value);
/** 删除键(不存在不算错)。返回 0 成功。 */
int sxcl_settings_remove(sxcl_settings *settings, const char *key);

int64_t sxcl_settings_get_int(sxcl_settings *settings, const char *key, int64_t def);
double  sxcl_settings_get_double(sxcl_settings *settings, const char *key, double def);
int     sxcl_settings_get_bool(sxcl_settings *settings, const char *key, int def);

/** 带默认值的便捷读取:这些键的默认值已经定好,别自己另定一套。
 *  download.rate        = "0"(字节/秒,0 = 不限速)
 *  download.workers     = "0"(0 = 自动)
 *  download.max_conn    = "1"(单文件最大连接数,1 = 不分片)
 *  download.cache_dir   = "" (空 = 用默认位置)
 *  game.default_dir     = "" (空 = 用平台默认 .minecraft)
 *  ui.theme             = "auto"
 *  ui.language          = "zh-CN"
 */
const char *sxcl_settings_download_rate_text(sxcl_settings *settings);
int64_t sxcl_settings_download_workers(sxcl_settings *settings);
int64_t sxcl_settings_download_max_conn(sxcl_settings *settings);
const char *sxcl_settings_download_cache_dir(sxcl_settings *settings);
const char *sxcl_settings_game_default_dir(sxcl_settings *settings);
const char *sxcl_settings_ui_theme(sxcl_settings *settings);
const char *sxcl_settings_ui_language(sxcl_settings *settings);

/** 每实例设置:内部键名是 "instance.<实例名>.<键>"。
 *  实例名与键里不允许出现 '.' 之外的怪字符由调用方保证;返回的指针在 settings 释放前有效。 */
const char *sxcl_settings_instance_get(sxcl_settings *settings, const char *instance, const char *key, const char *def);
int sxcl_settings_instance_set(sxcl_settings *settings, const char *instance, const char *key, const char *value);
/** 渲染后端:键名固定 "graphicsApi",值域由调用方给(如 "default"/"vulkan"/"opengl")。 */
const char *sxcl_settings_instance_graphics_api(sxcl_settings *settings, const char *instance);

size_t sxcl_settings_count(const sxcl_settings *settings);
const char *sxcl_settings_key_at(const sxcl_settings *settings, size_t index);
const char *sxcl_settings_value_at(const sxcl_settings *settings, size_t index);
#ifdef __cplusplus
}
#endif
#endif
