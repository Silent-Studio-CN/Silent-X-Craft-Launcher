/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>

#include "sxcl/auth.h"
#include "sxcl/auth_store.h"

class QThread;

namespace sxcl::ui {

// 我们自己的设置文件路径(%APPDATA%/SilentXCraftLauncher/settings.conf 等),
// 与 settings_page.cpp 的 settingsFilePath() 同口径。main.cpp 的启动恢复也用它。
QString uiSettingsPath();

// ── 账户状态快照(UI 只读;数据来源 = 核心库的加密令牌文件) ──
struct AccountSnapshot {
    bool tokenFileExists = false;   // 令牌文件在不在(不在 = 从没登录过,不是错误)
    bool loggedIn = false;          // 解出了会话且拿到微软令牌
    bool hasMcToken = false;        // 有 Minecraft access_token
    QString accountName;            // 展示名(gamertag 优先,其次 mc.name)
    QString playerName;             // Java 版玩家名(**空 = 这个账号没有 Java 版**)
    QString uuid;
    int entitlementCount = 0;       // mcstore 条目数(>0 = 有 Java 版权益)
    bool entitlementChecked = false; // 权益真的查过了(不是"没查")
    bool mcExpired = true;          // MC 令牌过期(留了 120s 余量,与 CLI 一致)
    qint64 msExpiresAt = 0;
    qint64 mcExpiresAt = 0;
    QString storeBackend;           // "dpapi" / "keychain" / "libsecret" / "file+key"
    QString storeNote;              // 后端人话 + 风险说明(核心库原文,如实展示)
    QString error;                  // 读取失败原因(核心库 err 原文;空 = 没失败)

    bool javaEntitled() const { return entitlementCount > 0; }
};

// 默认令牌文件路径(sxcl_auth_store_default_path)。拿不到返回空串。
QString defaultTokenPath();

// 读一次账户状态。**会解密令牌文件**(DPAPI/Keychain/…),失败原因原样放在 error 里。
// 注意:本函数会做磁盘 IO + 解密,但**不联网**,几十毫秒级,可在 UI 线程调用。
AccountSnapshot loadAccountSnapshot();

// 能不能直接用这个账户启动正版:microsoft 令牌在、MC 令牌没过期、有玩家名。
// (玩家名取不到就用默认名继续启动是核心库明令禁止的行为,这里跟着一起挡住。)
bool accountCanLaunch(const AccountSnapshot &snapshot);

// ── 后台任务:登录 / 续期 / 注销 ──
// 设备码流为主(不依赖浏览器/环回监听);成功后写回加密令牌文件。
// 信号从工作线程发出,Qt 自动排队到 UI 线程 —— UI 侧 connect 即可,主线程不阻塞。
class AccountTask : public QObject {
    Q_OBJECT
public:
    enum class Operation { Login, Refresh, Logout };

    explicit AccountTask(Operation operation, QObject *parent = nullptr);
    ~AccountTask() override;
    AccountTask(const AccountTask &) = delete;
    AccountTask &operator=(const AccountTask &) = delete;

    void start();
    // 取消:置取消位 + 撤在飞的 HTTP。线程安全(核心库轮询循环会查 should_cancel,
    // 在飞的请求由 transport cancel_all 立刻结束,不用干等 30s 超时)。
    void cancel();
    // 工作线程还在跑吗(关窗时用来决定:直接删,还是等它自己收尾后自删)
    bool running() const;
    Operation operation() const { return m_operation; }

signals:
    // 设备码:8 位 user_code + 验证网址(必须展示给用户,否则没法登录)
    void userCode(const QString &code, const QString &verificationUri);
    // 设备码细节:轮询间隔 / 有效期(秒)。对应 docs/09 的 expires_in / interval。
    void deviceCodeInfo(int intervalSeconds, int expiresInSeconds);
    // 进度人话("正在换 token…")。**不含任何令牌内容**。
    void statusText(const QString &text);
    // 结束。ok=false 时 rawError 是核心库 err 的**原文**(例如第 6 跳的
    // "Invalid app registration, see https://aka.ms/AppRegInfo ..."),界面原样展示。
    void finished(bool ok, const QString &message, const QString &rawError, int code);

private:
    void run();
    void runLogin(sxcl_transport *transport, char *err, size_t errLen);
    void runRefresh(sxcl_transport *transport, char *err, size_t errLen);
    void runLogout(char *err, size_t errLen);
    sxcl_transport *createTransport() const;
    bool saveSession(const sxcl_auth_session *session, QString *errorOut) const;

    // 核心库回调(cb.userdata = this;核心库在工作线程里调它们)
    static void cbUserCode(void *userdata, const char *userCode, const char *verificationUri,
                           const char *message);
    static void cbDeviceCodeInfo(void *userdata, int intervalSeconds, int expiresInSeconds);
    static void cbStatus(void *userdata, const char *message);
    static void cbOpenUrl(void *userdata, const char *url);
    static int cbShouldCancel(void *userdata);

    Operation m_operation;
    // 登录链参数:在 start() 里由 **UI 线程**解析好(设置句柄不跨线程用),工作线程只读。
    QString m_clientId;
    QString m_tenant;
    QString m_tokenPath;
    std::atomic<bool> m_cancel{false};
    QThread *m_thread = nullptr;
    // 在飞的传输后端:取消时要能从 UI 线程调 cancel_all(net.h 明说它是线程安全的)
    mutable QMutex m_transportMutex;
    sxcl_transport *m_transport = nullptr;
};

// ── 后台任务:用已登录的正版身份启动(等价 CLI 的 launch <版本> <目录> --account) ──
// 身份来自加密存储;令牌只在内存里交给启动层(request.access_token),
// 进程命令行/日志里都不会出现(sxcl 的启动层负责不打印)。
class AccountLaunchTask : public QObject {
    Q_OBJECT
public:
    AccountLaunchTask(QString gameDirectory, QString versionName, QString javaPath, int memoryMb,
                      QObject *parent = nullptr);
    void start();

signals:
    // ok=进程真的起来了(退出码 0);detail 是核心库给的一条人话结论
    void finished(bool ok, const QString &title, const QString &detail);

private:
    void run();
    void report(bool ok, const QString &title, const QString &detail);

    QString m_gameDir;
    QString m_versionName;
    QString m_javaPath;
    int m_memoryMb = 0;
    QThread *m_thread = nullptr;
};

// 便捷入口(失败也不抛):建任务、启动、结束即自删。home_page 的启动接线只用这一个。
// 返回 nullptr = 参数不合法(调用方自己报错)。
AccountLaunchTask *startAccountLaunch(const QString &gameDirectory, const QString &versionName,
                                      const QString &javaPath, int memoryMb);

} // namespace sxcl::ui
