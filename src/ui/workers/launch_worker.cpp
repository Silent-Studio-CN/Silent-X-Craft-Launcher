#include "launch_worker.h"

#include <QByteArray>
#include <QDir>
#include <QThread>

#include <cstring>
#include <utility>
#include <vector>

#include "sxcl/launch.h"
#include "sxcl/process.h" // sxcl_process_kill_pid(按 PID 单独结束游戏进程)

#include "ui_paths.h"

namespace sxcl::ui {
namespace {

// 启动层要的字符串一律 UTF-8、且必须活到 sxcl_launch_run 返回 —— 全部落在调用处的
// QByteArray 上,不留临时对象(踩过:toUtf8().constData() 语句一结束就失效)。
QByteArray utf8OrNull(const QString &text) {
    return text.trimmed().isEmpty() ? QByteArray() : text.toUtf8();
}

} // namespace

QStringList LaunchWorker::phaseNames() {
    // 与 launch_page.py:557-563 逐条一致(第 4 行在 Windows 上叫"等待游戏窗口",
    // 其它平台退化成"等待游戏启动"—— src/core/platform.py 的 supports_window_detection)。
    QStringList names;
    names << QStringLiteral("检测 Java 运行时") << QStringLiteral("构建启动命令")
          << QStringLiteral("启动游戏进程")
#ifdef Q_OS_WIN
          << QStringLiteral("等待游戏窗口")
#else
          << QStringLiteral("等待游戏启动")
#endif
          << QStringLiteral("运行完成");
    return names;
}

LaunchWorker::LaunchWorker(LaunchRequest request, QObject *parent)
    : QObject(parent), m_request(std::move(request)) {}

LaunchWorker::~LaunchWorker() {
    // 页面销毁时先请工作线程收工(与 install_worker.cpp 同一口径):
    // 取消是异步的,wait() 等它真的退出;不 detach,否则线程会回调已析构的 this。
    if (m_thread != nullptr) {
        m_cancel.store(true);
        m_thread->quit();
        m_thread->wait();
    }
}

void LaunchWorker::start() {
    if (m_started.exchange(true))
        return;
    m_thread = new QThread(this);
    moveToThread(m_thread);
    connect(m_thread, &QThread::started, this, &LaunchWorker::run);
    connect(m_thread, &QThread::finished, this, [this] { m_running.store(false); });
    m_thread->start();
}

void LaunchWorker::cancel() {
    m_cancel.store(true);
    // ★ 立刻生效的那一半:进程已经起来了(on_started 给了 PID)就直接结束它。
    //   只置取消位的话,核心库要等**下一次输出**才会终止进程 —— 游戏刚起、还没吐一个字时
    //   用户点"取消"会像没反应(这是原实现的真实边界,已在文件头写明)。
    killRunningProcess();
}

bool LaunchWorker::killRunningProcess() {
    const int64_t pid = m_pid.load();
    if (pid <= 0)
        return false;
    const int rc = sxcl_process_kill_pid(pid);
    uiTrace(QStringLiteral("launch | kill pid=%1 rc=%2").arg(pid).arg(rc));
    return rc == 0;
}

bool LaunchWorker::running() const { return m_running.load(); }

int LaunchWorker::cbLine(void *userdata, int is_stderr, const char *line) {
    if (userdata == nullptr)
        return 0;
    return static_cast<LaunchWorker *>(userdata)->onLine(is_stderr, line);
}

int LaunchWorker::cbStarted(void *userdata, int64_t pid) {
    if (userdata == nullptr)
        return 0;
    return static_cast<LaunchWorker *>(userdata)->onStarted(pid);
}

// 进程真的起来了。**工作线程里**调,界面只收信号。
// 返回非 0 = 立刻终止(与 on_line 同一语义,算 killed_by_client)—— 用户可能在
// 进程起来的那一瞬间已经按过取消了。
int LaunchWorker::onStarted(int64_t pid) {
    m_pid.store(pid);
    uiTrace(QStringLiteral("launch | pid=%1").arg(pid));
    emit processStarted(static_cast<qint64>(pid));
    return m_cancel.load() ? 1 : 0;
}

int LaunchWorker::onLine(int isStderr, const char *line) {
    const QString text = QString::fromUtf8(line != nullptr ? line : "");

    // 第一遍(dry_run)的回调就是"最终命令行"(driver.c:360-378);先收着,等整行都齐了
    // 一次性交给界面 —— 一条条发会让界面看到半截命令。
    // 判据是**当前这一遍**的标志,不是界面的 dryRun 请求(真启动时界面请求也是 0)。
    if (m_collectingCommand.load()) {
        m_pendingCommand.append(text);
        return m_cancel.load() ? 1 : 0;
    }

    // ★ 取消:on_line 返回非 0 = 请求终止进程(launch.h:477-479)。
    //   **边取消边把最后几行放出去** —— 直接 return 1 会让用户看不到"为什么被终止"。
    const int stop = m_cancel.load() ? 1 : 0;

    // 进程真的开始输出 = 游戏起来了 -> 进入"等待游戏窗口/游戏运行中"阶段
    if (!m_sawProcessOutput.exchange(true)) {
        const QStringList names = phaseNames();
        if (names.size() > 3)
            emit phaseChanged(3, names.size(), names.at(3));
    }

    // 日志归类:核心库的 logscan(launch.h:358-437)。类别名给界面做筛选/着色用。
    sxcl_log_line scan;
    std::memset(&scan, 0, sizeof(scan));
    scan.exit_code = -1;
    const sxcl_log_kind kind = sxcl_log_scan_line(line, &scan);
    const QString kindName = QString::fromUtf8(sxcl_log_kind_name(kind));

    uiTrace(QStringLiteral("launch | %1 | %2")
                .arg(kindName, text.left(300)));
    emit logLine(text, kindName, scan.severity, isStderr);
    return stop;
}

void LaunchWorker::run() {
    m_running.store(true);

    const QStringList names = phaseNames();
    const int total = names.size();

    const QByteArray gameDir = QDir::fromNativeSeparators(m_request.gameDir).toUtf8();
    const QByteArray versionName = m_request.versionName.toUtf8();
    const QByteArray javaPath = QDir::fromNativeSeparators(m_request.javaPath).toUtf8();
    const QByteArray offlineName = utf8OrNull(m_request.offlineName);
    const QByteArray playerName = utf8OrNull(m_request.playerName);
    const QByteArray uuid = utf8OrNull(m_request.uuid);
    const QByteArray accessToken = utf8OrNull(m_request.accessToken);
    const QByteArray userType = utf8OrNull(m_request.userType);
    const QByteArray xuid = utf8OrNull(m_request.xuid);
    const QByteArray clientId = utf8OrNull(m_request.clientId);
    const QByteArray settingsPath = utf8OrNull(m_request.settingsFile);
    const QByteArray backend = utf8OrNull(m_request.backend);

    if (gameDir.isEmpty() || versionName.isEmpty()) {
        emit finished(false, false, m_request.dryRun != 0, -1, 0, 0,
                      QStringLiteral("unknown"),
                      QStringLiteral("参数不完整:游戏目录与版本名都必须有"), QString());
        m_running.store(false);
        return;
    }

    // 身份口径与 CLI 完全一致(tools/sxcl-dl/main.c:906-916):有 accessToken 才给正版那套,
    // 否则一律空着让核心库退回离线默认值 —— 不"假装"用账户启动。
    const bool online = !accessToken.isEmpty();

    sxcl_launch_request req;
    std::memset(&req, 0, sizeof(req));
    req.game_dir = gameDir.constData();
    req.version_name = versionName.constData();
    req.java_path = javaPath.isEmpty() ? nullptr : javaPath.constData();
    req.memory_mb = m_request.memoryMb;
    req.offline_name = offlineName.isEmpty() ? nullptr : offlineName.constData();
    if (online) {
        req.player_name = playerName.isEmpty() ? nullptr : playerName.constData();
        req.uuid = uuid.isEmpty() ? nullptr : uuid.constData();
        req.access_token = accessToken.constData();
        req.user_type = userType.isEmpty() ? "msa" : userType.constData();
        req.xuid = xuid.isEmpty() ? nullptr : xuid.constData();
        req.client_id = clientId.isEmpty() ? nullptr : clientId.constData();
    }
    req.backend = backend.isEmpty() ? nullptr : backend.constData();
    req.settings_path = settingsPath.isEmpty() ? nullptr : settingsPath.constData();
    req.timeout_ms = m_request.timeoutMs;
    req.on_line = &LaunchWorker::cbLine;
    // 进程真起来时把 PID 交出来(界面显示 + 按 PID 结束;见文件头"取消"一节)
    req.on_started = &LaunchWorker::cbStarted;
    req.userdata = this;

    char err[256];
    err[0] = '\0';

    // ── 阶段 0:检测 Java 运行时(第一遍会把 Java 选出来)──
    emit phaseChanged(0, total, names.value(0));
    if (m_cancel.load()) {
        emit finished(false, true, false, -1, 0, 0, QStringLiteral("cancelled"),
                      QStringLiteral("已取消:准备阶段就被取消,没有起进程"), QString());
        m_running.store(false);
        return;
    }

    // ── 阶段 1:构建启动命令(第一遍 dry_run=1;真实走完全部准备)──
    emit phaseChanged(1, total, names.value(1));
    req.dry_run = 1;
    m_pendingCommand.clear();
    m_collectingCommand.store(true); // 这一遍的回调是命令行,不是游戏输出
    sxcl_launch_result prep;
    std::memset(&prep, 0, sizeof(prep));
    const int prepRc = sxcl_launch_run(&req, &prep, err, sizeof(err));

    m_collectingCommand.store(false); // 准备跑完:后面的回调都是真进程输出
    emit javaInfo(QString::fromUtf8(prep.java_path), prep.java_major,
                  QString::fromUtf8(prep.java_version), prep.java_is_64bit);
    emit commandLine(m_pendingCommand);
    for (const QString &line : m_pendingCommand)
        uiTrace(QStringLiteral("launch | argv | %1").arg(line));

    if (prepRc != 0) {
        // 准备就没过:真实原因在 prep.error / err 里(核心库的人话)
        const QString reason = QString::fromUtf8(prep.error).trimmed().isEmpty()
                                   ? QString::fromUtf8(err)
                                   : QString::fromUtf8(prep.error);
        emit finished(false, false, false, -1, 0, 0,
                      QStringLiteral("unknown"),
                      reason.isEmpty() ? QStringLiteral("启动准备失败") : reason,
                      QStringLiteral("Java:%1 · 目录:%2")
                          .arg(QString::fromUtf8(prep.java_path),
                               QDir::toNativeSeparators(m_request.gameDir)));
        m_running.store(false);
        return;
    }

    const QString prepDetail =
        QStringLiteral("Java %1(%2)· natives %3 个 · 后端 %4 -> %5")
            .arg(QString::fromUtf8(prep.java_path),
                 prep.java_major > 0 ? QStringLiteral("Java %1").arg(prep.java_major)
                                     : QStringLiteral("版本未知"))
            .arg(prep.natives_count)
            .arg(QString::fromUtf8(prep.requested_backend), QString::fromUtf8(prep.actual_backend));

    if (m_request.dryRun != 0) {
        // ── dry-run:只准备,不起进程(验收的"启动 --dry-run"就是这条)──
        emit phaseChanged(4, total, names.value(4));
        emit finished(true, false, true, -1, 0, 0, QStringLiteral("ok"),
                      QStringLiteral("准备完成(未起进程):%1").arg(QString::fromUtf8(
                          prep.conclusion_text)),
                      prepDetail);
        m_running.store(false);
        return;
    }

    if (m_cancel.load()) {
        emit finished(false, true, false, -1, 0, 0, QStringLiteral("cancelled"),
                      QStringLiteral("已取消:准备完成但还没起进程"), prepDetail);
        m_running.store(false);
        return;
    }

    // ── 阶段 2:启动游戏进程(第二遍 dry_run=0,真的起)──
    emit phaseChanged(2, total, names.value(2));
    req.dry_run = 0;
    m_sawProcessOutput.store(false);
    sxcl_launch_result res;
    std::memset(&res, 0, sizeof(res));
    err[0] = '\0';
    const int rc = sxcl_launch_run(&req, &res, err, sizeof(err));

    // ── 阶段 4:运行完成 ──
    emit phaseChanged(4, total, names.value(4));

    const QString conclusionKey = QString::fromUtf8(sxcl_log_conclusion_name(res.conclusion));
    const QString conclusionText = QString::fromUtf8(res.conclusion_text);
    const QString detail =
        QStringLiteral("退出码 %1 · 用时 %2 s · 日志 %3 行 · Java %4%5%6")
            .arg(res.exit_code)
            .arg(double(res.elapsed_ms) / 1000.0, 0, 'f', 1)
            .arg(qulonglong(res.log.lines))
            .arg(QString::fromUtf8(res.java_path))
            .arg(res.timed_out ? QStringLiteral(" · 超时被终止") : QString())
            .arg(res.killed_by_client ? QStringLiteral(" · 按请求终止") : QString());

    if (rc != 0) {
        const QString reason = QString::fromUtf8(res.error).trimmed().isEmpty()
                                   ? QString::fromUtf8(err)
                                   : QString::fromUtf8(res.error);
        const bool cancelled = (res.started == 0 && m_cancel.load());
        emit finished(false, cancelled, false, res.exit_code, res.timed_out, res.killed_by_client,
                      conclusionKey,
                      reason.isEmpty() ? QStringLiteral("启动失败(核心库没给原因)") : reason,
                      detail);
        m_running.store(false);
        return;
    }

    // 起来了:结算按"退出码 + 核心库的人话结论"。killed_by_client 是"按请求终止",
    // 我们只在取消时才会请求终止,所以它单独算取消。
    const bool cancelled = (res.killed_by_client != 0) || m_cancel.load();
    const bool ok = (res.exit_code == 0) && !cancelled && res.timed_out == 0;

    QString message;
    if (cancelled)
        message = QStringLiteral("已取消:进程按请求终止(退出码 %1)").arg(res.exit_code);
    else if (res.timed_out)
        message = conclusionText.isEmpty() ? QStringLiteral("启动超时,进程已被终止")
                                           : conclusionText;
    else if (ok)
        message = QStringLiteral("游戏已退出(退出码 0)");
    else
        message = conclusionText.isEmpty()
                      ? QStringLiteral("游戏以退出码 %1 结束").arg(res.exit_code)
                      : QStringLiteral("游戏以退出码 %1 结束:%2").arg(res.exit_code).arg(conclusionText);

    emit finished(ok, cancelled, false, res.exit_code, res.timed_out, res.killed_by_client,
                  conclusionKey, message, detail);
    m_running.store(false);
}

} // namespace sxcl::ui
