/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "install_worker.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QDateTime>

#include <cstdio>
#include <cstdlib> // free(maven-metadata 的文本缓冲)
#include <cstring>
#include <utility>

#include "sxcl/engine.h"
#include "sxcl/fs.h"
#include "sxcl/loader.h"
#include "sxcl/log.h"
#include "sxcl/settings.h"

#include "ui_paths.h"

#if defined(SXCL_UI_HAVE_QT_TRANSPORT)
#include "sxcl/net.h" // sxcl_transport_qt_create / sxcl_transport_qt_bootstrap
#endif

namespace sxcl::ui {
namespace {

// ── Fabric 安装器版本:去 maven-metadata.xml 问,不能用 loader 版本 ──
//
// 事实(2026-09 实测,两条都拉过真地址):
//   * net.fabricmc:fabric-installer 的 maven 版本号是**安装器自己的版本**
//     (maven-metadata.xml 里 <latest>=1.1.2;历史版本 0.2.0.7…0.15.x),**不是** loader 版本;
//   * 拿 loader 版本 0.16.9 拼出来的
//       https://maven.fabricmc.net/net/fabricmc/fabric-installer/0.16.9/fabric-installer-0.16.9.jar
//     实测 HTTP 404 —— 绝大多数选 Fabric 的用户都会撞上。
// 所以:先取 maven-metadata.xml 解析 <latest>(没有就用 <release>),用解析出来的版本拼 URL。
// 镜像优先(与"下载源"设置同口径),取不到就退回老写法 —— 但日志里**必须写明这是猜测值**。
QString versionFromMavenMetadata(const QByteArray &xml) {
    const auto valueOf = [&xml](const char *tag) -> QString {
        const QByteArray open = QByteArray("<") + tag + ">";
        const QByteArray close = QByteArray("</") + tag + ">";
        const int at = xml.indexOf(open);
        if (at < 0)
            return QString();
        const int from = at + open.size();
        const int to = xml.indexOf(close, from);
        if (to < 0)
            return QString();
        return QString::fromUtf8(xml.mid(from, to - from)).trimmed();
    };
    QString version = valueOf("latest");
    if (version.isEmpty())
        version = valueOf("release");
    return version;
}

// 只认"数字 + 点"这种形状的版本号(防止把 HTML 错误页里的半截字当版本用)
bool looksLikeMavenVersion(const QString &version) {
    if (version.isEmpty() || version.size() > 32 || !version.contains(QLatin1Char('.')))
        return false;
    for (const QChar c : version) {
        if (!c.isDigit() && c != QLatin1Char('.'))
            return false;
    }
    return version.at(0).isDigit();
}

// 下载源设置里的 maven 镜像根(选 mojang = 不用镜像,与其它下载同一个开关)
QString mavenMirrorRoot() {
    if (uiDownloadSource() == QLatin1String("mojang"))
        return QString();
    return QStringLiteral("https://bmclapi2.bangbang93.com/maven");
}

/** 解析 Fabric 安装器版本(镜像 -> 官方),并把"从哪拿到的"写进 *source。
 *  传 opts 是为了复用同一条传输后端(核心库的 sxcl_install_http_get_text)。 */
QString resolveFabricInstallerVersion(sxcl_engine_opts *opts, QString *source) {
    QStringList roots;
    const QString mirror = mavenMirrorRoot();
    if (!mirror.isEmpty())
        roots << mirror;
    roots << QStringLiteral("https://maven.fabricmc.net");

    for (const QString &root : roots) {
        const QString url =
            root + QStringLiteral("/net/fabricmc/fabric-installer/maven-metadata.xml");
        char *text = nullptr;
        char err[SXCL_INSTALL_ERROR_MAX];
        err[0] = '\0';
        if (sxcl_install_http_get_text(opts, url.toUtf8().constData(), &text, err, sizeof(err)) != 0) {
            SXCL_LOG_W("install", "取 Fabric 安装器版本清单失败: %s(%s)",
                       url.toUtf8().constData(), err[0] ? err : "未知原因");
            continue;
        }
        const QString version = versionFromMavenMetadata(QByteArray(text != nullptr ? text : ""));
        free(text);
        if (!looksLikeMavenVersion(version)) {
            SXCL_LOG_W("install", "Fabric 安装器版本清单里没有能用的 latest/release: %s",
                       url.toUtf8().constData());
            continue;
        }
        if (source != nullptr)
            *source = url;
        return version;
    }
    return QString();
}

// ── 加载器安装器的 maven 地址 ──
//
// install.h:118-119 明说 installer_url"由加载器版本列表层给,本层不会自己拼 URL"
// (规格 §7 缺口 2)。界面层就是那个"列表层",所以拼 URL 这件事落在这里 —— 但这**只**
// 拼地址,不改变安装器怎么跑、校验怎么做(那些全在 loader.c / install.c 里)。
//
// 各家布局(都是它们 maven 仓库里的稳定路径):
//   Forge     https://maven.minecraftforge.net/net/minecraftforge/forge/<mc>-<fv>/forge-<mc>-<fv>-installer.jar
//   NeoForge  https://maven.neoforged.net/releases/net/neoforged/neoforge/<fv>/neoforge-<fv>-installer.jar
//   Fabric    https://maven.fabricmc.net/net/fabricmc/fabric-installer/<安装器版本>/fabric-installer-<安装器版本>.jar
//             **安装器版本 != loader 版本**(见上面 resolveFabricInstallerVersion);解析不到时
//             才退回用 loader 版本猜(调用方必须把"用了猜测值"写进日志)。
//   OptiFine  没有稳定的 maven 直链(官方按网页放行),**如实返回空** —— 上层据此报真实原因
//             ("OptiFine 的安装器地址拿不到"),绝不去猜一个 URL 然后拿 404 冒充网络故障。
QByteArray installerUrlFor(const QString &loaderType, const QString &mcVersion,
                           const QString &loaderVersion,
                           const QString &fabricInstallerVersion) {
    const QString type = loaderType.trimmed().toLower();
    const QString fv = loaderVersion.trimmed();
    if (fv.isEmpty())
        return QByteArray();
    if (type == QLatin1String("forge")) {
        // Forge 的坐标是 <mc>-<forge>(如 1.20.1-47.2.0);用户给的可能已经带 mc 前缀。
        const QString coord = fv.startsWith(mcVersion + QLatin1Char('-')) ? fv
                                                                          : mcVersion + QLatin1Char('-') + fv;
        return QStringLiteral("https://maven.minecraftforge.net/net/minecraftforge/forge/%1/"
                              "forge-%1-installer.jar")
            .arg(coord)
            .toUtf8();
    }
    if (type == QLatin1String("neoforge")) {
        return QStringLiteral("https://maven.neoforged.net/releases/net/neoforged/neoforge/%1/"
                              "neoforge-%1-installer.jar")
            .arg(fv)
            .toUtf8();
    }
    if (type == QLatin1String("fabric")) {
        // 坐标 = **安装器自己的版本**(maven-metadata 的 latest),不是 loader 版本
        const QString installer =
            fabricInstallerVersion.trimmed().isEmpty() ? fv : fabricInstallerVersion.trimmed();
        return QStringLiteral("https://maven.fabricmc.net/net/fabricmc/fabric-installer/%1/"
                              "fabric-installer-%1.jar")
            .arg(installer)
            .toUtf8();
    }
    return QByteArray(); // optifine / 认不出的:没有直链
}

sxcl_install_assets assetsLevelFrom(const QString &level) {
    const QString text = level.trimmed().toLower();
    if (text == QLatin1String("none"))
        return SXCL_INSTALL_ASSETS_NONE;
    if (text == QLatin1String("index"))
        return SXCL_INSTALL_ASSETS_INDEX;
    if (text == QLatin1String("full"))
        return SXCL_INSTALL_ASSETS_FULL;
    return SXCL_INSTALL_ASSETS_DEFAULT;
}

// 毫秒级墙钟(算速度用)。不受系统时间调整影响。
qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

QString humanBytes(qint64 bytes) {
    if (bytes < 0)
        return QStringLiteral("—");
    const double mb = double(bytes) / (1024.0 * 1024.0);
    if (mb >= 1.0)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    const double kb = double(bytes) / 1024.0;
    if (kb >= 1.0)
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

} // namespace

InstallWorker::InstallWorker(InstallRequest request, QObject *parent)
    : QObject(parent), m_request(std::move(request)) {
    qRegisterMetaType<sxcl::ui::InstallProgress>("sxcl::ui::InstallProgress");
}

InstallWorker::~InstallWorker() {
    // 页面销毁时先请工作线程收工:取消是异步的,join 等它真的退出。
    // 不 detach —— 否则线程会拿着已经析构的 this 去回调,那是崩溃,不是"偶发"
    // (与 settings_page.cpp 的 JavaSettingCard 同一口径)。
    if (m_thread != nullptr) {
        m_cancel.store(true);
        m_thread->quit();
        m_thread->wait();
        delete m_thread; // 线程对象归本类所有(没有 parent,也没有 deleteLater)
        m_thread = nullptr;
    }
}

void InstallWorker::start() {
    if (m_started.exchange(true))
        return; // 只起一次
    // ★ QThread 这里**不能**给 parent(this):带 parent 的对象 moveToThread 会被 Qt 拒绝
    //   ("QObject::moveToThread: Cannot move objects with a parent"),run() 就还在界面线程里跑 ——
    //   界面会卡住。线程对象的生命周期由本类的析构函数负责(quit + wait + deleteLater)。
    // 线程对象没有 parent,**由本类析构函数 delete**(不接 finished->deleteLater:
    // 那样析构里再 delete 会与排队的删除撞成二次释放)。
    m_thread = new QThread();
    // 工作对象搬到新线程:槽 run() 在那条线程里执行,界面线程的 start() 立刻返回。
    moveToThread(m_thread);
    connect(m_thread, &QThread::started, this, &InstallWorker::run);
    connect(m_thread, &QThread::finished, this, [this] { m_running.store(false); });
    m_thread->start();
}

void InstallWorker::cancel() {
    // 只置一个原子位 —— 可以在界面线程直接调,不需要排队(排队要等 run() 让出事件循环,
    // 而 run() 是阻塞的,排队等于取消不了)。
    m_cancel.store(true);
}

bool InstallWorker::running() const { return m_running.load(); }

int InstallWorker::cbCancelled(void *userdata) {
    return (userdata != nullptr && static_cast<InstallWorker *>(userdata)->m_cancel.load()) ? 1 : 0;
}

void InstallWorker::emitLog(const QString &line) {
    uiTrace(QStringLiteral("install | ") + line);
    emit logLine(line);
}

void InstallWorker::cbProgress(void *userdata, const sxcl_install_progress *progress) {
    if (userdata == nullptr || progress == nullptr)
        return;
    static_cast<InstallWorker *>(userdata)->onProgress(progress, false);
}

void InstallWorker::onProgress(const sxcl_install_progress *p, bool force) {
    const QString stageId = QString::fromUtf8(sxcl_install_stage_id(p->stage));
    const QString stageName = QString::fromUtf8(sxcl_install_stage_name(p->stage));

    InstallProgress update;
    update.stageIndex = int(p->stage_index);
    update.stageTotal = int(p->stage_total);
    update.stageId = stageId;
    update.stageName = stageName;
    update.stagePercent = p->stage_percent;
    update.percent = p->percent;
    update.bytesDone = p->bytes_done;
    update.bytesTotal = p->bytes_total;
    update.filesDone = qint64(p->files_done);
    update.filesTotal = qint64(p->files_total);
    update.filesSkipped = qint64(p->files_skipped);
    update.filesFailed = qint64(p->files_failed);
    update.status = QString::fromUtf8(p->status);
    update.current = QString::fromUtf8(p->current);

    const qint64 now = nowMs();
    bool emitIt = force;
    bool stageChanged = false;
    {
        // 回调可能来自引擎的多个工作线程:节流状态与速度必须上锁(install.h:194-197)。
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_lastBytesMs == 0 || p->bytes_done < m_lastBytes) {
            // 第一次 / 换了阶段(字节回退)-> 重新起算速度
            m_lastBytesMs = now;
            m_lastBytes = p->bytes_done;
            m_speedBps = 0.0;
        }
        const qint64 dt = now - m_lastBytesMs;
        if (dt >= 250) {
            const qint64 db = p->bytes_done - m_lastBytes;
            if (db >= 0)
                m_speedBps = double(db) * 1000.0 / double(dt);
            m_lastBytesMs = now;
            m_lastBytes = p->bytes_done;
        }
        update.speedBps = m_speedBps;
        if (m_speedBps > 1.0 && p->bytes_total > p->bytes_done) {
            const double remain = double(p->bytes_total - p->bytes_done) / m_speedBps;
            update.etaSeconds = qint64(remain + 0.5);
        } else {
            update.etaSeconds = -1;
        }

        // 节流:最多 10 次/秒(与 Python 下载进度页的 100ms 节流同一口径)。
        // 阶段切换(STAGE_BEGIN/END)、清理、结束**必须发得出去**,否则界面会停在旧阶段。
        if (int(p->stage_index) != m_lastStage) {
            m_lastStage = int(p->stage_index);
            emitIt = true;
            stageChanged = true;
        }
        if (now - m_lastEmitMs >= 100)
            emitIt = true;
        if (emitIt)
            m_lastEmitMs = now;
    }
    if (!emitIt)
        return;

    // 阶段**开始**进运行日志(默认级别就看得见):这是安装时间线的骨架。
    // 每秒的百分比/速度/剩余**不进**默认日志(那是刷屏);它们仍按原样进界面,
    // 需要留档时把日志级别开到 debug 即可(uiTrace 那条路会写进同一个文件)。
    if (stageChanged) {
        SXCL_LOG_I("install", "阶段开始 %d/%d:%s(%s) 整体 %d%% 文件 %lld/%lld",
                   update.stageIndex + 1, update.stageTotal, update.stageName.toUtf8().constData(),
                   update.stageId.toUtf8().constData(), update.percent, update.filesDone,
                   update.filesTotal);
    }

    // 一条可以逐行读的追踪:阶段(中文+稳定 id)/ 阶段百分比 / 整体百分比 / 速度 / 剩余 /
    // 字节 / 文件数 / 当前文件。这是"进度回调真实日志"的来源,不是另算的。
    QString line = QStringLiteral("阶段 %1/%2 %3(%4) [%5%] 整体 %6%")
                       .arg(update.stageIndex + 1)
                       .arg(update.stageTotal)
                       .arg(stageName, stageId)
                       .arg(update.stagePercent)
                       .arg(update.percent);
    if (update.speedBps > 0.0)
        line += QStringLiteral(" | %1/s").arg(humanBytes(qint64(update.speedBps)));
    if (update.etaSeconds >= 0)
        line += QStringLiteral(" | 剩余 %1s").arg(update.etaSeconds);
    if (update.bytesTotal > 0)
        line += QStringLiteral(" | %1/%2").arg(humanBytes(update.bytesDone),
                                               humanBytes(update.bytesTotal));
    if (update.filesTotal > 0)
        line += QStringLiteral(" | 文件 %1/%2").arg(update.filesDone).arg(update.filesTotal);
    if (update.filesSkipped > 0)
        line += QStringLiteral("(跳过 %1)").arg(update.filesSkipped);
    if (update.filesFailed > 0)
        line += QStringLiteral("(失败 %1)").arg(update.filesFailed);
    if (!update.current.isEmpty())
        line += QStringLiteral(" | %1").arg(update.current);
    emitLog(line);

    emit progress(update);
}

void InstallWorker::run() {
    m_running.store(true);

    // ── 1. 计划(字符串要活到 sxcl_install_run 返回,所以全部落在本函数的 QByteArray 上)──
    const QString instance =
        m_request.instanceName.trimmed().isEmpty() ? m_request.versionId : m_request.instanceName;
    const QByteArray gameDir = QDir::fromNativeSeparators(m_request.gameDir).toUtf8();
    const QByteArray versionId = m_request.versionId.toUtf8();
    const QByteArray instanceName = instance.toUtf8();
    const QByteArray loaderId = m_request.loaderType.trimmed().toLower().toUtf8();
    const QByteArray loaderVersion = m_request.loaderVersion.toUtf8();
    const QByteArray javaPath = QDir::fromNativeSeparators(m_request.javaPath).toUtf8();
    // 安装器地址**在传输后端就绪之后**才算:Fabric 要先去 maven-metadata.xml 问安装器版本(联网)。
    QByteArray installerUrl;
    const QByteArray mavenMirror;

    const sxcl_loader_kind loaderKind = sxcl_loader_kind_from_id(loaderId.constData());
    const bool hasLoader = (loaderKind != SXCL_LOADER_VANILLA);

    if (gameDir.isEmpty() || versionId.isEmpty()) {
        SXCL_LOG_E("install", "没开始就失败:游戏目录或版本号为空(目录='%s' 版本='%s')",
                   m_request.gameDir.toUtf8().constData(), m_request.versionId.toUtf8().constData());
        emit finished(false, false, SXCL_INSTALL_ERR_ARG, false,
                      QStringLiteral("参数不完整:游戏目录与版本号都必须有"), QString());
        m_running.store(false);
        return;
    }
    if (hasLoader && javaPath.isEmpty()) {
        SXCL_LOG_E("install", "没开始就失败:装加载器要 Java,但一个都没选到");
        emit finished(false, false, SXCL_INSTALL_ERR_ARG, false,
                      QStringLiteral("装加载器要指定 Java(安装器本身是个 Java 程序)"),
                      QStringLiteral("阶段:执行加载器安装"));
        m_running.store(false);
        return;
    }

    // ── 2. 下载参数:与设置页下载 JRE、命令行前端同一口径(环境变量 > 设置文件 > 默认)──
    sxcl_settings_download dl;
    memset(&dl, 0, sizeof(dl));
    char cacheFile[600];
    cacheFile[0] = '\0';
    {
        const QByteArray settingsPath = m_request.settingsFile.toUtf8();
        char cfg[1024];
        char cfgErr[128];
        const char *path = nullptr;
        if (!settingsPath.isEmpty()) {
            path = settingsPath.constData();
        } else if (sxcl_settings_default_path(cfg, sizeof(cfg), cfgErr, sizeof(cfgErr)) ==
                   SXCL_SETTINGS_OK) {
            path = cfg;
        }
        if (path != nullptr) {
            if (sxcl_settings *settings = sxcl_settings_open(path)) {
                sxcl_settings_resolve_download(settings, &dl);
                sxcl_settings_free(settings);
            }
        }
        if (dl.cache_dir[0] != '\0' && sxcl_fs_mkdirs(dl.cache_dir) == 0) {
            // 与命令行前端同口径:<缓存目录>/hashes.txt(sxcl-dl main.c:447)
            std::snprintf(cacheFile, sizeof(cacheFile), "%s/hashes.txt", dl.cache_dir);
        }
    }

    // ── 3. 引擎配置。**必须活到 install 返回**:引擎会一直持有任务指针(engine.h:87-93)──
    sxcl_engine_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.workers = dl.workers;
    opts.rate_bps = dl.rate_bps;
    opts.max_conn_per_file = dl.max_conn_per_file;
    opts.cache_path = cacheFile[0] != '\0' ? cacheFile : nullptr;
#if defined(SXCL_UI_HAVE_QT_TRANSPORT)
    // Qt 后端有线程亲和性,必须**每个工作线程一个**(engine.h:75-77)—— 核心库按需调用它。
    sxcl_transport_qt_bootstrap();
    opts.transport_factory = [](void *) -> sxcl_transport * { return sxcl_transport_qt_create(); };
#else
    SXCL_LOG_E("install", "没开始就失败:本次构建没有链接 Qt Network 传输后端(sxcl_net_qt)");
    emit finished(false, false, SXCL_INSTALL_ERR_IO, false,
                  QStringLiteral("本次构建没有链接 Qt Network 传输后端(sxcl_net_qt),无法下载"),
                  QStringLiteral("需要重新配置 -DSXCL_BUILD_QT_TRANSPORT=ON 后重建界面"));
    m_running.store(false);
    return;
#endif

    // ── 3.5 加载器安装器地址 ──
    // Fabric 的安装器 maven 版本号 = **安装器自己的版本**(实测 latest=1.1.2),不是 loader 版本
    // (0.16.x/0.19.x):拿 loader 版本拼 URL 实测 404。先去 maven-metadata.xml 问(镜像优先),
    // 解析结果在本次安装里只拉一次;问不到才退回老写法,并在日志里写明"用了猜测值"。
    QString fabricInstaller;
    if (loaderKind == SXCL_LOADER_FABRIC) {
        if (m_fabricInstallerVersion.isEmpty()) {
            QString source;
            m_fabricInstallerVersion = resolveFabricInstallerVersion(&opts, &source);
            if (!m_fabricInstallerVersion.isEmpty()) {
                SXCL_LOG_I("install", "Fabric 安装器版本=%s 来源=%s",
                           m_fabricInstallerVersion.toUtf8().constData(),
                           source.toUtf8().constData());
                emitLog(QStringLiteral("Fabric 安装器版本 %1(来自 %2)")
                            .arg(m_fabricInstallerVersion, source));
            } else {
                SXCL_LOG_W("install",
                           "Fabric 安装器版本取不到(镜像与官方都不通)—— 退回**猜测值**:"
                           "拿 loader 版本 %s 当安装器版本(大概率 404)",
                           m_request.loaderVersion.toUtf8().constData());
                emitLog(QStringLiteral("[注意] 取不到 Fabric 安装器版本清单,用了**猜测值** %1"
                                       "(拿 loader 版本当安装器版本,大概率 404)")
                            .arg(m_request.loaderVersion));
            }
        } else {
            emitLog(QStringLiteral("Fabric 安装器版本 %1(本次安装已解析过,清单不重复拉)")
                        .arg(m_fabricInstallerVersion));
        }
        fabricInstaller = m_fabricInstallerVersion;
    }
    installerUrl = installerUrlFor(m_request.loaderType, m_request.versionId,
                                  m_request.loaderVersion, fabricInstaller);
    if (hasLoader && installerUrl.isEmpty()) {
        // **如实报**:不去猜一个 URL 然后让用户看到 404;
        // OptiFine 没有稳定的 maven 直链,这条分支就是它的实话。
        SXCL_LOG_E("install", "没开始就失败:%s 的安装器地址拿不到(加载器版本='%s')",
                   QByteArray(sxcl_loader_kind_name(loaderKind)).constData(),
                   m_request.loaderVersion.toUtf8().constData());
        emit finished(false, false, SXCL_INSTALL_ERR_LOADER, false,
                      QStringLiteral("%1 的安装器地址拿不到(界面只认 Forge/NeoForge/Fabric 的 "
                                     "maven 直链;OptiFine 没有稳定直链,需要先手工备好安装器 jar)")
                          .arg(QString::fromUtf8(sxcl_loader_kind_name(loaderKind))),
                      QStringLiteral("阶段:下载加载器安装器 · 已选版本 %1")
                          .arg(m_request.loaderVersion.isEmpty() ? QStringLiteral("(空)")
                                                                 : m_request.loaderVersion));
        m_running.store(false);
        return;
    }

    sxcl_install_plan plan;
    memset(&plan, 0, sizeof(plan));
    plan.game_dir = gameDir.constData();
    plan.version_id = versionId.constData();
    plan.instance_name = instanceName.constData();
    plan.loader = loaderKind;
    plan.loader_version = hasLoader ? loaderVersion.constData() : nullptr;
    plan.installer_url = hasLoader ? installerUrl.constData() : nullptr;
    plan.java_path = javaPath.isEmpty() ? nullptr : javaPath.constData();
    plan.assets = assetsLevelFrom(m_request.assetsLevel);
    plan.keep_installer = m_request.keepInstaller;
    plan.engine_opts = &opts;
    // ★ 下载源(src/ui/workers/ui_paths.cpp 的 uiDownloadSource()):设置里选 bmclapi(默认)或 auto
    //   -> **每个文件都镜像优先**,官方作第二候选;选 mojang -> 官方优先,镜像兜底。
    //   核心库侧的实现是 sxcl_version_plan_prefer_mirror()(plan 上只应调用一次)。
    //   mirror_base 留空 = 核心库用 SXCL_MIRROR_BMCLAPI_BASE(资源对象自动走它 + /assets)。
    plan.prefer_mirror = (uiDownloadSource() != QLatin1String("mojang")) ? 1 : 0;
    uiTrace(QStringLiteral("install | 下载源=%1 prefer_mirror=%2")
                .arg(uiDownloadSource())
                .arg(plan.prefer_mirror));
    // 安装的"前提"进运行日志:目录/版本/实例/加载器 + 下载参数(默认级别就看得见)。
    // 失败时这几行就是"当时到底按什么在装"的唯一凭据。
    SXCL_LOG_I("install", "计划:游戏目录=%s 版本=%s 实例=%s 加载器=%s 资产=%s 安装器=%s",
               QDir::toNativeSeparators(m_request.gameDir).toUtf8().constData(),
               m_request.versionId.toUtf8().constData(), instance.toUtf8().constData(),
               QString::fromUtf8(sxcl_loader_kind_name(loaderKind)).toUtf8().constData(),
               m_request.assetsLevel.isEmpty() ? "default" : m_request.assetsLevel.toUtf8().constData(),
               hasLoader ? (installerUrl.isEmpty() ? "(没有直链)" : installerUrl.constData())
                         : "(不需要)");
    SXCL_LOG_I("install",
               "下载源=%s prefer_mirror=%d 引擎:workers=%d rate=%.0fB/s 单文件连接=%d 哈希缓存=%s",
               uiDownloadSource().toUtf8().constData(), plan.prefer_mirror, opts.workers,
               opts.rate_bps, opts.max_conn_per_file,
               cacheFile[0] != '\0' ? cacheFile : "(不用)");
    if (!mavenMirror.isEmpty())
        plan.loader_mirror_maven = mavenMirror.constData();

    // ── 4. 先报计划:界面照它画阶段行(阶段表**只认核心库那一份**,UI 不另写)──
    {
        QStringList ids;
        QStringList names;
        const size_t count = sxcl_install_plan_stage_count(&plan);
        for (size_t i = 0; i < count; ++i) {
            const sxcl_install_stage stage = sxcl_install_plan_stage_at(&plan, i);
            ids.append(QString::fromUtf8(sxcl_install_stage_id(stage)));
            names.append(QString::fromUtf8(sxcl_install_stage_name(stage)));
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stageIds = ids;
            m_stageNames = names;
        }
        emitLog(QStringLiteral("计划:游戏目录 %1 · 版本 %2 · 实例 %3 · 加载器 %4 · 资产级别 %5")
                    .arg(QDir::toNativeSeparators(m_request.gameDir), m_request.versionId, instance,
                         QString::fromUtf8(sxcl_loader_kind_name(loaderKind)),
                         m_request.assetsLevel.isEmpty() ? QStringLiteral("default")
                                                         : m_request.assetsLevel));
        emitLog(QStringLiteral("计划:%1 个阶段 -> %2").arg(count).arg(names.join(QStringLiteral(" → "))));
        emit planReady(ids, names);
    }

    // ── 5. 跑(阻塞在工作线程里,界面线程不受影响)──
    // **必须复制一份 IO 表再填 userdata**。
    // sxcl_install_default_io() 返回的是静态常量表(kDefaultIo),它的 userdata 是 NULL
    // (install.c:1307-1313),而"取版本清单"那条路正是从 userdata 里取 engine_opts 的
    // (sxcl_install_http_get_text / default_fetch_text,install.c:1335-1340)。
    // 不填的后果**实测过**:阶段 0 直接失败 —— "取版本清单失败: 取文本需要传输后端
    // (engine_opts.transport_factory 为空)",整个安装一步都跑不了。
    // 自己写一个自己的 IO 表也行,但复制默认表更稳:默认实现以后加了钩子这里不会漏。
    sxcl_install_io io = *sxcl_install_default_io();
    io.userdata = &opts;

    sxcl_install_request req;
    memset(&req, 0, sizeof(req));
    req.plan = &plan;
    req.io = &io;
    req.on_progress = &InstallWorker::cbProgress;
    req.userdata = this;
    req.is_cancelled = &InstallWorker::cbCancelled;
    req.cancel_userdata = this;

    sxcl_install_result result;
    memset(&result, 0, sizeof(result));
    const int rc = sxcl_install_run(&req, &result);

    // ── 6. 收尾:三种终态分开,失败必须带**真实原因** ──
    const bool ok = (rc == SXCL_INSTALL_OK);
    const bool cancelled = result.cancelled != 0 || rc == SXCL_INSTALL_ERR_CANCELLED;

    QString detail = QStringLiteral("阶段 %1/%2").arg(result.stages_done).arg(
        sxcl_install_plan_stage_count(&plan));
    if (result.files_skipped > 0)
        detail += QStringLiteral(" · 跳过 %1 个已完好文件").arg(qulonglong(result.files_skipped));
    if (result.files_failed > 0)
        detail += QStringLiteral(" · 非致命失败 %1 个文件").arg(qulonglong(result.files_failed));
    if (result.bytes_done > 0)
        detail += QStringLiteral(" · %1").arg(humanBytes(result.bytes_done));
    if (result.natives_files >= 0)
        detail += QStringLiteral(" · natives %1 个").arg(result.natives_files);

    if (ok) {
        SXCL_LOG_I("install", "安装成功:实例=%s 阶段=%d/%llu 跳过文件=%llu 非致命失败=%llu 字节=%llu",
                   instance.toUtf8().constData(), result.stages_done,
                   (unsigned long long)sxcl_install_plan_stage_count(&plan),
                   (unsigned long long)result.files_skipped,
                   (unsigned long long)result.files_failed,
                   (unsigned long long)result.bytes_done);
        emitLog(QStringLiteral("完成:版本 %1 安装成功(%2)").arg(instance, detail));
        emit finished(true, false, rc, false,
                      QStringLiteral("版本「%1」安装完成").arg(instance), detail);
    } else if (cancelled) {
        // **不是成功**:取消是独立终态,界面必须照实显示"已取消"
        SXCL_LOG_W("install", "安装已取消:实例=%s 阶段=%d/%llu 已完成字节=%llu", 
                   instance.toUtf8().constData(), result.stages_done,
                   (unsigned long long)sxcl_install_plan_stage_count(&plan),
                   (unsigned long long)result.bytes_done);
        emitLog(QStringLiteral("完成:已取消(%1)").arg(detail));
        emit finished(false, true, rc, false,
                      QStringLiteral("已取消:未完成的下载产物(.part)已清理"), detail);
    } else {
        const QString stageName =
            QString::fromUtf8(sxcl_install_stage_name(result.fail_stage));
        const QString reason = QString::fromUtf8(result.error).trimmed().isEmpty()
                                   ? QStringLiteral("核心库没有给出原因(码 %1)")
                                         .arg(QString::fromUtf8(sxcl_install_code_name(rc)))
                                   : QString::fromUtf8(result.error);
        SXCL_LOG_E("install",
                   "安装失败:码=%s 失败阶段=%d/%llu(%s) 原因=%s 可重试=%d 跳过=%llu 失败文件=%llu "
                   "字节=%llu",
                   sxcl_install_code_name(rc), result.fail_stage_index + 1,
                   (unsigned long long)sxcl_install_plan_stage_count(&plan),
                   stageName.toUtf8().constData(), reason.toUtf8().constData(), result.retryable,
                   (unsigned long long)result.files_skipped,
                   (unsigned long long)result.files_failed,
                   (unsigned long long)result.bytes_done);
        emitLog(QStringLiteral("完成:失败 [%1] %2(失败阶段 %3/%4 %5)")
                    .arg(QString::fromUtf8(sxcl_install_code_name(rc)), reason)
                    .arg(result.fail_stage_index + 1)
                    .arg(sxcl_install_plan_stage_count(&plan))
                    .arg(stageName));
        emit finished(false, false, rc, result.retryable != 0,
                      QStringLiteral("%1(失败阶段:%2)").arg(reason, stageName),
                      detail + QStringLiteral(" · %1")
                                   .arg(result.retryable ? QStringLiteral("建议重试(网络类)")
                                                         : QStringLiteral("不建议直接重试")));
    }

    m_running.store(false);
}

} // namespace sxcl::ui
