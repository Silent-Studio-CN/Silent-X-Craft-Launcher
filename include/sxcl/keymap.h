/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_KEYMAP_H
#define SXCL_KEYMAP_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 返回码 ── */
#define SXCL_KEYMAP_OK          0
#define SXCL_KEYMAP_ERR_ARG    (-1)   /* 参数不合法 */
#define SXCL_KEYMAP_ERR_FORMAT (-2)   /* JSON 不是 sxcl.keymap.v1 的样子(解析器给的 err 里有原因) */
#define SXCL_KEYMAP_ERR_IO     (-3)   /* 文件读写失败 */
#define SXCL_KEYMAP_ERR_NOMEM  (-4)   /* 内存不足 */
#define SXCL_KEYMAP_ERR_SPACE  (-5)   /* 输出缓冲不够 */

/* ── 尺寸上限(定长字段;超长截断而不是崩,校验会把它报出来) ── */
#define SXCL_KEYMAP_SCHEMA_MAX   24
#define SXCL_KEYMAP_NAME_MAX     64
#define SXCL_KEYMAP_SCREEN_MAX   16
#define SXCL_KEYMAP_VERSION_MAX  24
#define SXCL_KEYMAP_DESC_MAX     256
#define SXCL_KEYMAP_ID_MAX       40
#define SXCL_KEYMAP_LABEL_MAX    48
#define SXCL_KEYMAP_HINT_MAX     200
#define SXCL_KEYMAP_ICON_MAX     64
#define SXCL_KEYMAP_KEY_MAX      32    /* 单个键名 "KEY_LEFT_SHIFT" / "MOUSE_RIGHT" */
#define SXCL_KEYMAP_ACTION_MAX   40
#define SXCL_KEYMAP_ENUM_MAX     16    /* behavior/shape/style/group 这种枚举串 */
#define SXCL_KEYMAP_KEYS_MAX     8     /* 一个绑定最多几个键 */
#define SXCL_KEYMAP_ISSUE_MAX    64    /* 一次校验/冲突最多给几条 */
#define SXCL_KEYMAP_ISSUE_TEXT_MAX 208
#define SXCL_KEYMAP_HITS_MAX     32
#define SXCL_KEYMAP_PRESET_MAX   8
#define SXCL_KEYMAP_META_MAX     (64u * 1024u)   /* meta 原样保留的上限(9 套资产里最大 ~6KB) */

#define SXCL_KEYMAP_SCHEMA "sxcl.keymap.v1"

/* ── 四种事件 ── */

typedef enum sxcl_keymap_event {
    SXCL_KEYMAP_EVENT_PRESS = 0,        /**< 按下 */
    SXCL_KEYMAP_EVENT_LONG_PRESS = 1,   /**< 长按(400ms,与 FCL 一致) */
    SXCL_KEYMAP_EVENT_CLICK = 2,        /**< 单击 */
    SXCL_KEYMAP_EVENT_DOUBLE_CLICK = 3, /**< 双击(400ms 内两次) */
    SXCL_KEYMAP_EVENT_COUNT = 4
} sxcl_keymap_event;

/** "press"/"long_press"/"click"/"double_click";越界返回 ""。 */
const char *sxcl_keymap_event_id(sxcl_keymap_event event);
/** "按下"/"长按"/"单击"/"双击"(界面用);越界返回 ""。 */
const char *sxcl_keymap_event_label(sxcl_keymap_event event);
/** 字符串 -> 事件;-1 = 认不出来。 */
int sxcl_keymap_event_from_id(const char *id);

/** 枚举串的白名单归一(认不出来 -> 默认值):behavior(hold/toggle/tap)、
 *  shape(round/square/pill)、style(dpad/rocker/dpad_compact)、screen(landscape/portrait)。
 *  normalized 非空时把"原来是别的值、被换掉了"写进去(1=换过),供校验报警。 */
const char *sxcl_keymap_normalize_behavior(const char *value, int *normalized);
const char *sxcl_keymap_normalize_shape(const char *value, int *normalized);
const char *sxcl_keymap_normalize_style(const char *value, int *normalized);
const char *sxcl_keymap_normalize_screen(const char *value, int *normalized);

/* ── 模型 ── */

typedef struct sxcl_keymap_binding {
    char action[SXCL_KEYMAP_ACTION_MAX];              /**< 逻辑动作 jump/sneak/attack/… */
    char keys[SXCL_KEYMAP_KEYS_MAX][SXCL_KEYMAP_KEY_MAX]; /**< 注入的按键 KEY_SPACE / MOUSE_LEFT */
    size_t key_count;
    char behavior[SXCL_KEYMAP_ENUM_MAX];              /**< hold / toggle / tap */
} sxcl_keymap_binding;

typedef struct sxcl_keymap_button {
    char id[SXCL_KEYMAP_ID_MAX];
    char label[SXCL_KEYMAP_LABEL_MAX];
    char hint[SXCL_KEYMAP_HINT_MAX];      /**< 教学提示(FCL 没有这一项) */
    char icon[SXCL_KEYMAP_ICON_MAX];
    double x, y, w, h;                    /**< 0~1 归一化(FCL 用像素,会错位) */
    char shape[SXCL_KEYMAP_ENUM_MAX];     /**< round / square / pill */
    double opacity;
    char group[SXCL_KEYMAP_ENUM_MAX];     /**< left / right / center(换手用) */
    char alias_of[SXCL_KEYMAP_ID_MAX];    /**< 别名按钮:与某个动作等价(如"盾"=右键"放") */
    sxcl_keymap_binding events[SXCL_KEYMAP_EVENT_COUNT];
    unsigned int event_mask;              /**< 第 i 位 = 第 i 个事件有绑定 */
} sxcl_keymap_button;

typedef struct sxcl_keymap_direction {
    char id[SXCL_KEYMAP_ID_MAX];
    char label[SXCL_KEYMAP_LABEL_MAX];
    char hint[SXCL_KEYMAP_HINT_MAX];
    double x, y, w, h;
    char style[SXCL_KEYMAP_ENUM_MAX];     /**< dpad / rocker / dpad_compact */
    double opacity;
    double dead_zone;                     /**< 死区(手指在里面不算动) */
    char group[SXCL_KEYMAP_ENUM_MAX];
    char up[SXCL_KEYMAP_KEY_MAX];
    char down[SXCL_KEYMAP_KEY_MAX];
    char left[SXCL_KEYMAP_KEY_MAX];
    char right[SXCL_KEYMAP_KEY_MAX];
    char sprint_key[SXCL_KEYMAP_KEY_MAX]; /**< 推到底 + 该键 = 疾跑 */
} sxcl_keymap_direction;

typedef struct sxcl_keymap_layout {
    char schema[SXCL_KEYMAP_SCHEMA_MAX];
    char name[SXCL_KEYMAP_NAME_MAX];
    char screen[SXCL_KEYMAP_SCREEN_MAX];
    char mc_version[SXCL_KEYMAP_VERSION_MAX];
    char description[SXCL_KEYMAP_DESC_MAX];
    char *meta_json;                      /**< malloc 的 meta 原文(没有就是 NULL);原样保留,不认识也不丢 */
    sxcl_keymap_button *buttons;
    size_t button_count;
    size_t button_cap;
    sxcl_keymap_direction *directions;
    size_t direction_count;
    size_t direction_cap;
} sxcl_keymap_layout;

/* ── 校验 / 冲突结果 ── */

#define SXCL_KEYMAP_LEVEL_WARNING 0
#define SXCL_KEYMAP_LEVEL_ERROR   1

typedef struct sxcl_keymap_issue {
    int level;                                   /**< 0=warning 1=error */
    char message[SXCL_KEYMAP_ISSUE_TEXT_MAX];    /**< 人话(界面直接显示,前面自己加 ✗ / ⚠) */
    char control_id[SXCL_KEYMAP_ID_MAX];         /**< 相关的控件 id(没有就是空串) */
} sxcl_keymap_issue;

typedef struct sxcl_keymap_issues {
    sxcl_keymap_issue items[SXCL_KEYMAP_ISSUE_MAX];
    size_t count;
    size_t dropped;                              /**< 超过上限被丢掉的条数 */
} sxcl_keymap_issues;

/** 清空(每次校验前调一次;不调的话是往后面追加)。 */
void sxcl_keymap_issues_reset(sxcl_keymap_issues *issues);
/** 有没有 error 级结论(= 界面应当拦住"设为当前布局")。 */
int sxcl_keymap_issues_has_error(const sxcl_keymap_issues *issues);

/* ── 布局对象 ── */

void sxcl_keymap_layout_init(sxcl_keymap_layout *layout);
void sxcl_keymap_layout_free(sxcl_keymap_layout *layout);
/** 深拷贝(含 meta)。dst 必须是**空的/刚 init 的**:本函数不会先 free dst 里已有的内容,
 *  直接覆盖会造成泄漏(要复用同一个对象就先 sxcl_keymap_layout_free)。 */
int sxcl_keymap_clone(const sxcl_keymap_layout *src, sxcl_keymap_layout *dst);

/** 追加一个按钮/方向(值拷贝)。返回 SXCL_KEYMAP_OK 或 ERR_ARG/ERR_NOMEM。 */
int sxcl_keymap_add_button(sxcl_keymap_layout *layout, const sxcl_keymap_button *button);
int sxcl_keymap_add_direction(sxcl_keymap_layout *layout, const sxcl_keymap_direction *direction);

/** 给某个按钮绑一个事件(action/behavior 可空 = 用默认;keys 可为空)。 */
int sxcl_keymap_bind(sxcl_keymap_layout *layout, const char *button_id, sxcl_keymap_event event,
                     const char *action, const char *behavior,
                     const char *const *keys, size_t key_count);

/** 按 id 找控件(先方向后按钮,与安卓端 controls() 的顺序一致);找不到返回 NULL。 */
const sxcl_keymap_button *sxcl_keymap_button_by_id(const sxcl_keymap_layout *layout, const char *id);
const sxcl_keymap_direction *sxcl_keymap_direction_by_id(const sxcl_keymap_layout *layout, const char *id);

/** 控件总数(方向 + 按钮)。 */
size_t sxcl_keymap_control_count(const sxcl_keymap_layout *layout);

/** 某个事件上的绑定;没绑返回 NULL。 */
const sxcl_keymap_binding *sxcl_keymap_button_event(const sxcl_keymap_button *button, sxcl_keymap_event event);

/** 该按钮用到的全部按键(去重);写进 keys(最多 cap 个),返回**总个数**。 */
size_t sxcl_keymap_button_keys(const sxcl_keymap_button *button, const char *keys[], size_t cap);

/** meta 里的键(重新解析一次 meta 原文,取完即用;meta 空/解析失败就返回默认值)。 */
int sxcl_keymap_meta_bool(const sxcl_keymap_layout *layout, const char *key, int def);
int sxcl_keymap_meta_string(const sxcl_keymap_layout *layout, const char *key, char *out, size_t out_len);

/* ── 解析 / 序列化 ── */

/** 解析 sxcl.keymap.v1 文本。issues 可空(非空时会收到"哪一项非法、被怎么处理了"的告警)。
 *  失败返回 ERR_FORMAT/ERR_ARG/ERR_NOMEM,并把原因写进 err(可空)。
 *  **认不出来的字段一律丢掉,但不会因为某个字段不对就整份失败** —— 能读多少读多少。 */
int sxcl_keymap_parse(const char *text, size_t len, sxcl_keymap_layout *out,
                      sxcl_keymap_issues *issues, char *err, size_t err_len);

/** 读文件(UTF-8)。 */
int sxcl_keymap_load_file(const char *path, sxcl_keymap_layout *out, sxcl_keymap_issues *issues,
                          char *err, size_t err_len);

/** 序列化成 JSON 文本(2 空格缩进,末尾不写换行;UTF-8 原样,中文不转义)。
 *  成功时 *out_text 是 malloc 的,调用方 free。 */
int sxcl_keymap_to_json(const sxcl_keymap_layout *layout, char **out_text, char *err, size_t err_len);

/** 原子写文件(先写 <path>.tmp 再改名;目录不存在会先建)。 */
int sxcl_keymap_save_file(const sxcl_keymap_layout *layout, const char *path, char *err, size_t err_len);

/* ── 校验 / 冲突 / 搜索 ── */

/** 结构校验(对应安卓端 KeymapLayout.validate(),再补几条非法的项):
 *    * 没有 id 的控件 / id 重复;
 *    * 坐标越界(x+w>1.001 或 y+h>1.001;负数也算);
 *    * 按钮一个事件都没绑;
 *    * 解析时被归一过的枚举串(shape/style/behavior)与"键名不像键名"的告警。
 *  返回 error 级结论的条数。 */
size_t sxcl_keymap_validate(const sxcl_keymap_layout *layout, sxcl_keymap_issues *issues);

/** 冲突检测(对应安卓端 conflicts(),FCL 没有这个能力):
 *    1) 同一个按键被不同动作的控件抢(同控件按下+长按同键、别名按钮不算);
 *    2) 两个控件位置重叠超过 60%(触屏容易误触);
 *    3) 缺关键动作:jump / inventory,以及没有方向控件时的 forward。
 *  返回新增的结论条数。 */
size_t sxcl_keymap_conflicts(const sxcl_keymap_layout *layout, sxcl_keymap_issues *issues);

/** 搜索命中的控件 id 列表。 */
typedef struct sxcl_keymap_hits {
    char ids[SXCL_KEYMAP_HITS_MAX][SXCL_KEYMAP_ID_MAX];
    size_t count;
    size_t dropped;   /**< 超过上限被丢掉的条数 */
} sxcl_keymap_hits;

/** 模糊搜索:控件 id / 标签 / 提示 / 按键名 / 动作名 / 方向键名 里含 query 的都算命中。
 *  对应安卓端 KeymapLayout.find()。返回命中个数(也写进 hits->count)。 */
size_t sxcl_keymap_find(const sxcl_keymap_layout *layout, const char *query, sxcl_keymap_hits *hits);

/** 动作 -> 拥有它的控件 id(安卓端 actionIndex());"这个键在哪"搜索用。
 *  写进 ids(最多 cap 个),返回**总个数**。 */
size_t sxcl_keymap_action_owners(const sxcl_keymap_layout *layout, const char *action,
                                 char (*ids)[SXCL_KEYMAP_ID_MAX], size_t cap);

/* ── 内置预设(与 Python src/core/keymap/presets.py 逐字段一致) ── */

size_t sxcl_keymap_preset_count(void);
/** 第 index 个预设的 key(minimal/survival/building/pvp/one_hand);越界返回 NULL。 */
const char *sxcl_keymap_preset_key(size_t index);
/** 第 index 个预设的中文名("极简（推荐新手）"/"生存"/"建造"/"对战"/"单手")。 */
const char *sxcl_keymap_preset_label(size_t index);
/** 这个预设实际用的屏幕方向:one_hand 固定 portrait,别的跟着 screen 走。 */
const char *sxcl_keymap_preset_screen(const char *key, const char *screen);

/** 造一份预设布局(equiv: presets.build_preset(name, screen, mc_version));
 *  认不出的 key 回落到 minimal。用完 sxcl_keymap_layout_free。 */
int sxcl_keymap_build_preset(const char *key, const char *screen, const char *mc_version,
                             sxcl_keymap_layout *out);

/** 推荐一个预设(recommend_preset):竖屏 -> one_hand;没选版本 -> minimal;否则 survival。 */
const char *sxcl_keymap_recommend_preset(const char *mc_version, const char *screen);

/* ── 布局存取(对应 Python src/core/keymap/store.py) ──
 *
 * 目录结构(与 store.py 逐字一致,桌面端与安卓端共用同一份文件):
 *     {keymaps}/preset-minimal.json    内置预设(ensure_presets 首次运行落盘,可覆盖)
 *     {keymaps}/my-pvp.json            用户自己存的
 *     {keymaps}/active.json            {"active": "my-pvp"} —— 安卓端启动时读它
 *   {keymaps} = platform.py 的 default_config_directory("SilentXCraftLauncher")/keymaps:
 *     Windows  %APPDATA%/SilentXCraftLauncher/keymaps
 *     macOS    ~/Library/Application Support/SilentXCraftLauncher/keymaps
 *     Linux    ${XDG_CONFIG_HOME:-~/.config}/silentxcraftlauncher/keymaps
 *
 * 所有函数的第一个参数 dir 都是"keymaps 目录"的可选覆盖:
 *    NULL/空串 = 用上面的平台默认目录(界面就这么用,与 Python 完全同一条路径);
 *    给了目录   = 只用它(单元测试靠它把文件写进构建目录,绝不碰用户的真实配置)。
 * 目录不存在会自动建(store.py 的 keymap_dir() 也这么干)。
 *
 * 与 Python 的两处**有意**差异(都是安全加固,不是格式差异):
 *    * key 里带路径分隔符(../、子目录)一律拒绝 —— store.py 会照原样拼路径,
 *      C 版不让 UI 之外的输入有机会写到 keymaps/ 外面去;
 *    * 文件名安全化在 C 里按**码点**近似 str.isalnum():ASCII 字母数字与 -_ 保留,
 *      拉丁扩展/希腊/西里尔/假名/CJK/谚文/全角字母数字这些常用文字范围也保留,
 *      标点(含全角括号)与符号一律丢掉 —— 中文布局名两边都能用、规则也对得上;
 *      只有极冷门文字(如藏文)可能被丢掉,且只影响文件名、不影响文件内容。
 */

#define SXCL_KEYMAP_STORE_KEY_MAX   64    /* "my-pvp" / "preset-minimal" */
#define SXCL_KEYMAP_STORE_FILE_MAX  128   /* "preset-one_hand.json" */
#define SXCL_KEYMAP_STORE_PATH_MAX  640   /* 完整路径(UTF-8,中文一个 3 字节) */
#define SXCL_KEYMAP_STORE_LIST_MAX  64    /* 一次最多列出几份布局(超了记 dropped) */

/** 目录里的一份布局(list_layouts() 的一项)。字段全部由模块填好,调用方只读。 */
typedef struct sxcl_keymap_store_entry {
    char file[SXCL_KEYMAP_STORE_FILE_MAX];    /**< 文件名 "my-pvp.json" */
    char key[SXCL_KEYMAP_STORE_KEY_MAX];      /**< 去掉 .json 与 preset- 前缀的 key */
    char name[SXCL_KEYMAP_NAME_MAX];          /**< JSON 里的 name;读不出来用文件名 */
    char label[SXCL_KEYMAP_NAME_MAX];         /**< 内置预设的中文名;否则与 name 相同 */
    char screen[SXCL_KEYMAP_SCREEN_MAX];      /**< landscape / portrait */
    size_t buttons;                           /**< JSON 里 buttons 的条数 */
    size_t directions;                        /**< JSON 里 directions 的条数 */
    int builtin;                              /**< 1 = 文件名以 preset- 开头(内置,删不掉) */
} sxcl_keymap_store_entry;

/** 一次列目录的清单。**零分配**:定长数组,可以放栈上。 */
typedef struct sxcl_keymap_store_catalog {
    sxcl_keymap_store_entry items[SXCL_KEYMAP_STORE_LIST_MAX];
    size_t count;      /**< 有效条数(已按文件名排序) */
    size_t dropped;    /**< 超过上限被丢掉的条数(不静默) */
} sxcl_keymap_store_catalog;

/** keymaps 目录(store.py 的 keymap_dir()):dir 为空用平台默认;会顺手建好目录。
 *  写不进 out 返回 SXCL_KEYMAP_ERR_SPACE,连默认目录都拼不出来返回 ERR_IO(err 里给人话)。 */
int sxcl_keymap_store_dir(const char *dir, char *out, size_t out_len, char *err, size_t err_len);

/** 列出所有布局(store.py 的 list_layouts()):*.json、按文件名排序、跳过 active.json。
 *  读不出来的文件跳过(不整份失败),原因记在 err 里(可空;多条时只留最后一条)。 */
int sxcl_keymap_store_list(const char *dir, sxcl_keymap_store_catalog *out, char *err, size_t err_len);

/** 加载一份布局(store.py 的 load()):key 可带可不带 .json;
 *  key 是预设名而文件不在时先 ensure_presets() 再找(store.py 的行为)。
 *  找不到返回 ERR_IO(err 里说清找的是哪个路径),JSON 坏了返回 ERR_FORMAT。 */
int sxcl_keymap_store_load(const char *dir, const char *key, sxcl_keymap_layout *out,
                           sxcl_keymap_issues *issues, char *err, size_t err_len);

/** 保存一份布局(store.py 的 save()):key 空 -> 用 layout->name -> 兜底 "layout";
 *  文件名安全化后再写,重名直接覆盖。成功时 out_path 收到**完整路径**(可空)。
 *  out_path 缓冲不够返回 ERR_SPACE。 */
int sxcl_keymap_store_save(const char *dir, const sxcl_keymap_layout *layout, const char *key,
                           char *out_path, size_t out_len, char *err, size_t err_len);

/** 删除一份布局(store.py 的 delete()):1 = 删掉了,0 = 没有这份 / 是内置预设(不许删)。 */
int sxcl_keymap_store_delete(const char *dir, const char *key);

/** 当前启用的布局 key(store.py 的 active_name()):
 *  没有 active.json 返回 "preset-minimal";JSON 坏了返回 ""(与 store.py 一致)。 */
int sxcl_keymap_store_active(const char *dir, char *out, size_t out_len);

/** 设为当前布局(store.py 的 set_active()):写 active.json({ "active": "<key>" } 两空格缩进);
 *  key 前面的 "preset-" 会去掉。成功时 out_path 收到 active.json 的完整路径(可空)。 */
int sxcl_keymap_store_set_active(const char *dir, const char *key, char *out_path, size_t out_len,
                                 char *err, size_t err_len);

/** 首次运行把 5 套内置预设落盘(store.py 的 ensure_presets()):
 *  overwrite=0 时已经存在的文件不动。返回**这次写了几份**;
 *  written 可空(非空时填 file/key/name/label,方便界面说"写了哪几份")。 */
size_t sxcl_keymap_store_ensure_presets(const char *dir, int overwrite, sxcl_keymap_store_catalog *written,
                                        char *err, size_t err_len);

/* ── FCL 格式互转(降低"从 FCL 换过来"的门槛) ── */

/** FCL 的 GLFW 键码 -> 我们的键名(32 -> "KEY_SPACE"、-1 -> "MOUSE_LEFT");认不出来给 "KEY_<码>"。 */
const char *sxcl_keymap_key_from_glfw(int code, char *out, size_t out_len);
/** 各种写法 -> 我们的键名("空格"/"space"/"右键"/"e"/"KEY_W"/数字);空 -> ""。 */
const char *sxcl_keymap_key_from_text(const char *text, char *out, size_t out_len);
/** 键 -> 默认动作(KEY_SPACE -> jump、MOUSE_LEFT -> attack…);没有对应给 ""。 */
const char *sxcl_keymap_action_for_key(const char *key);

/** 导入 FCL 布局(像素坐标 + GLFW 键码;screen_w/screen_h 用来归一化,默认 2400x1080)。
 *  FCL 那份原始 JSON 会原样塞进 layout.meta.fcl_raw(fcl.py 的做法),方便对照排查。 */
int sxcl_keymap_import_fcl(const char *json_text, size_t len, const char *name,
                           double screen_w, double screen_h, sxcl_keymap_layout *out,
                           sxcl_keymap_issues *issues, char *err, size_t err_len);

/** 导出成 FCL 风格的视图列表(像素坐标)。成功时 *out_text 是 malloc 的。 */
int sxcl_keymap_to_fcl(const sxcl_keymap_layout *layout, double screen_w, double screen_h,
                       char **out_text, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_KEYMAP_H */
