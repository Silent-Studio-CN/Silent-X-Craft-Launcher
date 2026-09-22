/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* 事故夹具(界面侧)——「一唤醒就自动续装」这件事不许再发生:
 *
 *   ① 默认(没有 SXCL_UI_RESUME_TASKS=1):启动后**不**建下载进度页、**不**起安装,
 *      只在任务页登记一条可点击的"上次未完成"记录 + InfoBar 告知;
 *   ② 目标目录变了(记录里的 gameDir != 这次解析出来的):照样不续跑,登记"上次目标目录已变";
 *   ③ 旧文件(schema 1,没有 gameDir):照读不炸,同样不自动续装(兼容);
 *   ④ 用户点那条记录 = 走产品路径(navigateToTask,与任务卡 onClick 同一个入口):
 *      目标已存在 -> 拒装 + InfoBar,绝不覆盖用户的版本 JSON;
 *   ⑤ SXCL_UI_RESUME_TASKS=1(验收钉子):自动续跑确实恢复了(页面被建出来),
 *      而且**即使钉子开着**,目标已存在时安装引擎也会拒装(核心库预检,不覆盖)。
 *
 * 每个用例跑在**独立进程**里(与真实启动器一致:一个进程一个窗口;同一个进程里反复
 * 建/拆 MainWindow 会踩到界面层已有的生命周期问题,那不属于本轮四条根因)。
 * 设置文件、游戏目录、待恢复任务文件全部钉在**带进程号的临时目录**(_sxcl_ui_resume_fixture_<pid>,
 * 跑完自己清掉):不碰用户真实配置与游戏目录,也不和并行跑的另一份 ctest 抢同一个路径。 */

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMetaObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStackedWidget>
#include <QThread>
#include <QWidget>

#include <cstdio>

#include "fluent/fluent_controls.h" // InfoBar(告知通路)
#include "main_window.h"

using namespace sxcl::ui;

namespace {

int g_checks = 0;
int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString()) {
    ++g_checks;
    if (!ok)
        ++g_fail;
    std::printf("%s %s%s%s\n", ok ? "[ ok ]" : "[FAIL]", what.toUtf8().constData(),
                detail.isEmpty() ? "" : "  ->  ", detail.toUtf8().constData());
    std::fflush(stdout);
}

void section(const QString &title) {
    std::printf("\n---- %s ----\n", title.toUtf8().constData());
    std::fflush(stdout);
}

// 夹具根目录**带进程号**:同一棵树里并行跑两份 ctest 时各用各的目录,互不踩
// (不靠"跑的时候别并发" —— 那种约定迟早会变成假红灯)。跑完由驱动进程清掉自己这一份。
QString fixtureRoot() {
    static const QString root = [] {
        // 子进程(用例进程)用**驱动进程**给的那一份:SXCL_UI_RESUME_ROOT。
        // (子进程自己按 pid 再算一个的话,父进程清不到它那份,而且 SXCL_UI_SETTINGS 钉的
        //  路径会和 pending_tasks.json/gameDir 的路径分家 —— 实测踩过。)
        const QString inherited = qEnvironmentVariable("SXCL_UI_RESUME_ROOT");
        if (!inherited.isEmpty())
            return QDir::fromNativeSeparators(inherited);
        const qint64 pid = QCoreApplication::applicationPid();
        return QDir::current().absoluteFilePath(
            QStringLiteral("_sxcl_ui_resume_fixture_%1").arg(pid));
    }();
    return root;
}
QString settingsFile() { return fixtureRoot() + QStringLiteral("/settings.conf"); }
QString pendingFile() { return fixtureRoot() + QStringLiteral("/pending_tasks.json"); }
QString gameDir(const QString &name) { return fixtureRoot() + QStringLiteral("/") + name; }

// 跑事件循环(缩成一个名字,免得三处各写一遍;兜底定时器由调用方先挂好)
int app_exec_placeholder(QCoreApplication *core) {
    Q_UNUSED(core);
    return QCoreApplication::exec();
}

QString instanceId() { return QStringLiteral("1.20.1"); }
QString taskKey() { return QStringLiteral("download_progress_") + instanceId(); }

void pump(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
}

QStringList allTexts(QWidget *root) {
    QStringList out;
    if (root == nullptr)
        return out;
    for (QLabel *label : root->findChildren<QLabel *>()) {
        if (!label->text().isEmpty())
            out << label->text();
    }
    return out;
}

bool hasText(const QStringList &texts, const QString &needle) {
    for (const QString &t : texts) {
        if (t.contains(needle))
            return true;
    }
    return false;
}

// 全应用范围的 InfoBar 文字(浮层通知挂在窗口的 overlay 上,别只找窗口子树)
QStringList infoBarTexts() {
    QStringList out;
    for (QWidget *w : QApplication::allWidgets()) {
        if (auto *bar = qobject_cast<InfoBar *>(w))
            out += allTexts(bar);
    }
    return out;
}

QWidget *tasksPage(MainWindow &window) {
    return window.findChild<QWidget *>(QStringLiteral("TasksPage"));
}

int taskCount(QWidget *tasks) {
    int count = -1;
    if (tasks != nullptr)
        QMetaObject::invokeMethod(tasks, "taskCount", Qt::DirectConnection, Q_RETURN_ARG(int, count));
    return count;
}

bool hasRoute(MainWindow &window, const QString &key) { return window.sessionPageKeys().contains(key); }

void writePending(const QString &id, const QString &versionId, const QString &instance,
                  const QString &dir, int schema) {
    QJsonObject item;
    item[QStringLiteral("id")] = id;
    item[QStringLiteral("kind")] = QStringLiteral("download");
    item[QStringLiteral("title")] = QStringLiteral("下载 %1").arg(instance);
    item[QStringLiteral("status")] = QStringLiteral("下载中");
    item[QStringLiteral("versionId")] = versionId;
    item[QStringLiteral("versionName")] = instance;
    item[QStringLiteral("instanceName")] = instance;
    item[QStringLiteral("loaderType")] = QStringLiteral("none");
    item[QStringLiteral("loaderVersion")] = QString();
    if (!dir.isEmpty())
        item[QStringLiteral("gameDir")] = dir; // schema 2 才有;schema 1 的夹具不写这个键

    QJsonObject root;
    root[QStringLiteral("schema")] = schema;
    root[QStringLiteral("app")] = QStringLiteral("Silent X Craft Launcher");
    root[QStringLiteral("interrupted")] = true;
    QJsonArray tasks;
    tasks.append(item);
    root[QStringLiteral("tasks")] = tasks;

    QFile file(pendingFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        std::printf("[!!] 夹具:写不了 %s\n", pendingFile().toUtf8().constData());
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
}

/* 用户真实在用的那个版本 JSON(字段齐全,像真装过的一样),带一个只属于它的标记:
 * USER-KEPT-MARKER —— 安装/恢复都不许把它覆盖掉。 */
const char kUserJson[] =
    "{\n"
    "  \"id\": \"1.20.1\",\n"
    "  \"USER-KEPT-MARKER\": \"do-not-overwrite\",\n"
    "  \"type\": \"release\",\n"
    "  \"mainClass\": \"net.minecraft.client.main.Main\",\n"
    "  \"time\": \"2024-01-01T00:00:00+00:00\",\n"
    "  \"releaseTime\": \"2024-01-01T00:00:00+00:00\",\n"
    "  \"assetIndex\": { \"id\": \"5\", \"sha1\": \"1111111111111111111111111111111111111111\","
    " \"size\": 32, \"url\": \"https://example.invalid/5.json\" },\n"
    "  \"downloads\": { \"client\": { \"sha1\": \"2222222222222222222222222222222222222222\","
    " \"size\": 32, \"url\": \"https://example.invalid/client.jar\" } },\n"
    "  \"libraries\": [\n"
    "    { \"name\": \"org.ow2.asm:asm:9.5\", \"downloads\": { \"artifact\": {"
    " \"path\": \"org/ow2/asm/asm/9.5/asm-9.5.jar\","
    " \"sha1\": \"3333333333333333333333333333333333333333\", \"size\": 32,"
    " \"url\": \"https://example.invalid/asm-9.5.jar\" } } }\n"
    "  ]\n"
    "}\n";

void seedExistingTarget(const QString &dir, const QString &instance) {
    const QString path = dir + QStringLiteral("/versions/%1/%1.json").arg(instance);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        std::printf("[!!] 夹具:写不了 %s\n", QDir::toNativeSeparators(path).toUtf8().constData());
    file.write(kUserJson);
    file.close();
}

bool targetMarkerAlive(const QString &dir, const QString &instance) {
    QFile file(dir + QStringLiteral("/versions/%1/%1.json").arg(instance));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return file.readAll().contains("USER-KEPT-MARKER");
}

/* ── 每个用例一个进程:返回失败条数 ── */

int runCase(int n) {
    const QString instance = instanceId();
    const QString key = taskKey();
    const QString dirOk = gameDir(QStringLiteral("game-ok"));
    const QString dirOther = gameDir(QStringLiteral("game-other"));
    QDir().mkpath(dirOk);
    QDir().mkpath(dirOther);

    if (n == 1) {
        section(QStringLiteral("① 默认:不自动续跑,只登记一条可点击记录"));
        qputenv("SXCL_UI_GAME_DIR", QDir::toNativeSeparators(dirOk).toLocal8Bit());
        qunsetenv("SXCL_UI_RESUME_TASKS");
        writePending(key, instance, instance, dirOk, 2);
        MainWindow window;
        window.show();
        pump(300);
        check(!hasRoute(window, key), QStringLiteral("没有建下载进度页(没自动续跑)"));
        check(window.pageStack()->count() == 9,
              QStringLiteral("页面栈还是 9 页(6 侧边栏 + 3 隐藏常驻,没多出临时页)"),
              QStringLiteral("%1").arg(window.pageStack()->count()));
        // 重构后任务页在"更多"里,不再是侧边栏的一项:先切到它的路由,页面才会被建出来。
        window.switchToRoute(QStringLiteral("tasks"));
        QWidget *tasks = tasksPage(window);
        check(tasks != nullptr, QStringLiteral("任务页在"));
        check(taskCount(tasks) == 1, QStringLiteral("任务页登记了 1 条(上次未完成)"),
              QStringLiteral("%1").arg(taskCount(tasks)));
        const QStringList texts = allTexts(tasks);
        check(hasText(texts, QStringLiteral("上次未完成")), QStringLiteral("卡片标题写明「上次未完成」"));
        check(hasText(texts, QStringLiteral("点这里继续安装")),
              QStringLiteral("卡片状态写明「点这里继续安装」(可点击恢复)"));
        const QStringList bars = infoBarTexts();
        check(hasText(bars, QStringLiteral("上次有 1 个任务没做完")), QStringLiteral("InfoBar 告知了(不是静默)"));
        check(hasText(bars, QStringLiteral("没有自动开始")), QStringLiteral("InfoBar 说清「这次没有自动开始」"));
        check(QFileInfo::exists(pendingFile()), QStringLiteral("任务状态文件保留(用户没点之前不消费掉)"));
    } else if (n == 2) {
        section(QStringLiteral("② 目标目录变了 -> 不自动恢复,登记「上次目标目录已变」"));
        qputenv("SXCL_UI_GAME_DIR", QDir::toNativeSeparators(dirOther).toLocal8Bit());
        writePending(key, instance, instance, dirOk, 2);
        MainWindow window;
        window.show();
        pump(300);
        check(!hasRoute(window, key), QStringLiteral("目录不一致 = 没自动续跑"));
        // 重构后任务页在"更多"里,不再是侧边栏的一项:先切到它的路由,页面才会被建出来。
        window.switchToRoute(QStringLiteral("tasks"));
        QWidget *tasks = tasksPage(window);
        check(taskCount(tasks) == 1, QStringLiteral("仍然登记一条(让用户看见)"));
        const QStringList texts = allTexts(tasks);
        check(hasText(texts, QStringLiteral("上次目标目录已变")), QStringLiteral("卡片写明「上次目标目录已变」"));
        check(hasText(texts, QStringLiteral("旧:")), QStringLiteral("卡片带旧目录"));
    } else if (n == 3) {
        section(QStringLiteral("③ 旧文件(schema 1,没有 gameDir)-> 照读不炸,也不自动续装"));
        qputenv("SXCL_UI_GAME_DIR", QDir::toNativeSeparators(dirOk).toLocal8Bit());
        writePending(key, instance, instance, QString(), 1);
        MainWindow window;
        window.show();
        pump(300);
        check(!hasRoute(window, key), QStringLiteral("旧记录没有目录 = 没自动续跑"));
        // 重构后任务页在"更多"里,不再是侧边栏的一项:先切到它的路由,页面才会被建出来。
        window.switchToRoute(QStringLiteral("tasks"));
        QWidget *tasks = tasksPage(window);
        check(taskCount(tasks) == 1, QStringLiteral("旧记录也登记了(兼容读得懂)"));
        check(hasText(allTexts(tasks), QStringLiteral("没有目标目录")), QStringLiteral("卡片说清为什么不能恢复"));
    } else if (n == 4) {
        section(QStringLiteral("④ 用户点那条记录:目标已存在 -> 拒装,绝不覆盖"));
        qputenv("SXCL_UI_GAME_DIR", QDir::toNativeSeparators(dirOk).toLocal8Bit());
        writePending(key, instance, instance, dirOk, 2);
        seedExistingTarget(dirOk, instance);
        MainWindow window;
        window.show();
        pump(300);
        check(!hasRoute(window, key), QStringLiteral("起点仍然没有自动续跑"));
        QMetaObject::invokeMethod(&window, "navigateToTask", Qt::DirectConnection,
                                  Q_ARG(QString, key)); // 与任务卡 onClick 同一个入口
        pump(500);
        check(!hasRoute(window, key), QStringLiteral("目标已存在 -> 点它也不开工"));
        check(hasText(infoBarTexts(), QStringLiteral("没有恢复")), QStringLiteral("给了一句人话(InfoBar)"));
        check(targetMarkerAlive(dirOk, instance), QStringLiteral("用户的版本 JSON 原样还在"));
    } else if (n == 5) {
        section(QStringLiteral("⑤ 验收钉子 SXCL_UI_RESUME_TASKS=1 -> 自动续跑确实回来了"));
        qputenv("SXCL_UI_GAME_DIR", QDir::toNativeSeparators(dirOk).toLocal8Bit());
        writePending(key, instance, instance, dirOk, 2);
        seedExistingTarget(dirOk, instance); // 目标已存在:引擎必须拒装(不覆盖)
        qputenv("SXCL_UI_RESUME_TASKS", "1");
        MainWindow window;
        window.show();
        pump(400);
        check(hasRoute(window, key), QStringLiteral("钉子=1 时自动恢复(建出了下载进度页)"));
        // 9(6 侧边栏 + 3 隐藏常驻) + 1(临时页) = 10
        check(window.pageStack()->count() == 10, QStringLiteral("页面栈多了一个临时页"),
              QStringLiteral("%1").arg(window.pageStack()->count()));
        check(!QFileInfo::exists(pendingFile()), QStringLiteral("恢复过一次就消费掉状态文件(不重复恢复)"));
        pump(3000); // 让安装 worker 跑到"被引擎拒装"为止(核心库预检在网络之前,很快就结束)
        check(targetMarkerAlive(dirOk, instance), QStringLiteral("即使钉子开着,目标已存在也不会被覆盖"));
    } else if (n == 6 || n == 7 || n == 8) {
        // 「结束后关闭」(电脑端)的三条行为。判据:**只有成功才自动退出**;
        // 失败/取消必须留在界面上(否则用户看不到原因);默认关。
        const bool pinOn = (n != 8);           // 8 = 没有钉子(默认关)
        if (pinOn)
            qputenv("SXCL_UI_CLOSE_AFTER_INSTALL", "1");
        else
            qunsetenv("SXCL_UI_CLOSE_AFTER_INSTALL");
        qputenv("SXCL_UI_GAME_DIR", QDir::toNativeSeparators(dirOk).toLocal8Bit());
        section(n == 6 ? QStringLiteral("⑥ 结束后关闭:失败 -> 不退出")
                       : (n == 7 ? QStringLiteral("⑦ 结束后关闭:成功 -> 3 秒后自动退出")
                                 : QStringLiteral("⑧ 结束后关闭默认关:成功也不退出")));
        bool quitFired = false;
        QCoreApplication *core = QCoreApplication::instance();
        QObject::connect(core, &QCoreApplication::aboutToQuit, core,
                         [&quitFired] { quitFired = true; });
        MainWindow window;
        window.show();
        pump(200);
        // 真跑一次事件循环:自动退出会让 exec() **提前**返回(兜底定时器 4.5 秒)。
        // 只看 aboutToQuit 不行 —— 不跑 exec() 的话它永远不会发出来(踩过)。
        QElapsedTimer clock;
        clock.start();
        QTimer::singleShot(4500, core, [] { QCoreApplication::quit(); });
        if (n == 6) {
            QMetaObject::invokeMethod(&window, "notifyInstallFinished", Qt::DirectConnection,
                                      Q_ARG(bool, false), Q_ARG(bool, false));
            (void)app_exec_placeholder(core);
            check(clock.elapsed() >= 4400, QStringLiteral("失败不自动退出(原因留在界面上)"));
        } else if (n == 7) {
            QMetaObject::invokeMethod(&window, "notifyInstallFinished", Qt::DirectConnection,
                                      Q_ARG(bool, true), Q_ARG(bool, false));
            (void)app_exec_placeholder(core);
            check(clock.elapsed() < 4400, QStringLiteral("3 秒后自动退出(exec() 提前返回)"));
            check(quitFired, QStringLiteral("退出时发了 aboutToQuit"));
        } else {
            QMetaObject::invokeMethod(&window, "notifyInstallFinished", Qt::DirectConnection,
                                      Q_ARG(bool, true), Q_ARG(bool, false));
            (void)app_exec_placeholder(core);
            check(clock.elapsed() >= 4400, QStringLiteral("设置默认关 = 成功也不退出"));
        }
        qunsetenv("SXCL_UI_CLOSE_AFTER_INSTALL");
    } else {
        std::printf("[!!] 未知用例 %d\n", n);
        return 1;
    }
    return g_fail;
}

} // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        const QByteArray windir = qgetenv("WINDIR");
        const QString fonts = QString::fromLocal8Bit(windir) + QStringLiteral("/Fonts");
        if (!windir.isEmpty() && QFileInfo::exists(fonts))
            qputenv("QT_QPA_FONTDIR", fonts.toLocal8Bit());
    }
    // 先钉死"读哪一个设置文件/任务状态文件":loadTaskState 从设置文件旁边读 pending_tasks.json
    QDir().mkpath(fixtureRoot());
    qputenv("SXCL_UI_SETTINGS", QDir::toNativeSeparators(settingsFile()).toLocal8Bit());
    qunsetenv("SXCL_UI_RESUME_TASKS");

    QApplication app(argc, argv);

    const QString which = qEnvironmentVariable("SXCL_UI_RESUME_CASE");
    if (!which.isEmpty()) {
        const int n = which.toInt();
        const int fails = runCase(n);
        std::printf("\n==== 用例 %d:断言 %d 条,失败 %d 条 ====\n", n, g_checks, fails);
        std::fflush(stdout);
        return fails == 0 ? 0 : 1;
    }

    // 驱动进程:一个用例一个子进程(真实启动器 = 一个进程一个窗口,窗口只建一次)
    std::printf("夹具根目录(临时): %s\n", fixtureRoot().toUtf8().constData());
    int failed = 0;
    for (int n = 1; n <= 8; ++n) {
        QProcess child;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SXCL_UI_RESUME_CASE"), QString::number(n));
        // 把本次的夹具根目录交给子进程:两边必须用**同一份**(见 fixtureRoot 的注释)
        env.insert(QStringLiteral("SXCL_UI_RESUME_ROOT"),
                   QDir::toNativeSeparators(fixtureRoot()));
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        child.start(QCoreApplication::applicationFilePath(),
                    QStringList{QStringLiteral("-platform"), QStringLiteral("offscreen")});
        if (!child.waitForStarted(15000)) {
            std::printf("[FAIL] 用例 %d 起不来\n", n);
            ++failed;
            continue;
        }
        child.waitForFinished(-1);
        const int rc = child.exitStatus() == QProcess::NormalExit ? child.exitCode() : -1;
        std::printf("用例 %d:子进程退出码 %d\n", n, rc);
        std::fflush(stdout);
        if (rc != 0)
            ++failed;
        // 每个用例之间把夹具清干净(下一个用例自己摆自己那份)
        QFile::remove(pendingFile());
        QDir(gameDir(QStringLiteral("game-ok"))).removeRecursively();
        QDir(gameDir(QStringLiteral("game-other"))).removeRecursively();
    }
    // 跑完清掉**自己这一份**(带进程号的目录;别人那份按定义不归我们管)
    if (!qEnvironmentVariableIsSet("SXCL_UI_RESUME_KEEP_FIXTURE")) {
        const bool removed = QDir(fixtureRoot()).removeRecursively();
        std::printf("夹具目录已清理: %s(%s)\n", fixtureRoot().toUtf8().constData(),
                    removed ? "removed" : "nothing to remove");
    }
    std::printf("\n==== 8 个用例,失败的 %d 个 ====\n", failed);
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
}
