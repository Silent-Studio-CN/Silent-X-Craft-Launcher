/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* Fabric 安装器地址的**真实验收**(要联网 -> 没开 SXCL_ACCEPT_NETWORK=1 就跳过,ctest 不受影响):
 *
 *   现状(改前):URL 用 **loader 版本** 当 fabric-installer 的 maven 版本,实测必 404
 *     https://maven.fabricmc.net/net/fabricmc/fabric-installer/0.16.9/fabric-installer-0.16.9.jar
 *   改后:maven-metadata.xml 的 <latest> 才是安装器自己的版本(实测 1.1.2)。
 *
 * 这条验收跑的是**界面点"开始下载"那条路**(sxcl_ui_core 的 InstallWorker),装到临时游戏目录:
 *   ① 打印解析到的 fabric-installer 版本;
 *   ② 打印实际下载到的安装器 jar 路径与字节数;
 *   ③ 打印退出码/失败原因。
 * 资产级别钉成 none(省掉几百 MB 资源;**加载器安装器那一段是真的跑**)。
 * 目录固定在 CWD 下的 _sxcl_fabric_accept_<pid>(带进程号 = 并行跑也互不踩),**绝不碰任何真实游戏目录**。 */

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTimer>

#include <cstdio>

#include "workers/install_worker.h"

using namespace sxcl::ui;

namespace {

int g_exitCode = -1;
QString g_message;
QString g_detail;
bool g_cancelled = false;
bool g_ok = false;
QString g_installerJar;

void line(const QString &text) {
    std::printf("%s\n", text.toUtf8().constData());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariable("SXCL_ACCEPT_NETWORK") != QLatin1String("1")) {
        std::printf("SKIP:Fabric 安装器验收要联网(设 SXCL_ACCEPT_NETWORK=1 才跑)\n");
        return 0;
    }

    // 夹具目录带**进程号**:并行跑两份验收时各用各的
    const QString root = QDir::current().absoluteFilePath(
        QStringLiteral("_sxcl_fabric_accept_%1").arg(QCoreApplication::applicationPid()));
    // 只清"这次真的会装进去的那个目录";root 留着 —— settings.conf 允许外部先摆好
    // (例如把 download.source 钉成 mojang,排查镜像不通时的差异)
    const QString game = root + QStringLiteral("/game");
    QDir(game).removeRecursively();
    QDir().mkpath(game);

    InstallRequest req;
    req.gameDir = game;
    req.versionId = qEnvironmentVariable("SXCL_ACCEPT_MC", QStringLiteral("1.21.11"));
    req.instanceName = qEnvironmentVariable("SXCL_ACCEPT_INSTANCE",
                                            req.versionId + QStringLiteral("-fabric-accept"));
    req.loaderType = QStringLiteral("fabric");
    req.loaderVersion = qEnvironmentVariable("SXCL_ACCEPT_LOADER", QStringLiteral("0.19.5"));
    req.javaPath = qEnvironmentVariable("SXCL_ACCEPT_JAVA",
                                        QStringLiteral("D:/jdk17/bin/java.exe"));
    req.settingsFile = root + QStringLiteral("/settings.conf"); // 不存在 = 全用默认下载参数
    req.assetsLevel = QStringLiteral("none");
    req.keepInstaller = 1; // 留着安装器 jar:验收要量它的大小

    line(QStringLiteral("验收夹具:游戏目录=%1 MC=%2 实例=%3 加载器=fabric %4 Java=%5")
             .arg(QDir::toNativeSeparators(game), req.versionId, req.instanceName,
                  req.loaderVersion, req.javaPath));

    InstallWorker worker(req, &app);
    QObject::connect(&worker, &InstallWorker::logLine, &app, [](const QString &text) {
        line(QStringLiteral("  [log] ") + text);
    });
    QObject::connect(&worker, &InstallWorker::finished, &app,
                     [&app](bool ok, bool cancelled, int code, bool retryable,
                            const QString &message, const QString &detail) {
                         g_ok = ok;
                         g_cancelled = cancelled;
                         g_exitCode = code;
                         g_message = message;
                         g_detail = detail;
                         line(QStringLiteral("退出码(核心库返回码)= %1  可重试=%2  取消=%3")
                                  .arg(code)
                                  .arg(retryable)
                                  .arg(cancelled ? 1 : 0));
                         line(QStringLiteral("结果:ok=%1 原因=%2").arg(ok ? 1 : 0).arg(message));
                         if (!detail.isEmpty())
                             line(QStringLiteral("明细:%1").arg(detail));
                         app.quit();
                     });

    worker.start();
    QTimer::singleShot(20 * 60 * 1000, &app, [&app] {
        line(QStringLiteral("验收超时(20 分钟),主动取消"));
        app.quit();
    });
    app.exec();

    // ── 判定 ──
    const QString jarPath = game + QStringLiteral("/versions/") + req.instanceName +
                            QStringLiteral("/fabric-installer.jar");
    const QFileInfo jar(jarPath);
    line(QStringLiteral("安装器 jar:%1").arg(QDir::toNativeSeparators(jarPath)));
    line(QStringLiteral("安装器 jar 大小:%1 字节(存在=%2)")
             .arg(jar.size())
             .arg(jar.exists() ? 1 : 0));

    int fails = 0;
    if (!jar.exists() || jar.size() <= 0) {
        line(QStringLiteral("[FAIL] 安装器 jar 没下到(见上面的日志:真 404 会写明 HTTP 状态)"));
        ++fails;
    } else {
        line(QStringLiteral("[ ok ] 安装器 jar 真的下到了"));
    }
    if (g_exitCode == 404) {
        line(QStringLiteral("[FAIL] 安装器地址 404"));
        ++fails;
    }
    // 加载器那一段必须真的跑过(下载+执行);即便后面因为 Java 版本失败,也说明地址是对的
    if (g_exitCode == 0 || jar.exists()) {
        line(QStringLiteral("[ ok ] 地址可用(退出码 %1)").arg(g_exitCode));
    } else {
        ++fails;
    }
    (void)g_ok;
    (void)g_cancelled;
    std::printf("Fabric 验收结束:失败 %d 项\n", fails);
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}
