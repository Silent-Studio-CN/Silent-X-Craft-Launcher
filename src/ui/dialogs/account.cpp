/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "account.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QMutexLocker>
#include <QThread>

#include <cstring>

#include "auth_replay.h"    // 取证:夹具回放传输(默认不启用)
#include "sxcl/launch.h"
#include "sxcl/net.h"       // sxcl_transport_qt_create / bootstrap
#include "sxcl/settings.h"  // auth.client_id / auth.tenant 的解析来源

namespace sxcl::ui {
namespace {

// ── 取证通路:夹具回放(默认完全关闭;见 auth_replay.h 的说明) ──
//   SXCL_UI_AUTH_REPLAY=<tests/fixtures/auth 目录>  用实测响应的脱敏副本跑完整条链
//   SXCL_UI_AUTH_REPLAY_HOP6=1                     让第 6 跳返回实测的 403 正文
QString replayFixtureDir() { return qEnvironmentVariable("SXCL_UI_AUTH_REPLAY"); }
bool replayFailsAtHop6() { return qEnvironmentVariableIntValue("SXCL_UI_AUTH_REPLAY_HOP6") != 0; }

QString timeText(qint64 unixSeconds) {
    if (unixSeconds <= 0)
        return QStringLiteral("(未取到)");
    return QDateTime::fromSecsSinceEpoch(unixSeconds).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace

QString uiSettingsPath() {
#if defined(Q_OS_WIN)
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/AppData/Roaming");
    return base + QStringLiteral("/SilentXCraftLauncher/settings.conf");
#elif defined(Q_OS_MACOS)
    return QDir::homePath() +
           QStringLiteral("/Library/Application Support/SilentXCraftLauncher/settings.conf");
#else
    return QDir::homePath() + QStringLiteral("/.config/SilentXCraftLauncher/settings.conf");
#endif
}

QString defaultTokenPath() {
    char path[SXCL_AUTH_STORE_PATH_MAX];
    char err[SXCL_AUTH_ERROR_MAX];
    path[0] = '\0';
    err[0] = '\0';
    if (sxcl_auth_store_default_path(path, sizeof(path), err, sizeof(err)) != SXCL_AUTH_OK)
        return QString();
    return QString::fromUtf8(path);
}

AccountSnapshot loadAccountSnapshot() {
    AccountSnapshot snapshot;
    const sxcl_auth_store_kind backend = sxcl_auth_store_backend();
    snapshot.storeBackend = QString::fromUtf8(sxcl_auth_store_kind_name(backend));
    snapshot.storeNote = QString::fromUtf8(sxcl_auth_store_kind_note(backend));

    const QString path = defaultTokenPath();
    if (path.isEmpty()) {
        snapshot.error = QStringLiteral("拿不到令牌文件路径（sxcl_auth_store_default_path 失败）");
        return snapshot;
    }
    const QByteArray nativePath = path.toUtf8();
    snapshot.tokenFileExists = sxcl_auth_store_exists(nativePath.constData()) != 0;
    if (!snapshot.tokenFileExists)
        return snapshot; // 没登录过 —— 正常状态,不是错误

    sxcl_auth_session *session = new sxcl_auth_session();
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_auth_store_load(nativePath.constData(), session, err, sizeof(err));
    if (rc != SXCL_AUTH_OK) {
        snapshot.error = QString::fromUtf8(err);
        delete session;
        return snapshot;
    }

    snapshot.loggedIn = sxcl_auth_session_has_ms(session) != 0;
    snapshot.hasMcToken = sxcl_auth_session_has_mc(session) != 0;
    snapshot.accountName = QString::fromUtf8(session->account_name);
    snapshot.playerName = QString::fromUtf8(session->mc.name);
    snapshot.uuid = QString::fromUtf8(session->mc.uuid);
    snapshot.entitlementCount = session->mc.entitlement_count;
    snapshot.entitlementChecked = session->mc.entitlements_checked != 0;
    snapshot.msExpiresAt = session->ms.expires_at;
    snapshot.mcExpiresAt = session->mc.expires_at;
    // 过期判断一律用核心库的函数(与 CLI 同一个 120s 余量),别拿时间戳自己比。
    snapshot.mcExpired = sxcl_auth_mc_expired(session, sxcl_auth_now(), 120) != 0;
    delete session;
    return snapshot;
}

bool accountCanLaunch(const AccountSnapshot &snapshot) {
    return snapshot.loggedIn && snapshot.hasMcToken && !snapshot.mcExpired &&
           !snapshot.playerName.isEmpty();
}

// ───────────────────────────── AccountTask ─────────────────────────────

AccountTask::AccountTask(Operation operation, QObject *parent)
    : QObject(parent), m_operation(operation) {}

AccountTask::~AccountTask() {
    cancel();
    if (m_thread != nullptr) {
        // 线程必须在 QThread 对象析构前结束(否则 Qt 会警告并可能崩)。
        // 取消位已经置上,核心库的轮询循环每 200ms 查一次;在飞的 HTTP 也被 cancel() 撤掉,
        // 所以正常情况这里几毫秒就返回。
        //
        // **有界等待**:实测过"取消后不等、把任务脱手"的写法 —— 退出时留着一个活线程,
        // 整个进程会卡在退出路径上(必须强杀)。所以这里最多等 3 秒:
        //   等到了 → 正常删除;
        //   等不到(极罕见:卡在系统调用里)→ 如实打一条诊断,**把 QThread 脱手**,
        //   宁可泄漏一个对象,也不让用户关不掉窗口 / 退不出程序。
        if (m_thread->wait(3000)) {
            delete m_thread;
        } else {
            std::fprintf(stderr, "[sxcl-ui] 登录线程 3 秒内没有结束,退出时不再等它(对象脱手)\n");
            m_thread->setParent(nullptr);
        }
        m_thread = nullptr;
    }
}

void AccountTask::start() {
    if (m_thread != nullptr)
        return; // 已经在跑

    // client_id / 租户段在 **UI 线程**解析(设置句柄有线程亲和性,不跨线程使用):
    // 优先级 = 环境变量 > sxcl_settings > 内置默认(核心库的 sxcl_auth_resolve_*,别自己再写一套)。
    char buf[SXCL_AUTH_CLIENT_ID_MAX];
    char tenant[SXCL_AUTH_TENANT_MAX];
    buf[0] = '\0';
    tenant[0] = '\0';
    const QByteArray settingsPath = uiSettingsPath().toUtf8();
    sxcl_settings *settings = sxcl_settings_open(settingsPath.constData());
    (void)sxcl_auth_resolve_client_id(nullptr, settings, buf, sizeof(buf));
    (void)sxcl_auth_resolve_tenant(nullptr, settings, tenant, sizeof(tenant));
    if (settings != nullptr)
        sxcl_settings_free(settings);
    m_clientId = QString::fromUtf8(buf);
    m_tenant = QString::fromUtf8(tenant);
    m_tokenPath = defaultTokenPath();

    m_thread = QThread::create([this] { run(); });
    connect(m_thread, &QThread::finished, this, [this] {
        QThread *finishedThread = m_thread;
        m_thread = nullptr;
        if (finishedThread != nullptr)
            finishedThread->deleteLater();
    });
    m_thread->start();
}

bool AccountTask::running() const {
    return m_thread != nullptr && m_thread->isRunning();
}

void AccountTask::cancel() {
    m_cancel.store(true);
    QMutexLocker locker(&m_transportMutex);
    if (m_transport != nullptr)
        m_transport->cancel_all(m_transport->ctx);
}

void AccountTask::run() {
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';

    if (m_operation == Operation::Logout) {
        runLogout(err, sizeof(err));
        return;
    }
    if (m_tokenPath.isEmpty()) {
        emit finished(false, QStringLiteral("登录失败"),
                      QStringLiteral("拿不到令牌文件路径（见 docs/09-正版登录.md §6）"),
                      SXCL_AUTH_ERR_STORE);
        return;
    }
    sxcl_transport *transport = createTransport();
    if (transport == nullptr) {
        emit finished(false, QStringLiteral("登录失败"),
                      QStringLiteral("没有可用的网络后端（Qt Network 传输不可用）"),
                      SXCL_AUTH_ERR_UNSUPPORTED);
        return;
    }
    {
        QMutexLocker locker(&m_transportMutex);
        m_transport = transport;
    }

    if (m_operation == Operation::Login)
        runLogin(transport, err, sizeof(err));
    else
        runRefresh(transport, err, sizeof(err));

    {
        QMutexLocker locker(&m_transportMutex);
        m_transport = nullptr;
    }
    transport->destroy(transport->ctx);
}

sxcl_transport *AccountTask::createTransport() const {
    // 取证通路(默认关闭):用 tests/fixtures/auth 的真实响应副本跑同一条链。
    const QString fixtureDir = replayFixtureDir();
    if (!fixtureDir.isEmpty()) {
        return sxcl_ui_auth_replay_create(fixtureDir.toUtf8().constData(),
                                          replayFailsAtHop6() ? 1 : 0);
    }
#if defined(SXCL_UI_HAVE_QT_TRANSPORT)
    sxcl_transport_qt_bootstrap();
    return sxcl_transport_qt_create();
#else
    return nullptr; // 没编 Qt 传输后端:如实报"没有可用后端",不假装成功
#endif
}

bool AccountTask::saveSession(const sxcl_auth_session *session, QString *errorOut) const {
    char err[SXCL_AUTH_ERROR_MAX];
    err[0] = '\0';
    const QByteArray path = m_tokenPath.toUtf8();
    if (sxcl_auth_store_save(path.constData(), session, err, sizeof(err)) != SXCL_AUTH_OK) {
        if (errorOut != nullptr)
            *errorOut = QString::fromUtf8(err);
        return false;
    }
    return true;
}

void AccountTask::runLogin(sxcl_transport *transport, char *err, size_t errLen) {
    // 注意:QByteArray 必须是**具名局部变量** —— opts 里只存指针,临时对象一结束就是悬空指针。
    const QByteArray clientId = m_clientId.toUtf8();
    const QByteArray tenant = m_tenant.toUtf8();

    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = clientId.isEmpty() ? nullptr : clientId.constData();
    opts.tenant = tenant.isEmpty() ? nullptr : tenant.constData();
    opts.settings = nullptr;     // 设置句柄留在 UI 线程(见 start())
    opts.device_code = 1;        // 设备码流为主:不依赖浏览器,也不需要能监听 127.0.0.1
    opts.no_browser = 1;
    opts.http_timeout_ms = 30000;
    opts.poll_timeout_ms = 0;    // 0 = 用微软给的 expires_in(通常 900s),不自己拍一个
    opts.should_cancel = &AccountTask::cbShouldCancel;
    opts.cb.userdata = this;
    opts.cb.on_user_code = &AccountTask::cbUserCode;
    opts.cb.on_device_code_info = &AccountTask::cbDeviceCodeInfo;
    opts.cb.on_status = &AccountTask::cbStatus;
    opts.cb.on_open_url = &AccountTask::cbOpenUrl;

    sxcl_auth_session *session = new sxcl_auth_session();
    const int rc = sxcl_auth_login(transport, &opts, session, err, errLen);

    // docs/09 §6.3:微软令牌一到手就**立刻加密落盘**,哪怕后面某一跳失败。
    // (第 6 跳被应用资质挡住时,用户不该被迫再输一次设备码 —— 那是第一版踩过的坑。)
    QString saveError;
    bool saved = false;
    if (rc == SXCL_AUTH_OK || sxcl_auth_session_has_ms(session))
        saved = saveSession(session, &saveError);

    QString message;
    QString raw = QString::fromUtf8(err);
    if (rc == SXCL_AUTH_OK) {
        const QString player = session->mc.name[0] ? QString::fromUtf8(session->mc.name)
                                                   : QStringLiteral("(未取到玩家名)");
        const QString entitlement =
            session->mc.entitlement_count > 0
                ? QStringLiteral("拥有 Java 版权益（mcstore 条目数 %1）")
                      .arg(session->mc.entitlement_count)
                : QStringLiteral("mcstore 里没有条目");
        message = QStringLiteral("登录成功：%1（uuid %2）；%3")
                      .arg(player, QString::fromUtf8(session->mc.uuid), entitlement);
        if (!saved) {
            message += QStringLiteral("\n注意：凭据落盘失败：%1").arg(saveError);
        }
    } else {
        message = QStringLiteral("登录没有完成（返回码 %1）").arg(rc);
        if (rc == SXCL_AUTH_ERR_XBOX && session->last_error_xerr != 0) {
            char human[SXCL_AUTH_MESSAGE_MAX];
            human[0] = '\0';
            (void)sxcl_auth_xsts_error_message((uint64_t)session->last_error_xerr, human,
                                               sizeof(human));
            message += QStringLiteral("\nXSTS 原因：%1").arg(QString::fromUtf8(human));
        }
        if (raw.isEmpty()) {
            // 取消不算"失败原因未知":用户在界面上点了取消,如实说。
            raw = (rc == SXCL_AUTH_ERR_CANCELLED)
                      ? QStringLiteral("已取消（核心库返回 SXCL_AUTH_ERR_CANCELLED）")
                      : QStringLiteral("核心库没有给出详细原因（返回码 %1）").arg(rc);
        }
        const int64_t aadsts = sxcl_auth_aadsts_extract(err);
        if (aadsts != 0) {
            const char *hint = sxcl_auth_aadsts_hint(aadsts);
            if (hint != nullptr)
                message += QStringLiteral("\nAADSTS %1 建议：%2").arg(aadsts).arg(QString::fromUtf8(hint));
        }
        if (saved) {
            message += QStringLiteral(
                "\n微软登录本身是成功的：凭据已经加密保存，修好问题后点「刷新」即可，不用再输一次设备码。");
        }
    }
    delete session;
    emit finished(rc == SXCL_AUTH_OK, message, raw, rc);
}

void AccountTask::runRefresh(sxcl_transport *transport, char *err, size_t errLen) {
    const QByteArray path = m_tokenPath.toUtf8();
    sxcl_auth_session *session = new sxcl_auth_session();
    int rc = sxcl_auth_store_load(path.constData(), session, err, errLen);
    if (rc != SXCL_AUTH_OK) {
        const QString detail = QString::fromUtf8(err);
        delete session;
        emit finished(false, QStringLiteral("本机还没有登录过的账户，先点「登录」"), detail, rc);
        return;
    }

    const QByteArray clientId = m_clientId.toUtf8();
    const QByteArray tenant = m_tenant.toUtf8();
    sxcl_auth_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = clientId.isEmpty() ? nullptr : clientId.constData();
    opts.tenant = tenant.isEmpty() ? nullptr : tenant.constData();
    opts.settings = nullptr;
    opts.http_timeout_ms = 30000;
    opts.should_cancel = &AccountTask::cbShouldCancel;
    opts.cb.userdata = this;
    opts.cb.on_status = &AccountTask::cbStatus;

    rc = sxcl_auth_refresh(transport, &opts, session, err, errLen);

    QString saveError;
    bool saved = false;
    if (rc == SXCL_AUTH_OK || sxcl_auth_session_has_ms(session))
        saved = saveSession(session, &saveError);

    QString message;
    const QString raw = QString::fromUtf8(err);
    if (rc == SXCL_AUTH_OK) {
        message = QStringLiteral("刷新成功：%1（MC 令牌到 %2）")
                      .arg(QString::fromUtf8(session->mc.name), timeText(session->mc.expires_at));
        if (!saved)
            message += QStringLiteral("\n注意：新凭据落盘失败：%1").arg(saveError);
    } else {
        message = QStringLiteral("刷新没有完成（返回码 %1）").arg(rc);
        if (rc == SXCL_AUTH_ERR_XBOX && session->last_error_xerr != 0) {
            char human[SXCL_AUTH_MESSAGE_MAX];
            human[0] = '\0';
            (void)sxcl_auth_xsts_error_message((uint64_t)session->last_error_xerr, human,
                                               sizeof(human));
            message += QStringLiteral("\nXSTS 原因：%1").arg(QString::fromUtf8(human));
        }
        if (rc == SXCL_AUTH_ERR_EXPIRED)
            message += QStringLiteral("\nrefresh token 已过期或被撤销：需要重新登录一次。");
    }
    delete session;
    emit finished(rc == SXCL_AUTH_OK, message, raw, rc);
}

void AccountTask::runLogout(char *err, size_t errLen) {
    if (m_tokenPath.isEmpty()) {
        emit finished(false, QStringLiteral("退出登录失败"),
                      QStringLiteral("拿不到令牌文件路径"), SXCL_AUTH_ERR_STORE);
        return;
    }
    const QByteArray path = m_tokenPath.toUtf8();
    const int rc = sxcl_auth_store_clear(path.constData(), err, errLen);
    const QString message = (rc == SXCL_AUTH_OK)
                                ? QStringLiteral("已退出登录：本机的加密凭据已删除（幂等，重复点也成功）")
                                : QStringLiteral("退出登录失败（返回码 %1）").arg(rc);
    emit finished(rc == SXCL_AUTH_OK, message, QString::fromUtf8(err), rc);
}

// 核心库回调(在工作线程里被调用):只做一件事 —— 把内容 emit 成信号,
// Qt 会自动排队到 UI 线程(AutoConnection)。**绝不在回调里碰任何控件。**
void AccountTask::cbUserCode(void *userdata, const char *userCode, const char *verificationUri,
                             const char *message) {
    (void)message; // 设备码流这条核心库目前传 NULL(整句提示在 verification_uri/user_code 里)
    AccountTask *task = static_cast<AccountTask *>(userdata);
    if (task == nullptr)
        return;
    emit task->userCode(QString::fromUtf8(userCode != nullptr ? userCode : ""),
                        QString::fromUtf8(verificationUri != nullptr ? verificationUri : ""));
}

void AccountTask::cbDeviceCodeInfo(void *userdata, int intervalSeconds, int expiresInSeconds) {
    AccountTask *task = static_cast<AccountTask *>(userdata);
    if (task == nullptr)
        return;
    emit task->deviceCodeInfo(intervalSeconds, expiresInSeconds);
}

void AccountTask::cbStatus(void *userdata, const char *message) {
    AccountTask *task = static_cast<AccountTask *>(userdata);
    if (task == nullptr)
        return;
    emit task->statusText(QString::fromUtf8(message != nullptr ? message : ""));
}

void AccountTask::cbOpenUrl(void *userdata, const char *url) {
    AccountTask *task = static_cast<AccountTask *>(userdata);
    if (task == nullptr)
        return;
    // 设备码流不会走这里(那是授权码流的回调);真走到也一行不落地如实转给界面。
    emit task->statusText(QStringLiteral("授权链接：%1")
                              .arg(QString::fromUtf8(url != nullptr ? url : "")));
}

int AccountTask::cbShouldCancel(void *userdata) {
    const AccountTask *task = static_cast<const AccountTask *>(userdata);
    if (task == nullptr)
        return 0;
    return task->m_cancel.load() ? 1 : 0;
}

// ────────────────────────── AccountLaunchTask ──────────────────────────

AccountLaunchTask::AccountLaunchTask(QString gameDirectory, QString versionName, QString javaPath,
                                     int memoryMb, QObject *parent)
    : QObject(parent), m_gameDir(std::move(gameDirectory)), m_versionName(std::move(versionName)),
      m_javaPath(std::move(javaPath)), m_memoryMb(memoryMb) {}

void AccountLaunchTask::start() {
    if (m_thread != nullptr)
        return;
    m_thread = QThread::create([this] { run(); });
    connect(m_thread, &QThread::finished, this, [this] {
        QThread *finishedThread = m_thread;
        m_thread = nullptr;
        if (finishedThread != nullptr)
            finishedThread->deleteLater();
    });
    m_thread->start();
}

void AccountLaunchTask::report(bool ok, const QString &title, const QString &detail) {
    emit finished(ok, title, detail);
}

void AccountLaunchTask::run() {
    // 身份判据与 CLI 的 launch --account 完全一致(那里是 core 的参考实现,别另立一套):
    //   读加密存储 → 过期就拒绝 → 没有 Java 版档案(名字为空)就拒绝。
    const QString tokenPath = defaultTokenPath();
    if (tokenPath.isEmpty()) {
        report(false, QStringLiteral("正版启动失败"),
               QStringLiteral("拿不到令牌文件路径（见 docs/09-正版登录.md §6）"));
        return;
    }
    sxcl_auth_session *session = new sxcl_auth_session();
    char authError[SXCL_AUTH_ERROR_MAX];
    authError[0] = '\0';
    const QByteArray tokenPathUtf8 = tokenPath.toUtf8();
    if (sxcl_auth_store_load(tokenPathUtf8.constData(), session, authError,
                             sizeof(authError)) != SXCL_AUTH_OK) {
        const QString detail = QStringLiteral("读取已登录账户失败：%1").arg(QString::fromUtf8(authError));
        delete session;
        report(false, QStringLiteral("正版启动失败"), detail);
        return;
    }
    if (sxcl_auth_mc_expired(session, sxcl_auth_now(), 120) != 0) {
        delete session;
        report(false, QStringLiteral("正版启动失败"),
               QStringLiteral("Minecraft 令牌已过期：去 设置 → 账户 点「刷新」（免密续期）；"
                              "不行就重新登录一次。"));
        return;
    }
    if (session->mc.name[0] == '\0' || session->mc.access_token[0] == '\0') {
        delete session;
        report(false, QStringLiteral("正版启动失败"),
               QStringLiteral("这个账户没有可用的 Java 版身份（没买或档案没取到）—— "
                              "按离线身份启动请到版本页用离线入口。"));
        return;
    }

    // UTF-8 缓冲必须活到 sxcl_launch_run 结束(启动层只存指针)。
    const QByteArray gameDir = m_gameDir.toUtf8();
    const QByteArray versionName = m_versionName.toUtf8();
    const QByteArray javaPath = m_javaPath.toUtf8();
    const QByteArray settingsPath = uiSettingsPath().toUtf8(); // 具名:下面只存指针
    char xuid[32];
    xuid[0] = '\0';
    if (session->xbox.xuid != 0)
        (void)snprintf(xuid, sizeof(xuid), "%llu", (unsigned long long)session->xbox.xuid);

    sxcl_launch_request request;
    memset(&request, 0, sizeof(request));
    request.game_dir = gameDir.constData();
    request.version_name = versionName.constData();
    request.java_path = javaPath.isEmpty() ? nullptr : javaPath.constData();
    request.memory_mb = m_memoryMb;
    request.instance = versionName.constData();
    // ── 正版身份(与 CLI --account 同一组字段;access_token 只在内存里传,不落日志) ──
    request.player_name = session->mc.name;
    request.uuid = session->mc.uuid;
    request.access_token = session->mc.access_token;
    request.user_type = "msa";
    if (xuid[0] != '\0')
        request.xuid = xuid;
    request.client_id = session->client_id;
    request.settings_path = settingsPath.constData();

    sxcl_launch_result result;
    memset(&result, 0, sizeof(result));
    char err[256];
    err[0] = '\0';
    const int rc = sxcl_launch_run(&request, &result, err, sizeof(err));

    QString detail;
    if (result.java_path[0] != '\0') {
        detail = QStringLiteral("Java %1 · %2\n")
                     .arg(result.java_major)
                     .arg(QString::fromUtf8(result.java_path));
    }
    if (result.conclusion_text[0] != '\0')
        detail += QString::fromUtf8(result.conclusion_text);
    else if (err[0] != '\0')
        detail += QString::fromUtf8(err);
    if (result.started && result.exit_code != 0)
        detail += QStringLiteral("\n进程退出码 %1").arg(result.exit_code);

    const bool ok = (rc == 0 && result.started != 0);
    delete session;
    report(ok, ok ? QStringLiteral("已用正版账户启动 %1").arg(m_versionName)
                  : QStringLiteral("正版启动失败"),
           detail);
}

AccountLaunchTask *startAccountLaunch(const QString &gameDirectory, const QString &versionName,
                                      const QString &javaPath, int memoryMb) {
    if (gameDirectory.isEmpty() || versionName.isEmpty())
        return nullptr;
    auto *task = new AccountLaunchTask(gameDirectory, versionName, javaPath, memoryMb);
    // 结束即自删:调用方(主页)只关心 InfoBar 上那一条提示,不需要持有任务。
    QObject::connect(task, &AccountLaunchTask::finished, task, &QObject::deleteLater);
    task->start();
    return task;
}

} // namespace sxcl::ui
