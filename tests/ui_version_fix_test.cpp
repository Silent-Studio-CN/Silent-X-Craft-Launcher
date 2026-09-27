/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 版本选择页(排序 / 分组 / 「修复」)+ 启动页(重写)的验收。用户 2026-09-27 的原话:
//   「关于版本选择,按照 MC 的版本号排,哪个是最新版哪个就放最前面。这个什么逻辑,你自己写。」
//   「错误版不参与大小排序,错误版自己单列一个,错误版在下面按再按照版本号排序。」
//   「这个版本识别,说什么去下载,那破坏的、已经残缺的版本,缺什么东西你给我统计出来;
//     然后把"去下载"改成"修复"」
//   「启动页重写:上面是"启动什么",下面变成一个加载动画,命令行、游戏输出、Java 版本全去掉;
//     等待游戏下面的进度条保留,等待游戏也保留,下面换成一些动态切换的小知识小 tips。」
//
// 这个用例**不联网**:夹具里所有下载地址都指向本进程起的一个本地 HTTP 服务(127.0.0.1 随机端口),
// 版本清单也是本地文件(SXCL_UI_MANIFEST)。所以"点「修复」真的把文件补上了"这件事是
// **真的跑过一遍下载**验出来的,不是断言我们自己报的账。
//
// 为什么要写成 C++ 用例而不是只写 PowerShell 脚本:点「修复」这一步要**真的点那个控件**
// (QMouseEvent 送进页面的 eventFilter),而这个仓库的主程序里没有"点某一行动作"的验收钩子
// (main.cpp 是别人正在改的文件,不去动它)。用例直接建页面 + 送事件,验的还是同一份产品代码。

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QHostAddress>
#include <QSharedPointer>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <functional>

#include "launch_tips.h"
#include "pages/page_factory.h"
#include "workers/instance_scan.h"
#include "workers/version_order.h"

// 被测的东西都在 sxcl::ui 里(页面工厂 / 版本号比较 / 小贴士表)
using namespace sxcl::ui;

namespace {

int g_checks = 0;
int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString()) {
    ++g_checks;
    if (ok) {
        std::printf("  [ok]   %s\n", what.toUtf8().constData());
        return;
    }
    ++g_fail;
    std::printf("  [FAIL] %s%s%s\n", what.toUtf8().constData(), detail.isEmpty() ? "" : " -- ",
                detail.toUtf8().constData());
}

void section(const QString &title) {
    std::printf("\n== %s ==\n", title.toUtf8().constData());
}

/** 转事件循环等条件成立:夹具的 HTTP 服务在**主线程**上应答,所以必须真的转事件循环,
 *  不能只 sleep(那样下载永远等不到响应)。 */
bool waitFor(const std::function<bool()> &done, int timeoutMs = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (done())
            return true;
        QThread::msleep(10);
    }
    QCoreApplication::processEvents();
    return done();
}

// ── 本地 HTTP 服务(只为这个用例存在:把"下载"这条真路跑通,又不碰外网)──

class LocalHttpServer : public QTcpServer {
public:
    QHash<QString, QByteArray> routes; // "/f/x.jar" -> 正文

    quint16 startOnRandomPort() {
        listen(QHostAddress::LocalHost, 0);
        return serverPort();
    }

protected:
    void incomingConnection(qintptr handle) override {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(handle);
        auto buffer = QSharedPointer<QByteArray>::create();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
            buffer->append(socket->readAll());
            if (!buffer->contains("\r\n\r\n"))
                return; // 请求头还没收全
            const QString requestLine = QString::fromLatin1(buffer->left(buffer->indexOf("\r\n")));
            const QStringList parts = requestLine.split(QLatin1Char(' '));
            const QString path = parts.size() >= 2 ? parts.at(1) : QString();
            const bool head = requestLine.startsWith(QLatin1String("HEAD"));
            const QByteArray body = routes.value(path);
            QByteArray response;
            if (!routes.contains(path)) {
                response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            } else {
                response = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                           "Accept-Ranges: none\r\nContent-Length: " +
                           QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
                if (!head)
                    response += body;
            }
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
};

QByteArray sha1Hex(const QByteArray &body) {
    return QCryptographicHash::hash(body, QCryptographicHash::Sha1).toHex();
}

void writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(body);
}

// ── 控件树 dump(与主程序的 SXCL_UI_DUMP 同一类文本,断言按文本做)──

QString dumpTree(QWidget *root, int maxDepth = 14) {
    QStringList lines;
    std::function<void(QWidget *, int)> walk = [&](QWidget *w, int depth) {
        if (depth > maxDepth)
            return;
        QString line = QString(depth * 2, QLatin1Char(' ')) +
                       QString::fromLatin1(w->metaObject()->className());
        if (!w->objectName().isEmpty())
            line += QStringLiteral(" #%1").arg(w->objectName());
        line += QStringLiteral(" (%1,%2 %3x%4)")
                    .arg(w->x())
                    .arg(w->y())
                    .arg(w->width())
                    .arg(w->height());
        if (!w->isVisible())
            line += QStringLiteral(" hidden");
        if (const auto *label = qobject_cast<QLabel *>(w)) {
            if (!label->text().isEmpty())
                line += QStringLiteral(" \"%1\"").arg(label->text());
        }
        if (const auto *bar = qobject_cast<QProgressBar *>(w))
            line += QStringLiteral(" value=%1").arg(bar->value());
        if (const auto *edit = qobject_cast<QPlainTextEdit *>(w))
            line += QStringLiteral(" lines=%1").arg(edit->blockCount());
        lines << line;
        const QList<QObject *> kids = w->children();
        for (QObject *child : kids) {
            if (auto *cw = qobject_cast<QWidget *>(child))
                walk(cw, depth + 1);
        }
    };
    walk(root, 0);
    return lines.join(QLatin1Char('\n'));
}

// ── 版本选择页的一行(按控件树的几何排序 —— 断言的是**屏幕上从上到下**的顺序)──

struct RowView {
    QString id;
    QString state;
    QString note;
    QString action;
    int y = 0;
};

QVector<RowView> visibleRows(QWidget *page, const QStringList &ids) {
    QVector<RowView> rows;
    const QList<QWidget *> all = page->findChildren<QWidget *>();
    for (QWidget *icon : all) {
        if (!icon->objectName().startsWith(QLatin1String("sxclVersionStateIcon_")))
            continue;
        QWidget *card = icon->parentWidget();
        if (card == nullptr || !card->isVisible())
            continue; // 上一轮扫描留下的、正在 deleteLater 的行不算
        RowView row;
        row.state = icon->objectName().mid(int(qstrlen("sxclVersionStateIcon_")));
        row.y = card->mapTo(page, QPoint(0, 0)).y();
        const QList<QLabel *> labels = card->findChildren<QLabel *>();
        for (QLabel *label : labels) {
            if (ids.contains(label->text()))
                row.id = label->text();
            else if (label->objectName() == QLatin1String("sxclVersionRowNote"))
                row.note = label->text();
            else if (label->objectName() == QLatin1String("sxclVersionRowAction"))
                row.action = label->text();
        }
        if (!row.id.isEmpty())
            rows.append(row);
    }
    std::sort(rows.begin(), rows.end(),
              [](const RowView &a, const RowView &b) { return a.y < b.y; });
    return rows;
}

QStringList rowIds(const QVector<RowView> &rows) {
    QStringList ids;
    for (const RowView &row : rows)
        ids << row.id;
    return ids;
}

QWidget *cardOf(QWidget *page, const QString &id) {
    const QList<QWidget *> all = page->findChildren<QWidget *>();
    for (QWidget *icon : all) {
        if (!icon->objectName().startsWith(QLatin1String("sxclVersionStateIcon_")))
            continue;
        QWidget *card = icon->parentWidget();
        if (card == nullptr || !card->isVisible())
            continue;
        const QList<QLabel *> labels = card->findChildren<QLabel *>();
        for (QLabel *label : labels) {
            if (label->text() == id)
                return card;
        }
    }
    return nullptr;
}

/** 点一个控件(真的送鼠标事件:页面那边是 eventFilter 收 MouseButtonRelease)。 */
void clickWidget(QWidget *w) {
    const QPointF local(2, 2);
    const QPointF global = w->mapToGlobal(local.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

QStringList expectedHealthy() {
    return QStringList{QStringLiteral("26.3"),   QStringLiteral("25w14a"), QStringLiteral("1.21.4"),
                       QStringLiteral("1.21.1"), QStringLiteral("1.20.6"), QStringLiteral("1.20.1"),
                       QStringLiteral("1.19.4"), QStringLiteral("1.12.2"), QStringLiteral("1.7.10")};
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    // ── 夹具目录 ──
    const QString work = QDir::tempPath() + QStringLiteral("/sxcl_ui_version_fix");
    QDir(work).removeRecursively();
    const QString game = work + QStringLiteral("/mc");
    QDir().mkpath(game);

    LocalHttpServer server;
    const quint16 port = server.startOnRandomPort();
    const QString base = QStringLiteral("http://127.0.0.1:%1").arg(port);

    const QByteArray jarBody(2048, 'J');
    const QByteArray libBody(1024, 'L');
    const QByteArray legacyJarBody(1500, 'K');
    const QByteArray legacyLibBody(900, 'M');
    server.routes.insert(QStringLiteral("/f/1.18.2.jar"), jarBody);
    server.routes.insert(QStringLiteral("/f/demo.jar"), libBody);
    server.routes.insert(QStringLiteral("/f/1.8.9.jar"), legacyJarBody);
    server.routes.insert(QStringLiteral("/f/legacy.jar"), legacyLibBody);

    auto versionJson = [](const QString &id, const QString &jarUrl, const QByteArray &jar,
                          const QString &libPath, const QString &libUrl, const QByteArray &lib) {
        return QStringLiteral(
                   "{\"id\":\"%1\",\"type\":\"release\","
                   "\"mainClass\":\"net.minecraft.client.main.Main\",\"clientVersion\":\"%1\","
                   "\"downloads\":{\"client\":{\"url\":\"%2\",\"sha1\":\"%3\",\"size\":%4}},"
                   "\"libraries\":[{\"name\":\"org.example:demo:1.0\",\"downloads\":"
                   "{\"artifact\":{\"path\":\"%5\",\"url\":\"%6\",\"sha1\":\"%7\","
                   "\"size\":%8}}}]}")
            .arg(id, jarUrl, QString::fromLatin1(sha1Hex(jar)))
            .arg(jar.size())
            .arg(libPath, libUrl, QString::fromLatin1(sha1Hex(lib)))
            .arg(lib.size())
            .toUtf8();
    };

    // 正常版本(有 JSON 有 jar)
    const QStringList healthy = expectedHealthy();
    for (const QString &id : healthy) {
        writeFile(game + QStringLiteral("/versions/%1/%1.json").arg(id),
                  QStringLiteral("{\"id\":\"%1\",\"type\":\"release\","
                                 "\"mainClass\":\"net.minecraft.client.main.Main\","
                                 "\"clientVersion\":\"%1\"}")
                      .arg(id)
                      .toUtf8());
        writeFile(game + QStringLiteral("/versions/%1/%1.jar").arg(id), QByteArray("fixture-jar"));
    }
    // 残缺:缺游戏本体(JSON 有、jar 没有)
    writeFile(game + QStringLiteral("/versions/1.16.5/1.16.5.json"),
              QByteArray("{\"id\":\"1.16.5\",\"type\":\"release\","
                         "\"mainClass\":\"net.minecraft.client.main.Main\","
                         "\"clientVersion\":\"1.16.5\"}"));
    // 残缺:缺前置版本(1.13.2 没装)
    writeFile(game + QStringLiteral("/versions/1.14.4/1.14.4.json"),
              QByteArray("{\"id\":\"1.14.4\",\"type\":\"release\","
                         "\"mainClass\":\"net.minecraft.client.main.Main\","
                         "\"inheritsFrom\":\"1.13.2\"}"));
    // 残缺:连版本文件都没有(只有一个 jar)
    writeFile(game + QStringLiteral("/versions/1.8.9/1.8.9.jar"), QByteArray("fixture-jar"));
    // 残缺:缺游戏本体 + 缺依赖库(点「修复」要能把这两样都补上)
    writeFile(game + QStringLiteral("/versions/1.18.2/1.18.2.json"),
              versionJson(QStringLiteral("1.18.2"), base + QStringLiteral("/f/1.18.2.jar"), jarBody,
                          QStringLiteral("org/example/demo/1.0/demo-1.0.jar"),
                          base + QStringLiteral("/f/demo.jar"), libBody));
    // 残缺:缺游戏本体(和 1.16.5 同一类,用来把"后 5 个"填满)
    writeFile(game + QStringLiteral("/versions/1.6.4/1.6.4.json"),
              QByteArray("{\"id\":\"1.6.4\",\"type\":\"release\","
                         "\"mainClass\":\"net.minecraft.client.main.Main\","
                         "\"clientVersion\":\"1.6.4\"}"));
    // 残缺:缺版本文件 + 缺游戏本体 + 缺依赖库(版本文件要从清单里取回来)
    const QByteArray legacyJson =
        versionJson(QStringLiteral("1.8.9"), base + QStringLiteral("/f/1.8.9.jar"), legacyJarBody,
                    QStringLiteral("org/example/legacy/1.0/legacy-1.0.jar"),
                    base + QStringLiteral("/f/legacy.jar"), legacyLibBody);
    server.routes.insert(QStringLiteral("/m/1.8.9.json"), legacyJson);

    // 版本清单(本地文件;修复要取版本文件时读它)
    QString manifest = QStringLiteral("{\"latest\":{\"release\":\"1.21.4\","
                                      "\"snapshot\":\"25w14a\"},\"versions\":["
                                      "{\"id\":\"1.8.9\",\"type\":\"release\",\"url\":\"%1/m/1.8.9.json\","
                                      "\"sha1\":\"\",\"size\":0,"
                                      "\"releaseTime\":\"2016-01-01T00:00:00+00:00\","
                                      "\"time\":\"2016-01-01T00:00:00+00:00\"}]}")
                           .arg(base);
    const QString manifestPath = work + QStringLiteral("/manifest.json");
    writeFile(manifestPath, manifest.toUtf8());

    const QString settingsPath = work + QStringLiteral("/sxcl.ini");
    QFile settings(settingsPath);
    if (settings.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        settings.write(QStringLiteral("game.default_dir=%1\ngame.selected_version=1.21.4\n"
                                      "download.source=mojang\n")
                           .arg(QDir::fromNativeSeparators(game))
                           .toUtf8());
        settings.close();
    }
    qputenv("SXCL_UI_SETTINGS", settingsPath.toUtf8());
    qputenv("SXCL_UI_GAME_DIR", QDir::fromNativeSeparators(game).toUtf8());
    qputenv("SXCL_UI_MANIFEST", manifestPath.toUtf8());
    qputenv("SXCL_UI_DATA_DIR", (work + QStringLiteral("/data")).toUtf8());
    qputenv("SXCL_UI_LAUNCH_DRY_RUN", "1");
    qputenv("SXCL_UI_DOWNLOAD_SOURCE", "mojang");

    // ═══════════════ 一、版本号解析 / 比较(纯函数)═══════════════
    section(QStringLiteral("版本号解析与比较"));
    check(compareVersionText(QStringLiteral("1.21.4"), QStringLiteral("1.20.1")) > 0,
          QStringLiteral("1.21.4 比 1.20.1 新"));
    check(compareVersionText(QStringLiteral("1.9"), QStringLiteral("1.12.2")) < 0,
          QStringLiteral("1.9 比 1.12.2 旧(逐段比数字,不是比字符串)"));
    check(compareVersionText(QStringLiteral("26.3"), QStringLiteral("1.21.4")) > 0,
          QStringLiteral("年份式 26.3 比 1.21.4 新"));
    check(compareVersionText(QStringLiteral("26.3"), QStringLiteral("25w14a")) > 0,
          QStringLiteral("26.3 比快照 25w14a 新"));
    check(compareVersionText(QStringLiteral("25w14a"), QStringLiteral("1.21.4")) > 0,
          QStringLiteral("快照 25w14a 比 1.21.4 新"));
    check(compareVersionText(QStringLiteral("1.21.4"), QStringLiteral("1.21.4-rc1")) > 0,
          QStringLiteral("同名正式版比候选版新"));
    check(compareVersionText(QStringLiteral("1.21.4-rc1"), QStringLiteral("1.21.4-pre2")) > 0,
          QStringLiteral("候选版比预览版新"));
    check(compareVersionText(QStringLiteral("1.21.4-pre1"), QStringLiteral("1.21.3")) > 0,
          QStringLiteral("1.21.4 的预览版仍比 1.21.3 新"));
    check(compareVersionText(QStringLiteral("1.7.10"), QStringLiteral("1.7.2")) > 0,
          QStringLiteral("1.7.10 比 1.7.2 新"));
    check(compareVersionText(QStringLiteral("b1.7.3"), QStringLiteral("1.0")) < 0,
          QStringLiteral("Beta 比 1.0 旧"));
    check(compareVersionText(QStringLiteral("c0.30"), QStringLiteral("b1.7.3")) < 0,
          QStringLiteral("Classic 比 Beta 旧"));
    check(parseMcVersion(QStringLiteral("26.3")).known, QStringLiteral("26.3 认得出是版本号"));
    check(!parseMcVersion(QStringLiteral("114514")).known,
          QStringLiteral("114514 不算版本号(没有小数点)"));
    check(!parseMcVersion(QStringLiteral("forge-1.0")).known,
          QStringLiteral("forge-1.0 不算版本号(前面没有数字段)"));

    // ═══════════════ 二、版本选择页:排序 / 分组 / 缺什么 ═══════════════
    section(QStringLiteral("版本选择页:排序与分组"));
    auto *host = new QWidget;
    host->resize(1100, 900);
    QWidget *page = createVersionsSelectPage(host);
    page->resize(1100, 900);
    host->show();

    const QStringList allIds = healthy
        + QStringList{QStringLiteral("1.18.2"), QStringLiteral("1.16.5"), QStringLiteral("1.14.4"),
                      QStringLiteral("1.8.9"), QStringLiteral("1.6.4")};
    const bool scanned = waitFor([&] { return visibleRows(page, allIds).size() >= 14; }, 30000);
    check(scanned, QStringLiteral("扫描出 14 个版本"),
          QStringLiteral("实际 %1 个").arg(visibleRows(page, allIds).size()));

    const QVector<RowView> rows = visibleRows(page, allIds);
    const QStringList ids = rowIds(rows);
    std::printf("  行序(上到下):%s\n", ids.join(QStringLiteral(" | ")).toUtf8().constData());

    const QStringList wantHealthy = expectedHealthy();
    check(ids.mid(0, 5) == QStringList({QStringLiteral("26.3"), QStringLiteral("25w14a"),
                                        QStringLiteral("1.21.4"), QStringLiteral("1.21.1"),
                                        QStringLiteral("1.20.6")}),
          QStringLiteral("前 5 个是版本号最新的 5 个"),
          QStringLiteral("实际 %1").arg(ids.mid(0, 5).join(QLatin1Char(','))));
    check(ids.mid(ids.size() - 5) == QStringList({QStringLiteral("1.18.2"), QStringLiteral("1.16.5"),
                                                  QStringLiteral("1.14.4"), QStringLiteral("1.8.9"),
                                                  QStringLiteral("1.6.4")}),
          QStringLiteral("后 5 个是残缺版那一组(组内也从新到旧)"),
          QStringLiteral("实际 %1").arg(ids.mid(ids.size() - 5).join(QLatin1Char(','))));
    check(ids.mid(0, wantHealthy.size()) == wantHealthy,
          QStringLiteral("正常那一组 9 行全部按版本号从新到旧"),
          QStringLiteral("实际 %1").arg(ids.mid(0, wantHealthy.size()).join(QLatin1Char(','))));

    // 分组标题:一句话,位置在正常组之后、残缺组之前
    auto *groupTitle = page->findChild<QLabel *>(QStringLiteral("sxclVersionGroupTitle"));
    check(groupTitle != nullptr, QStringLiteral("有分组标题控件(#sxclVersionGroupTitle)"));
    if (groupTitle != nullptr) {
        const int titleY = groupTitle->mapTo(page, QPoint(0, 0)).y();
        const int lastHealthyY = rows.at(wantHealthy.size() - 1).y;
        const int firstBrokenY = rows.at(wantHealthy.size()).y;
        std::printf("  分组标题:\"%s\" y=%d(最后一正常行 y=%d,第一残缺行 y=%d)\n",
                    groupTitle->text().toUtf8().constData(), titleY, lastHealthyY, firstBrokenY);
        check(titleY > lastHealthyY && titleY < firstBrokenY,
              QStringLiteral("分组标题夹在两组之间(残缺版在最后)"));
        check(groupTitle->text().contains(QStringLiteral("5")),
              QStringLiteral("分组标题里写着残缺版的个数"));
        check(!groupTitle->text().contains(QStringLiteral("JSON")) &&
                  !groupTitle->text().contains(QStringLiteral("jar")) &&
                  !groupTitle->text().contains(QStringLiteral("库")),
              QStringLiteral("分组标题不写实现细节"));
    }

    // 每一行:状态图标 / 缺什么 / 动作词
    for (const RowView &row : rows) {
        const bool broken = !wantHealthy.contains(row.id);
        if (broken) {
            check(row.state == QLatin1String("warn"), QStringLiteral("%1 是残缺版(警告图标)").arg(row.id));
            check(row.action == QStringLiteral("修复"), QStringLiteral("%1 的动作是「修复」").arg(row.id),
                  QStringLiteral("实际 \"%1\"").arg(row.action));
            check(!row.note.isEmpty(), QStringLiteral("%1 的行内写着缺什么").arg(row.id),
                  row.note);
        } else {
            check(row.state == QLatin1String("grass"), QStringLiteral("%1 能启动(草方块)").arg(row.id));
            check(row.action.isEmpty(), QStringLiteral("%1 没有动作词(它不缺东西)").arg(row.id));
        }
    }
    const RowView *noJson = nullptr;
    const RowView *noJar = nullptr;
    const RowView *noParent = nullptr;
    for (const RowView &row : rows) {
        if (row.id == QLatin1String("1.8.9"))
            noJson = &row;
        if (row.id == QLatin1String("1.16.5"))
            noJar = &row;
        if (row.id == QLatin1String("1.14.4"))
            noParent = &row;
    }
    check(noJson != nullptr && noJson->note.contains(QStringLiteral("缺版本文件")),
          QStringLiteral("1.8.9 那一行写着「缺版本文件」"),
          noJson != nullptr ? noJson->note : QString());
    check(noJar != nullptr && noJar->note.contains(QStringLiteral("缺游戏本体文件")),
          QStringLiteral("1.16.5 那一行写着「缺游戏本体文件」"),
          noJar != nullptr ? noJar->note : QString());
    check(noParent != nullptr && noParent->note.contains(QStringLiteral("缺前置版本 1.13.2")),
          QStringLiteral("1.14.4 那一行写着缺哪个前置版本"),
          noParent != nullptr ? noParent->note : QString());
    const RowView *repairTarget = nullptr;
    for (const RowView &row : rows) {
        if (row.id == QLatin1String("1.18.2"))
            repairTarget = &row;
    }
    check(repairTarget != nullptr &&
              repairTarget->note.contains(QStringLiteral("缺游戏本体文件")) &&
              repairTarget->note.contains(QStringLiteral("缺依赖库 1 个")),
          QStringLiteral("1.18.2 那一行把缺的东西数出来了(本体 + 1 个依赖库)"),
          repairTarget != nullptr ? repairTarget->note : QString());

    // ═══════════════ 三、「修复」真的把文件补上 ═══════════════
    section(QStringLiteral("「修复」:点下去真的补文件"));
    const QString jarDest = game + QStringLiteral("/versions/1.18.2/1.18.2.jar");
    const QString libDest =
        game + QStringLiteral("/libraries/org/example/demo/1.0/demo-1.0.jar");
    check(!QFileInfo::exists(jarDest) && !QFileInfo::exists(libDest),
          QStringLiteral("修复前:本体与依赖库都不在磁盘上"));

    QWidget *repairCard = cardOf(page, QStringLiteral("1.18.2"));
    auto *actionLabel = repairCard != nullptr
                            ? repairCard->findChild<QLabel *>(QStringLiteral("sxclVersionRowAction"))
                            : nullptr;
    check(actionLabel != nullptr && actionLabel->text() == QStringLiteral("修复"),
          QStringLiteral("残缺行上有一个能点的「修复」"));
    if (actionLabel != nullptr)
        clickWidget(actionLabel);

    const bool repaired = waitFor(
        [&] {
            return QFileInfo::exists(jarDest) && QFileInfo::exists(libDest) &&
                   cardOf(page, QStringLiteral("1.18.2")) != nullptr &&
                   cardOf(page, QStringLiteral("1.18.2"))
                           ->findChild<QWidget *>(QStringLiteral("sxclVersionStateIcon_grass")) !=
                       nullptr;
        },
        60000);
    check(QFileInfo::exists(jarDest), QStringLiteral("修复后:游戏本体文件真的落到了磁盘上"),
          QFileInfo(jarDest).size() > 0 ? QStringLiteral("%1 字节").arg(QFileInfo(jarDest).size())
                                        : QString());
    check(QFileInfo::exists(libDest), QStringLiteral("修复后:缺的那个依赖库也补上了"));
    check(repaired, QStringLiteral("修复后那一行自己变成了能启动(草方块)"));

    const QVector<RowView> rowsAfter = visibleRows(page, allIds);
    const QStringList idsAfter = rowIds(rowsAfter);
    std::printf("  修复后的行序:%s\n", idsAfter.join(QStringLiteral(" | ")).toUtf8().constData());
    // 行数不变(它只是从"有问题"那一组挪回了上面),但位置必须按版本号插对:
    // 1.18.2 排在 1.19.4 与 1.12.2 之间,残缺那组少了一个。
    check(idsAfter.size() == ids.size() &&
              idsAfter == (QStringList({QStringLiteral("26.3"), QStringLiteral("25w14a"),
                                        QStringLiteral("1.21.4"), QStringLiteral("1.21.1"),
                                        QStringLiteral("1.20.6"), QStringLiteral("1.20.1"),
                                        QStringLiteral("1.19.4"), QStringLiteral("1.18.2"),
                                        QStringLiteral("1.12.2"), QStringLiteral("1.7.10"),
                                        QStringLiteral("1.16.5"), QStringLiteral("1.14.4"),
                                        QStringLiteral("1.8.9"), QStringLiteral("1.6.4")})),
          QStringLiteral("修好的版本回到正常那一组,并且按版本号插到该在的位置"),
          QStringLiteral("实际 %1").arg(idsAfter.join(QLatin1Char(','))));

    // 缺版本文件那一行:修复要从清单里把版本文件取回来
    QWidget *jsonCard = cardOf(page, QStringLiteral("1.8.9"));
    auto *jsonAction = jsonCard != nullptr
                           ? jsonCard->findChild<QLabel *>(QStringLiteral("sxclVersionRowAction"))
                           : nullptr;
    check(jsonAction != nullptr && jsonAction->text() == QStringLiteral("修复"),
          QStringLiteral("缺版本文件那一行也有「修复」"));
    if (jsonAction != nullptr)
        clickWidget(jsonAction);
    const QString jsonDest = game + QStringLiteral("/versions/1.8.9/1.8.9.json");
    const QString legacyJarDest = game + QStringLiteral("/versions/1.8.9/1.8.9.jar");
    const bool jsonRepaired = waitFor(
        [&] {
            return QFileInfo::exists(jsonDest) && QFileInfo(legacyJarDest).size() == legacyJarBody.size() &&
                   cardOf(page, QStringLiteral("1.8.9")) != nullptr &&
                   cardOf(page, QStringLiteral("1.8.9"))
                           ->findChild<QWidget *>(QStringLiteral("sxclVersionStateIcon_grass")) != nullptr;
        },
        60000);
    check(jsonRepaired, QStringLiteral("缺版本文件的那一行:版本文件从清单取回、文件补齐、状态变正常"));
    check(QFileInfo::exists(jsonDest), QStringLiteral("取回来的版本文件真的写在 versions/1.8.9/ 下"));

    const QStringList idsFinal = rowIds(visibleRows(page, allIds));
    std::printf("  两行都修好之后的顺序:%s\n", idsFinal.join(QStringLiteral(" | ")).toUtf8().constData());
    check(idsFinal ==
              QStringList({QStringLiteral("26.3"), QStringLiteral("25w14a"),
                           QStringLiteral("1.21.4"), QStringLiteral("1.21.1"),
                           QStringLiteral("1.20.6"), QStringLiteral("1.20.1"),
                           QStringLiteral("1.19.4"), QStringLiteral("1.18.2"),
                           QStringLiteral("1.12.2"), QStringLiteral("1.8.9"),
                           QStringLiteral("1.7.10"), QStringLiteral("1.16.5"),
                           QStringLiteral("1.14.4"), QStringLiteral("1.6.4")}),
          QStringLiteral("两行都修好:正常组 11 行按版本号排,残缺组只剩 3 行"),
          QStringLiteral("实际 %1").arg(idsFinal.join(QLatin1Char(','))));

    // ═══════════════ 四、启动页:只有"启动什么 + 加载动画 + 等待游戏 + 进度条 + 小贴士" ═══════════════
    section(QStringLiteral("启动页:技术信息一个都不在"));
    auto *launchHost = new QWidget;
    launchHost->resize(1100, 900);
    VersionRef ref;
    ref.id = QStringLiteral("9.9.9-not-installed"); // 没装 -> 页面走"未安装"态(不起进程、不联网)
    QWidget *launch = createLaunchPage(ref, launchHost);
    launch->resize(1100, 900);
    launchHost->show();
    waitFor([&] { return launch->findChild<QLabel *>(QStringLiteral("sxclLaunchPhase")) != nullptr; }, 5000);
    QCoreApplication::processEvents();

    auto *title = launch->findChild<QLabel *>(QStringLiteral("sxclLaunchTitle"));
    check(title != nullptr && title->text().contains(QStringLiteral("启动 9.9.9-not-installed")),
          QStringLiteral("最上面写着「启动什么」(启动 + 版本号)"),
          title != nullptr ? title->text() : QString());
    auto *spinner = launch->findChild<QWidget *>(QStringLiteral("sxclLaunchSpinner"));
    check(spinner != nullptr && spinner->isVisible(), QStringLiteral("下面有加载动画(#sxclLaunchSpinner)"));
    auto *progress = launch->findChild<QProgressBar *>(QStringLiteral("sxclLaunchProgress"));
    check(progress != nullptr && progress->isVisible(), QStringLiteral("有进度条(#sxclLaunchProgress)"));
    auto *phase = launch->findChild<QLabel *>(QStringLiteral("sxclLaunchPhase"));
    check(phase != nullptr && !phase->text().isEmpty(), QStringLiteral("有一句状态(人话)"),
          phase != nullptr ? phase->text() : QString());
    auto *tip = launch->findChild<QLabel *>(QStringLiteral("sxclLaunchTip"));
    check(tip != nullptr && !tip->text().isEmpty(), QStringLiteral("有小贴士(#sxclLaunchTip)"),
          tip != nullptr ? tip->text() : QString());

    const QString dump = dumpTree(launch);
    check(!dump.contains(QStringLiteral("QPlainTextEdit")),
          QStringLiteral("dump 里没有命令行/游戏输出那种文本框(QPlainTextEdit)"));
    check(!dump.contains(QStringLiteral("最终命令行")) && !dump.contains(QStringLiteral("游戏输出")) &&
              !dump.contains(QStringLiteral("Java:")),
          QStringLiteral("dump 里没有「最终命令行 / 游戏输出 / Java:」这些字样"));
    const QStringList banned{QStringLiteral("classpath"), QStringLiteral("accessToken"),
                             QStringLiteral("natives"),   QStringLiteral("版本隔离"),
                             QStringLiteral("SXCL_"),     QStringLiteral("http://"),
                             QStringLiteral("https://"),  QStringLiteral(".json"),
                             QStringLiteral(".jar"),      QStringLiteral("JSON"),
                             QStringLiteral("SHA-1"),     QStringLiteral("PID")};
    QStringList hits;
    for (const QString &word : banned) {
        if (dump.contains(word, Qt::CaseInsensitive))
            hits << word;
    }
    check(hits.isEmpty(), QStringLiteral("dump 里没有代码/网址/术语"), hits.join(QLatin1Char(',')));
    std::printf("  ---- 启动页 dump ----\n%s\n  ---- dump 结束 ----\n",
                dump.toUtf8().constData());

    section(QStringLiteral("小贴士:表干净,而且会换"));
    const QStringList tips = launchTips();
    check(tips.size() >= 8, QStringLiteral("贴士表里至少 8 条"),
          QStringLiteral("实际 %1 条").arg(tips.size()));
    QStringList tipHits;
    for (const QString &one : tips) {
        for (const QString &word : banned) {
            if (one.contains(word, Qt::CaseInsensitive))
                tipHits << one.left(12) + QLatin1Char(':') + word;
        }
        for (const QChar &ch : one) {
            if (ch.unicode() > 0x1F000)
                tipHits << QStringLiteral("emoji?");
        }
    }
    check(tipHits.isEmpty(), QStringLiteral("每一条贴士都没有代码/网址/术语,也没有 emoji"),
          tipHits.join(QLatin1Char(',')));
    const QString tipBefore = tip != nullptr ? tip->text() : QString();
    const QString dumpBefore = dumpTree(launch);
    auto *tipTimer = launch->findChild<QTimer *>(QStringLiteral("sxclLaunchTipTimer"));
    check(tipTimer != nullptr, QStringLiteral("贴士有轮播定时器(#sxclLaunchTipTimer)"));
    if (tipTimer != nullptr) {
        QMetaObject::invokeMethod(tipTimer, "timeout", Qt::DirectConnection);
        QCoreApplication::processEvents();
    }
    const QString tipAfter = tip != nullptr ? tip->text() : QString();
    const QString dumpAfter = dumpTree(launch);
    check(!tipAfter.isEmpty() && tipAfter != tipBefore,
          QStringLiteral("换了一条贴士(两次读到的不一样)"),
          QStringLiteral("%1 -> %2").arg(tipBefore, tipAfter));
    check(dumpAfter != dumpBefore, QStringLiteral("两次 dump 不一样"));

    section(QStringLiteral("状态文案:保留「等待游戏」,不出现技术词"));
    check(launchPhaseText(3).contains(QStringLiteral("等待游戏")),
          QStringLiteral("第 4 段状态就是「等待游戏」"), launchPhaseText(3));
    QStringList statusHits;
    for (int i = 0; i < 5; ++i) {
        const QString text = launchPhaseText(i);
        if (text.isEmpty())
            statusHits << QStringLiteral("空状态 %1").arg(i);
        for (const QString &word : QStringList{QStringLiteral("Java"), QStringLiteral("命令行"),
                                               QStringLiteral("构建"), QStringLiteral("jar"),
                                               QStringLiteral("JSON")}) {
            if (text.contains(word, Qt::CaseInsensitive))
                statusHits << text + QLatin1Char(':') + word;
        }
    }
    check(statusHits.isEmpty(), QStringLiteral("每一句状态都是人话(没有 Java/命令行/构建)"),
          statusHits.join(QLatin1Char(',')));

    delete host;
    delete launchHost;

    std::printf("\n==== 断言 %d 条,失败 %d 条 ====\n", g_checks, g_fail);
    std::printf("夹具目录:%s\n", work.toUtf8().constData());
    return g_fail == 0 ? 0 : 1;
}
