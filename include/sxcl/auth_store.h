/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_AUTH_STORE_H
#define SXCL_AUTH_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/auth.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXCL_AUTH_STORE_MAGIC   "SXCLAUTH"
#define SXCL_AUTH_STORE_VERSION 1
#define SXCL_AUTH_STORE_PATH_MAX 640

typedef enum sxcl_auth_store_kind {
    SXCL_AUTH_STORE_NONE = 0,   /* 没有任何可用后端(不该发生:文件后端总是可用) */
    SXCL_AUTH_STORE_DPAPI = 1,  /* Windows */
    SXCL_AUTH_STORE_KEYCHAIN = 2, /* macOS */
    SXCL_AUTH_STORE_LIBSECRET = 3, /* Linux(GNOME Keyring 等) */
    SXCL_AUTH_STORE_FILE_KEY = 4   /* 降级:0600 文件 + 本机派生密钥 + ChaCha20-Poly1305 */
} sxcl_auth_store_kind;

/** 本机实际会用的后端(不改文件、不留副作用)。 */
sxcl_auth_store_kind sxcl_auth_store_backend(void);
/** **只给测试用**:强制指定后端(传 SXCL_AUTH_STORE_NONE 恢复"自动挑")。
 *  用来在没有 Keychain/DPAPI 的环境里也能把每条加密路径跑一遍。 */
void sxcl_auth_store_set_backend_for_test(sxcl_auth_store_kind kind);
/** 后端短名("dpapi"/"keychain"/"libsecret"/"file+key"),静态串。 */
const char *sxcl_auth_store_kind_name(sxcl_auth_store_kind kind);
/** 后端人话说明 + 风险(给 CLI/界面展示;静态串)。 */
const char *sxcl_auth_store_kind_note(sxcl_auth_store_kind kind);

/** 默认配置目录(与 keymap_store 的 platform.py 分支一致):
 *  Windows %APPDATA%/SilentXCraftLauncher;macOS ~/Library/Application Support/SilentXCraftLauncher;
 *  Linux $XDG_CONFIG_HOME/silentxcraftlauncher 或 ~/.config/silentxcraftlauncher。 */
int sxcl_auth_config_dir(char *out, size_t out_len, char *err, size_t err_len);

/** 默认令牌文件路径:<配置目录>/auth/tokens.bin
 *  (Windows %APPDATA%/SilentXCraftLauncher;macOS ~/Library/Application Support/…;
 *   Linux $XDG_CONFIG_HOME 或 ~/.config)。失败返回负错误码并写 err。 */
int sxcl_auth_store_default_path(char *out, size_t out_len, char *err, size_t err_len);

/** 加密保存(覆盖)。父目录自动建。成功返回 SXCL_AUTH_OK。 */
int sxcl_auth_store_save(const char *path, const sxcl_auth_session *session,
                         char *err, size_t err_len);
/** 读取解密。文件不存在返回 SXCL_AUTH_ERR_STORE 且 err 里说明"还没登录"。 */
int sxcl_auth_store_load(const char *path, sxcl_auth_session *session, char *err, size_t err_len);
/** 删除令牌文件(退出登录)。文件不存在也算成功。 */
int sxcl_auth_store_clear(const char *path, char *err, size_t err_len);
/** 令牌文件是否存在(1/0)。 */
int sxcl_auth_store_exists(const char *path);

/** 会话 → JSON(明文,**只在内存里用**;测试断言与"换后端迁移"用,绝不许直接落盘)。 */
int sxcl_auth_session_to_json(const sxcl_auth_session *session, char **out, size_t *out_len);
/** JSON → 会话(容忍缺字段)。 */
int sxcl_auth_session_from_json(const char *json, size_t len, sxcl_auth_session *out);

/** 存储里用的本机派生密钥材料(测试可覆盖目录)。key_out 必须 32 字节。
 *  slat_dir 空 = 用默认配置目录(盐文件放那儿:同一个用户+同一台机器才解不开)。 */
int sxcl_auth_store_machine_key(const char *salt_dir, unsigned char key_out[32],
                                char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_AUTH_STORE_H */
