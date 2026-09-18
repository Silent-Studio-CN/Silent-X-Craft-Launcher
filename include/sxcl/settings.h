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

/* ── 跨平台设置位置(四平台;Android 走**应用私有目录**) ──
 *
 * 为什么要有它:界面层原先各自拼路径(settings_page/home_page/versions_page 各一份),
 * 安卓上全都落到 "~/.config/..." —— 而安卓的 $HOME 是 "/",那个路径**根本写不进去**,
 * 于是设置能改、看着也成功,重启就没了。唯一权威放核心库:
 *   Windows  %APPDATA%/SilentXCraftLauncher(没有 APPDATA 用 %USERPROFILE%/AppData/Roaming/...)
 *   macOS    ~/Library/Application Support/SilentXCraftLauncher
 *   Linux    $XDG_CONFIG_HOME/silentxcraftlauncher 或 ~/.config/silentxcraftlauncher
 *            (小写短横线:与 Python platform.py:148-157 的 app_name.lower() 一致,
 *             也与 src/services/modloader/keymap_store.c 的 default_root 一致)
 *   Android  $SXCL_ANDROID_FILES/SilentXCraftLauncher(应用 files 目录,零权限即可读写;
 *            与 src/core/instance/paths.c 的默认游戏目录同一个口径)
 * 环境变量 **SXCL_CONFIG_DIR** 覆盖一切(便携版/测试/多配置并存)。
 *
 * 返回码:SXCL_SETTINGS_OK / ERR_ARG / ERR_UNSUPPORTED(拼不出,如安卓没有 SXCL_ANDROID_FILES)/
 * ERR_SPACE(缓冲不够)。err 可空。 */
#define SXCL_SETTINGS_OK              0
#define SXCL_SETTINGS_ERR_ARG       (-1)
#define SXCL_SETTINGS_ERR_UNSUPPORTED (-2)
#define SXCL_SETTINGS_ERR_SPACE     (-3)
#define SXCL_SETTINGS_ERR_MAX       256   /**< 人话错误缓冲长度 */

/** 设置**目录**(不含文件名)。 */
int sxcl_settings_default_dir(char *out, size_t out_len, char *err, size_t err_len);
/** 设置**文件**全路径 = 目录 + "/settings.conf"(与界面层一直用的文件名一致)。 */
int sxcl_settings_default_path(char *out, size_t out_len, char *err, size_t err_len);

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

/** ── 启动期解析(环境变量优先) ──
 *
 * 口径:先看环境变量,没有再看设置文件,都没有再用默认值。这样"临时改一次"不用动配置,
 * 验收/自动化也能钉死取值(界面层以前只有主题/强调色认环境变量,其余项落盘了却没人读)。
 *
 *   SXCL_UI_THEME   = auto|light|dark   (默认 auto)
 *   SXCL_UI_ACCENT  = #rrggbb           (默认 #0067c0,与 fluent_theme.h 的默认主题色一致)
 *   SXCL_UI_LANG    = zh-CN|en-US       (默认 zh-CN;旧名 SXCL_UI_LANGUAGE 也认,界面层先用过它)
 *   SXCL_GAME_DIR   = 游戏目录           (默认空 = 由 sxcl_paths_* 取平台默认)
 *   SXCL_DL_WORKERS / SXCL_DL_RATE / SXCL_DL_MAX_CONN / SXCL_DL_CACHE_DIR
 *
 * 返回的指针:来自设置文件的归句柄所有(settings 释放即失效);来自环境变量的一直有效。
 * settings 传 NULL 时只认环境变量(调用方还没打开设置文件也能用)。 */
const char *sxcl_settings_resolved_theme(sxcl_settings *settings);
const char *sxcl_settings_resolved_accent(sxcl_settings *settings);
const char *sxcl_settings_resolved_language(sxcl_settings *settings);
const char *sxcl_settings_resolved_game_dir(sxcl_settings *settings);

/** 强调色的默认值(#0067c0)。 */
#define SXCL_SETTINGS_DEFAULT_ACCENT "#0067c0"
/** 主题模式的默认值。 */
#define SXCL_SETTINGS_DEFAULT_THEME "auto"

/** 启动期解析出的下载参数(核心库只给值,建引擎由调用方做)。零初始化 = 全默认。 */
typedef struct sxcl_settings_download {
    int workers;             /**< <=0 = 引擎自动(min(8, CPU*2)) */
    double rate_bps;         /**< 0 = 不限速(字节/秒) */
    int max_conn_per_file;   /**< <=1 = 单文件不分片 */
    char cache_dir[512];     /**< 哈希缓存目录;空 = 不用缓存 */
} sxcl_settings_download;

/** 读 download.workers / download.rate / download.max_conn / download.cache_dir(环境变量优先)
 *  并填进 out(out 必须非空;函数内部先清零)。 */
void sxcl_settings_resolve_download(sxcl_settings *settings, sxcl_settings_download *out);

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
