/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_AUTH_H
#define SXCL_AUTH_H

#include <stddef.h>
#include <stdint.h>

#include "sxcl/net.h"
#include "sxcl/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 返回码(负数为错,与 fs/net/http 的约定一致) ── */
#define SXCL_AUTH_OK                0
#define SXCL_AUTH_ERR_ARG        (-1)  /* 参数不合法(空指针/缓冲不够) */
#define SXCL_AUTH_ERR_NET        (-2)  /* 连不上/超时/TLS:连响应都没拿到 */
#define SXCL_AUTH_ERR_HTTP       (-3)  /* 拿到了响应,但不是预期状态码 */
#define SXCL_AUTH_ERR_JSON       (-4)  /* 响应不是合法 JSON / 缺字段 */
#define SXCL_AUTH_ERR_DENIED     (-5)  /* 用户拒绝授权(authorization_declined / 页面点了拒绝) */
#define SXCL_AUTH_ERR_EXPIRED    (-6)  /* 设备码/授权码/refresh token 过期,必须重新登录 */
#define SXCL_AUTH_ERR_TIMEOUT    (-7)  /* 等用户操作超时(环回等 code / 设备码有效期到) */
#define SXCL_AUTH_ERR_CANCELLED  (-8)  /* 调用方主动取消 */
#define SXCL_AUTH_ERR_XBOX       (-9)  /* Xbox Live 用户认证或 XSTS 失败(人话见 err,XErr 见会话) */
#define SXCL_AUTH_ERR_NO_PROFILE (-10) /* 登录成功但**没有 Java 版档案**(name 为空 = 没买) */
#define SXCL_AUTH_ERR_NO_ENTITLE (-11) /* 权益接口明确说没有这份游戏(与上一条分开报) */
#define SXCL_AUTH_ERR_STORE      (-12) /* 加密存储读写失败 */
#define SXCL_AUTH_ERR_UNSUPPORTED (-13)/* 平台不支持(如没有可用的环回套接字实现) */
#define SXCL_AUTH_ERR_NOMEM      (-14) /* 内存不足 */
#define SXCL_AUTH_ERR_RATE_LIMIT (-15) /* 被限流(429),稍后重试 */

/* ── 尺寸上限(够用就好:MS access_token ~1.5KB,XSTS ~2KB,刷新后可能更长) ── */
#define SXCL_AUTH_TOKEN_MAX   8192
#define SXCL_AUTH_ERROR_MAX   512
#define SXCL_AUTH_CLIENT_ID_MAX 128
#define SXCL_AUTH_NAME_MAX    64
#define SXCL_AUTH_UUID_MAX    40
#define SXCL_AUTH_SCOPE_MAX   256
#define SXCL_AUTH_URL_MAX     1024
#define SXCL_AUTH_MESSAGE_MAX 512

/* ── 端点(全部固定,别在别处再抄一份) ──
 *
 * 微软那三个端点(授权/令牌/设备码)带一个**租户段**,它必须和"应用的受支持账户类型"对上:
 *   应用注册成"仅个人 Microsoft 帐户" → consumers
 *   应用注册成"任何组织目录 + 个人帐户" → common
 *   单租户应用 → 用它自己的租户 id/域名(这时 consumers 会被拒)
 * 所以租户段做成可配置(默认 consumers),配错的典型报错是 AADSTS50059 / AADSTS500011 /
 * AADSTS700016,见 sxcl_auth_aadsts_hint() 与 docs/09-正版登录.md 的排查表。 */
#define SXCL_AUTH_AUTHORITY_HOST    "https://login.microsoftonline.com"
#define SXCL_AUTH_TENANT_DEFAULT    "consumers"
#define SXCL_AUTH_TENANT_MAX        128
/* 默认租户下的三个端点(直接拼好的字面量,便于测试断言与文档引用) */
#define SXCL_AUTH_URL_AUTHORIZE     SXCL_AUTH_AUTHORITY_HOST "/consumers/oauth2/v2.0/authorize"
#define SXCL_AUTH_URL_TOKEN         SXCL_AUTH_AUTHORITY_HOST "/consumers/oauth2/v2.0/token"
#define SXCL_AUTH_URL_DEVICECODE    SXCL_AUTH_AUTHORITY_HOST "/consumers/oauth2/v2.0/devicecode"
#define SXCL_AUTH_URL_XBL_USER      "https://user.auth.xboxlive.com/user/authenticate"
#define SXCL_AUTH_URL_XSTS          "https://xsts.auth.xboxlive.com/xsts/authorize"
#define SXCL_AUTH_URL_MC_LOGIN      "https://api.minecraftservices.com/authentication/login_with_xbox"
#define SXCL_AUTH_URL_MC_ENTITLEMENTS "https://api.minecraftservices.com/entitlements/mcstore"
#define SXCL_AUTH_URL_MC_PROFILE    "https://api.minecraftservices.com/minecraft/profile"
#define SXCL_AUTH_URL_BEDROCK_AUTH  "https://multiplayer.minecraft.net/authentication"

#define SXCL_AUTH_SCOPE             "XboxLive.signin offline_access"
#define SXCL_AUTH_RP_XBOX           "http://auth.xboxlive.com"
#define SXCL_AUTH_RP_MINECRAFT      "rp://api.minecraftservices.com/"
#define SXCL_AUTH_RP_BEDROCK        "https://multiplayer.minecraft.net/"
#define SXCL_AUTH_SANDBOX           "RETAIL"

/* 默认 client_id:本项目的 Azure 应用 "Silent X Craft Launcher"(公共客户端,无 secret;
 * Azure 里注册为"移动和桌面应用程序",允许 http://localhost 环回重定向)。
 * 换成别的应用(含社区公共 id 00000000402b5328,排障时常用来对照):
 *   优先级 = 命令行/API 显式参数 > 环境变量 SXCL_AUTH_CLIENT_ID > sxcl_settings["auth.client_id"] > 这个默认值。
 * 仓库里**只有 client_id**(它本来就是公开信息),绝不放 client_secret —— 公共客户端也不该有。 */
#define SXCL_AUTH_DEFAULT_CLIENT_ID "aa4e81c8-f550-4720-bc51-c9b126f456d1"

/* 社区公共 client_id(少数启动器的历史默认值)。**仅作为可选项保留**:排障时用它做对照,
 * 可以快速区分"微软那边/账号的问题"还是"我们应用注册的问题"。 */
#define SXCL_AUTH_COMMUNITY_CLIENT_ID "00000000402b5328"

/** 环境变量名(CLI/UI 都用这一套,别再各起一套)。 */
#define SXCL_AUTH_ENV_CLIENT_ID "SXCL_AUTH_CLIENT_ID"
#define SXCL_AUTH_ENV_TENANT    "SXCL_AUTH_TENANT"
/** sxcl_settings 里的键名。 */
#define SXCL_AUTH_SETTINGS_CLIENT_ID "auth.client_id"
#define SXCL_AUTH_SETTINGS_TENANT    "auth.tenant"

/* ── XSTS 错误码(微软只给数字,人话由我们给;其它码原样带出) ── */
#define SXCL_AUTH_XERR_BANNED           2148916227ULL /* 账号已被 Xbox Live 封禁 */
#define SXCL_AUTH_XERR_ADULT_VERIFY_KR  2148916229ULL /* 需要成人验证(韩国) */
#define SXCL_AUTH_XERR_NO_XBOX_ACCOUNT  2148916233ULL /* 该账号没有 Xbox 档案(需先去 xbox.com 创建) */
#define SXCL_AUTH_XERR_REGION           2148916235ULL /* 所在地区不支持 Xbox Live */
#define SXCL_AUTH_XERR_ADULT_VERIFY     2148916236ULL /* 需要成人验证(韩国) */
#define SXCL_AUTH_XERR_ADULT_VERIFY2    2148916237ULL /* 需要成人验证(韩国) */
#define SXCL_AUTH_XERR_CHILD            2148916238ULL /* 未成年账号,需要成人加入家庭组同意 */

/* 微软登录端点带租户段:登录链要用到的三个端点在这里按租户拼(别自己拼字符串)。 */
typedef enum sxcl_auth_endpoint {
    SXCL_AUTH_EP_AUTHORIZE = 0,
    SXCL_AUTH_EP_TOKEN = 1,
    SXCL_AUTH_EP_DEVICECODE = 2
} sxcl_auth_endpoint;

/** 拼端点 URL。tenant 为空用 SXCL_AUTH_TENANT_DEFAULT。装不下返回 SXCL_AUTH_ERR_ARG。 */
int sxcl_auth_endpoint_url(sxcl_auth_endpoint endpoint, const char *tenant,
                           char *out, size_t out_len);

/** AADSTS 错误码 → 中文排查建议(认不出返回 NULL)。这些码来自 error_description,
 *  例如 "AADSTS50059: No tenant-identifying information found..."。
 *  最有用的几条:
 *    50059 / 500011 / 700016 → **应用的受支持账户类型 / 租户段没对上**(不是代码问题)
 *    70008 / 700082 / 50173  → refresh token 过期或被撤销,要重新登录
 *    65001 / 65004          → 用户/管理员没同意
 *    9002325 / 900971 / 9002313 → 公共客户端必须用 PKCE / 重定向不匹配 */
const char *sxcl_auth_aadsts_hint(int64_t aadsts_code);

/** 从微软的 error_description 里抠出第一个 AADSTS 码;没有返回 0。 */
int64_t sxcl_auth_aadsts_extract(const char *text);

/** XSTS 错误码 → 中文人话(认不出返回 NULL,调用方走"其它码原样带出")。返回静态串,勿 free。 */
const char *sxcl_auth_xsts_error_text(uint64_t xerr);
/** XSTS 错误码 → 完整人话("该微软账号还没有 Xbox 档案…(XErr 2148916233)")。返回写入字节数。 */
int sxcl_auth_xsts_error_message(uint64_t xerr, char *out, size_t out_len);

/** 令牌打码:只留前 6 位 + 长度("eyJ0eX…(共 1234 字节)")。给日志/CLI 用,**绝不打全文**。
 *  token 为空 → "(无)";out_len 不够就截断。返回写入字节数。 */
int sxcl_auth_mask_token(const char *token, char *out, size_t out_len);

/* ── 会话:一次登录链的全部产物 ── */
typedef struct sxcl_auth_ms_tokens {
    char access_token[SXCL_AUTH_TOKEN_MAX];
    char refresh_token[SXCL_AUTH_TOKEN_MAX]; /* 只有 scope 带 offline_access 才有 */
    char id_token[SXCL_AUTH_TOKEN_MAX];      /* 可选,可能为空 */
    char token_type[32];                     /* 一般是 "Bearer" */
    char scope[SXCL_AUTH_SCOPE_MAX];
    int64_t issued_at;    /* Unix 秒(本地时钟) */
    int64_t expires_at;   /* Unix 秒 = issued_at + expires_in;过期判断只看它 */
} sxcl_auth_ms_tokens;

typedef struct sxcl_auth_xbox_tokens {
    char user_hash[SXCL_AUTH_NAME_MAX];       /* uhs,login_with_xbox 的 identityToken 要用 */
    char xsts_token[SXCL_AUTH_TOKEN_MAX];
    int64_t xsts_expires_at;                  /* Unix 秒(XSTS 的 NotAfter) */
    char gamertag[SXCL_AUTH_NAME_MAX];        /* 只有部分响应带,可空 */
    uint64_t xuid;
} sxcl_auth_xbox_tokens;

/** Java 版链的产物(**与基岩版各自独立**) */
typedef struct sxcl_auth_minecraft_tokens {
    char access_token[SXCL_AUTH_TOKEN_MAX];
    int64_t expires_at;              /* Unix 秒 */
    char uuid[SXCL_AUTH_UUID_MAX];   /* 32 位无横线 */
    char name[SXCL_AUTH_NAME_MAX];   /* **空 = 这个账号没有 Java 版**,必须如实报错,不许继续 */
    int entitlement_count;           /* mcstore 里的条目数(0 = 没买) */
    int entitlements_checked;        /* 1 = mcstore 真的查过了(不是"没查") */
    int profile_checked;             /* 1 = profile 真的查过了 */
} sxcl_auth_minecraft_tokens;

/** 基岩版链的产物:同一份微软/Xbox 凭据派生,**权益与令牌独立** */
typedef struct sxcl_auth_bedrock_tokens {
    char chain[3][SXCL_AUTH_TOKEN_MAX]; /* /authentication 返回的证书链(通常 1~3 段 JWT) */
    int chain_count;
    char identity_public_key[256];      /* 本次请求用的 P-384 身份公钥(base64,未压缩点) */
    int entitlement_checked;            /* 基岩的权益要单独查(Java 买了不代表基岩买了) */
    int entitled;
} sxcl_auth_bedrock_tokens;

/** 一次登录的完整会话。**零动态分配**:可以直接放栈上/塞进设置。 */
typedef struct sxcl_auth_session {
    char client_id[SXCL_AUTH_CLIENT_ID_MAX];
    char tenant[SXCL_AUTH_TENANT_MAX];      /* 端点租户段(consumers/common/<租户 id>) */
    sxcl_auth_ms_tokens ms;
    sxcl_auth_xbox_tokens xbox;             /* Java 链的 XSTS(中继方 rp://api.minecraftservices.com/) */
    sxcl_auth_minecraft_tokens mc;
    sxcl_auth_xbox_tokens xbox_bedrock;     /* 基岩链的 XSTS(中继方 https://multiplayer.minecraft.net/) */
    sxcl_auth_bedrock_tokens bedrock;
    int64_t last_error_xerr;                /* 最近一次 XSTS 错误码(0 = 无);给界面/CLI 原样展示 */
    char account_name[SXCL_AUTH_NAME_MAX];  /* 展示名:优先 gamertag,其次 mc.name */
} sxcl_auth_session;

/** 会话字段的"有没有"判断(别拿 token 首字节自己判,免得两边判断不一致)。 */
int sxcl_auth_session_has_ms(const sxcl_auth_session *s);
int sxcl_auth_session_has_mc(const sxcl_auth_session *s);

/** ms access_token 是否已过期(留 skew 秒余量)。无 token 也算过期。 */
int sxcl_auth_ms_expired(const sxcl_auth_session *s, int64_t now, int64_t skew_seconds);
/** mc access_token 是否已过期。 */
int sxcl_auth_mc_expired(const sxcl_auth_session *s, int64_t now, int64_t skew_seconds);

/** 当前 Unix 秒。**测试可以覆盖时钟**(见 sxcl_auth_set_now_for_test)。 */
int64_t sxcl_auth_now(void);
/** 只给测试用:把 sxcl_auth_now 固定成固定值(传 -1 恢复真实时钟)。 */
void sxcl_auth_set_now_for_test(int64_t fixed_now);

/* ── 交互回调:把"该用户做的事"交给前端(CLI 打屏、UI 弹窗、测试记下来) ── */
typedef struct sxcl_auth_callbacks {
    /** 设备码流:把 user_code 与网址告诉用户(必须显示,否则没法登录)。
     *  message 是微软给的整句提示(可能含换行),可直接打屏。 */
    void (*on_user_code)(void *userdata, const char *user_code, const char *verification_uri,
                         const char *message);
    /** 设备码流的细节(可选;在 on_user_code 之后**立刻**调用一次):
     *  interval_seconds = 服务端要求的轮询间隔(秒),expires_in_seconds = 设备码有效期(秒)。
     *  给前端用的:让用户知道"这个码能用多久"。 */
    void (*on_device_code_info)(void *userdata, int interval_seconds, int expires_in_seconds);
    /** 授权码流:浏览器该打开哪个地址(默认实现交给系统打开;不想开就自己处理)。 */
    void (*on_open_url)(void *userdata, const char *url);
    /** 进度人话("正在换 token…")。可空。**不许在这里打 token**。 */
    void (*on_status)(void *userdata, const char *message);
    void *userdata;
} sxcl_auth_callbacks;

/** 登录/续期的可选项(整个结构可空 = 全默认)。 */
typedef struct sxcl_auth_opts {
    const char *client_id;   /**< NULL/空 = 按 环境变量 → settings → 默认 的顺序解析 */
    const char *tenant;      /**< NULL/空 = 按 环境变量 → settings → "consumers" 的顺序解析 */
    sxcl_settings *settings; /**< 可空。用来读/写 auth.client_id;有它才能把用户填的 id 记住 */
    int64_t http_timeout_ms; /**< 单跳超时;<= 0 用 30000 */
    int device_code;         /**< 1 = 走设备码流(无浏览器/无环回时的兜底) */
    int no_browser;          /**< 1 = 不自动开浏览器(只把链接交给 on_open_url) */
    int64_t loopback_timeout_ms; /**< 授权码流等用户授权的上限;<= 0 用 300000(5 分钟) */
    int64_t poll_timeout_ms;     /**< 设备码流轮询总上限;<= 0 用微软给的 expires_in */
    /** 返回非 0 = 立刻取消(轮询/等待循环里每次都会问一次;可空)。 */
    int (*should_cancel)(void *userdata);
    sxcl_auth_callbacks cb;
    /** 基岩链的身份公钥(base64 未压缩 P-384 点)。可空 = 由内置 P-384 生成器现场生成。 */
    const char *bedrock_public_key;
} sxcl_auth_opts;

/* ── 单跳 API(测试与"分步调试"用;整链走下面的 login_*) ── */

/** 第 1 步:拼授权码 URL(含 PKCE challenge 与 state)。成功返回 SXCL_AUTH_OK。
 *  verifier 与 challenge 由调用方给(通常是 sxcl_auth_pkce_generate 的产物)。 */
int sxcl_auth_build_authorize_url(const char *client_id, const char *redirect_uri,
                                  const char *code_challenge, const char *state,
                                  char *out, size_t out_len, char *err, size_t err_len);

/** 同上的"可指定租户段"版本(consumers/common/<租户 id>);默认版本用 consumers。 */
int sxcl_auth_build_authorize_url_tenant(const char *client_id, const char *tenant,
                                         const char *redirect_uri, const char *code_challenge,
                                         const char *state, char *out, size_t out_len,
                                         char *err, size_t err_len);

/** 第 1 步(换 token):authorization_code + PKCE。 */
int sxcl_auth_exchange_code(sxcl_transport *tr, const char *client_id, const char *code,
                            const char *redirect_uri, const char *code_verifier,
                            int64_t timeout_ms, sxcl_auth_ms_tokens *out,
                            char *err, size_t err_len);

int sxcl_auth_exchange_code_tenant(sxcl_transport *tr, const char *client_id, const char *tenant,
                                   const char *code, const char *redirect_uri,
                                   const char *code_verifier, int64_t timeout_ms,
                                   sxcl_auth_ms_tokens *out, char *err, size_t err_len);

/** 第 7 步:refresh_token 续期(免密)。refresh_token 会原样保留(微软会轮换,以新值为准)。 */
int sxcl_auth_refresh_ms(sxcl_transport *tr, const char *client_id, const char *refresh_token,
                         int64_t timeout_ms, sxcl_auth_ms_tokens *out, char *err, size_t err_len);

int sxcl_auth_refresh_ms_tenant(sxcl_transport *tr, const char *client_id, const char *tenant,
                                const char *refresh_token, int64_t timeout_ms,
                                sxcl_auth_ms_tokens *out, char *err, size_t err_len);

/** 可指定租户段的设备码流(见上)。 */
int sxcl_auth_device_code_login_tenant(sxcl_transport *tr, const sxcl_auth_opts *opts,
                                       const char *client_id, const char *tenant,
                                       sxcl_auth_ms_tokens *out, char *err, size_t err_len);

/** 兜底:设备码流。先拿 user_code(通过 cb.on_user_code 交出去),再按 interval 轮询到用户完成。 */
int sxcl_auth_device_code_login(sxcl_transport *tr, const sxcl_auth_opts *opts, const char *client_id,
                                sxcl_auth_ms_tokens *out, char *err, size_t err_len);

/** 第 3 步:Xbox Live 用户认证(RpsTicket = "d=<ms access token>")。 */
int sxcl_auth_xbox_user_authenticate(sxcl_transport *tr, const char *ms_access_token,
                                     int64_t timeout_ms, char *xbl_token, size_t xbl_token_len,
                                     char *user_hash, size_t user_hash_len,
                                     int64_t *expires_at, char *err, size_t err_len);

/** 第 4 步:XSTS(中继方由 relying_party 给:Java = rp://api.minecraftservices.com/,
 *  基岩 = https://multiplayer.minecraft.net/)。XErr 会翻成人话写进 err,session 里另存原始码。 */
int sxcl_auth_xsts_authorize(sxcl_transport *tr, const char *xbl_token, const char *relying_party,
                             int64_t timeout_ms, sxcl_auth_xbox_tokens *out, uint64_t *xerr_out,
                             char *err, size_t err_len);

/** 第 5 步:login_with_xbox(identityToken = "XBL3.0 x=<uhs>;<xsts>")。 */
int sxcl_auth_minecraft_login(sxcl_transport *tr, const char *user_hash, const char *xsts_token,
                              int64_t timeout_ms, sxcl_auth_minecraft_tokens *out,
                              char *err, size_t err_len);

/** 第 6 步之一:mcstore 权益。*count_out = items 条数(0 = 没有这份游戏)。 */
int sxcl_auth_minecraft_entitlements(sxcl_transport *tr, const char *mc_access_token,
                                     int64_t timeout_ms, int *count_out, char *err, size_t err_len);

/** 第 6 步之二:档案(uuid/name)。**name 为空 = 没买 Java 版** → 返回 SXCL_AUTH_ERR_NO_PROFILE。 */
int sxcl_auth_minecraft_profile(sxcl_transport *tr, const char *mc_access_token,
                                int64_t timeout_ms, char *uuid, size_t uuid_len,
                                char *name, size_t name_len, char *err, size_t err_len);

/** 基岩链:用同一份微软凭据派生(内部会重跑 XBL + XSTS,中继方换成 multiplayer.minecraft.net)。 */
int sxcl_auth_bedrock_authorize(sxcl_transport *tr, const sxcl_auth_opts *opts,
                                const char *ms_access_token, sxcl_auth_xbox_tokens *xbox_out,
                                sxcl_auth_bedrock_tokens *out, char *err, size_t err_len);

/* ── 整链 API(前端只该用这几个) ── */

/** 登录(授权码主路;opts->device_code=1 时走设备码兜底),成功后 out 里 Java 链是齐的。 */
int sxcl_auth_login(sxcl_transport *tr, const sxcl_auth_opts *opts, sxcl_auth_session *out,
                    char *err, size_t err_len);

/** 刷新:用会话里的 refresh_token 免密重跑(微软 token → XBL → XSTS → MC → 权益/档案)。
 *  refresh token 过期/被撤销 → SXCL_AUTH_ERR_EXPIRED(必须重新登录)。 */
int sxcl_auth_refresh(sxcl_transport *tr, const sxcl_auth_opts *opts, sxcl_auth_session *session,
                      char *err, size_t err_len);

/** 跑基岩链并把结果并进会话(Java 段不动;两段权益各自独立)。 */
int sxcl_auth_session_bedrock(sxcl_transport *tr, const sxcl_auth_opts *opts,
                              sxcl_auth_session *session, char *err, size_t err_len);

/** client_id 解析:显式参数 > 环境变量 SXCL_AUTH_CLIENT_ID > settings["auth.client_id"] > 默认。
 *  settings 可空;解析出的非默认值会写回 settings(若有)。 */
int sxcl_auth_resolve_client_id(const char *explicit_id, sxcl_settings *settings,
                                char *out, size_t out_len);

/** 租户段解析:显式参数 > 环境变量 SXCL_AUTH_TENANT > settings["auth.tenant"] > "consumers"。
 *  只允许 [0-9A-Za-z.-](租户 id / 域名 / consumers / common / organizations),
 *  防止有人把 "/" 之类塞进来改写端点路径。 */
int sxcl_auth_resolve_tenant(const char *explicit_tenant, sxcl_settings *settings,
                             char *out, size_t out_len);

/** 租户段形状校验(见上一条的值域)。 */
int sxcl_auth_tenant_valid(const char *tenant);

/** 校验 client_id 形状(只允许 [0-9A-Za-z._~-],长度 8..SXCL_AUTH_CLIENT_ID_MAX-1)。
 *  用来在发请求之前就把"用户填错了"报出来,而不是等微软回 400。 */
int sxcl_auth_client_id_valid(const char *client_id);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_AUTH_H */
