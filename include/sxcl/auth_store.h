/* SXCL-C 登录令牌的加密落盘 —— refresh token 是"免密续期"的唯一凭据,等价于密码。
 *
 * 硬规矩:**文件里绝不放明文 token**。存储后端按平台挑,挑不到强后端就走降级路径并且
 * 由 sxcl_auth_store_backend() 如实报出来,让界面/文档能告诉用户风险。
 *
 *   后端              平台        说明
 *   ───────────────────────────────────────────────────────────────────────────
 *   DPAPI            Windows     CryptProtectData,密钥由用户登录凭据派生(换用户/换机器解不开)
 *   Keychain         macOS       SecItemAdd / SecItemCopyMatching(kSecClassGenericPassword)
 *   libsecret        Linux       运行时 dlopen libsecret-1(GNOME Keyring 等),找不到就降级
 *   文件 + 本机密钥   全部         0600 权限文件 + ChaCha20-Poly1305,密钥 = SHA256(本机派生材料 + 盐)
 *
 * 降级路径的风险(必须如实说明):"本机派生密钥"只能防"拷走文件到别的机器解密",
 * 防不住"同一台机器上的其它程序读走密钥文件再解密" —— 它挡的是离线窃取与误提交,
 * 不是同机恶意软件。所以能上 DPAPI/Keychain/libsecret 就一定上。
 *
 * 文件格式(小端,固定头 + 后端自描述载荷;二进制,不是 JSON):
 *   magic   8 字节 "SXCLAUTH"
 *   version 1 字节 (= 1)
 *   backend 1 字节 (sxcl_auth_store_kind)
 *   flags   2 字节 (保留,= 0;将来做"多账户/多 profile"时用)
 *   length  4 字节 (载荷长度)
 *   payload …
 * 载荷内部再是后端自己的格式(DPAPI 是 DATA_BLOB;文件后端是 nonce||密文||tag)。
 * 头部同时作为 AEAD 的附加认证数据(AAD),所以改头会导致解密失败,而不是静默读出错数据。
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
