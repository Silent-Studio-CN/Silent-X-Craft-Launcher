/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* 动画基座的**真机验收探针**(docs/27 §13.1 第 6 条:看得见、验得了)。
 *
 * 为什么是一个探针,而不是往外壳里加几行:
 *   M1 骨架里子栏那 4 行假版本已经被主页代理删掉了(shell 里不该有"看起来像真数据"的东西),
 *   而错峰淡入这条能力要在**真外壳**里看得见才算数。探针不改任何人的文件:
 *   它在运行时找到真实的左侧子栏(#sxcl2SubRail),往里塞 4 行 MotionRow,
 *   用 Anim::stagger(rows, ms, step) 让它们错峰冒出来,在 30% / 100% 两个进度各截一张图,然后退出。
 *
 * 什么时候会动:**只有 SXCL_UI2_ANIM_DEMO=1**。其它时候这个文件一行都不执行(产品路径零影响)。
 *   SXCL_UI2_ANIM_MS       每行时长(默认 1200ms;截图脚本要放慢才好抓)
 *   SXCL_UI2_ANIM_STEP     错峰间隔(默认 400ms)
 *   SXCL_UI2_ANIM_SHOT_DIR 两张图的落点(默认当前目录)
 *   SXCL_UI2_ANIM_TRACE    =1 时 anim.cpp 每帧还会打一行进度与各轨道值
 *
 * 它编译进 sxcl-ui 可执行文件(不是静态库:静态库里没人引用的目标文件会被链接器丢掉),
 * 注册用 Q_COREAPP_STARTUP_FUNCTION,所以外壳自己的代码不需要知道它存在。
 */

#include "anim.h"
#include "motion_row.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QPalette>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

namespace sxcl::ui2 {
/** 探针入口(实现见文件尾;默认什么都不做)。 */
void animProbeStart();
} // namespace sxcl::ui2

static void sxclUi2AnimProbe();
Q_COREAPP_STARTUP_FUNCTION(sxclUi2AnimProbe)

static void sxclUi2AnimProbe() {
    sxcl::ui2::animProbeStart();
}

namespace sxcl::ui2 {
namespace {

struct Probe {
    QTimer poll;
    QWidget *shell = nullptr;
    QWidget *rail = nullptr;
    QVector<MotionRow *> rows;
    Anim anim;
    QColor bg{30, 30, 35};
    QColor ink{240, 240, 245};
    QString shotDir;
    int ms = 1200;
    int step = 400;
    int tries = 0;
    bool started = false;
};

Probe &probe() {
    static Probe p;
    return p;
}

void logLine(const QString &text) {
    std::fprintf(stderr, "[ui2-anim-demo] %s\n", text.toUtf8().constData());
}

QWidget *findChildByName(QWidget *root, const QString &name) {
    if (root == nullptr)
        return nullptr;
    if (root->objectName() == name)
        return root;
    const QList<QWidget *> kids = root->findChildren<QWidget *>();
    for (QWidget *w : kids) {
        if (w->objectName() == name)
            return w;
    }
    return nullptr;
}

QString colorHex(const QColor &c) {
    return QStringLiteral("%1,%2,%3").arg(c.red()).arg(c.green()).arg(c.blue());
}

QString revealList() {
    QString out;
    for (int i = 0; i < probe().rows.size(); ++i)
        out += QStringLiteral("%1%2").arg(i == 0 ? "" : ",").arg(probe().rows.at(i)->reveal(), 0, 'f', 3);
    return out;
}

void takeShot(const char *tag) {
    Probe &p = probe();
    const QString path = p.shotDir + QStringLiteral("/shot_") + QString::fromLatin1(tag) + QStringLiteral(".png");
    const bool ok = p.shell->grab().save(path);
    logLine(QStringLiteral("SHOT tag=%1 ok=%2 p=%3 reveal=%4 bg=%5 ink=%6 dpr=%7 file=%8")
                .arg(QString::fromLatin1(tag))
                .arg(ok ? 1 : 0)
                .arg(p.anim.progress(), 0, 'f', 3)
                .arg(revealList())
                .arg(colorHex(p.bg))
                .arg(colorHex(p.ink))
                .arg(p.shell->devicePixelRatioF(), 0, 'f', 2)
                .arg(path));
}

void startDemo(QWidget *shell, QWidget *rail) {
    Probe &p = probe();
    p.started = true;
    p.shell = shell;
    p.rail = rail;
    p.poll.stop();

    /* 子栏默认收起(48):演示要看得见,先按产品语义把它撑开。 */
    rail->setFixedWidth(240);
    if (QWidget *title = findChildByName(rail, "sxcl2SubTitle"))
        title->setVisible(true);

    /* 颜色从**真界面**量(不写死主题):撑开之后取子栏右下角那块底色,墨色按底色明暗二选一。 */
    const QImage probeShot = rail->grab().toImage();
    if (!probeShot.isNull()) {
        const QColor c = probeShot.pixelColor(probeShot.width() - 6, probeShot.height() - 6);
        if (c.isValid())
            p.bg = c;
    }
    const bool dark = (p.bg.red() + p.bg.green() + p.bg.blue()) / 3 < 140;
    p.ink = dark ? QColor(0xd8, 0xd8, 0xe0) : QColor(0x30, 0x30, 0x38);

    auto *layout = qobject_cast<QVBoxLayout *>(rail->layout());
    const char *names[] = {"1.20.1-Forge", "1.20.1-Quilt", "1.12.1", "26.3"};
    QVector<QObject *> targets;
    const int insertAt = (layout != nullptr && layout->count() > 0) ? 1 : 0;
    for (int i = 0; i < 4; ++i) {
        auto *row = new MotionRow(rail);
        row->setObjectName(QStringLiteral("sxcl2SubRow")); /* 与验收脚本约定的行名 */
        row->setText(QString::fromUtf8(names[i]));
        QPalette pal = row->palette();
        pal.setColor(QPalette::Window, p.bg);
        pal.setColor(QPalette::WindowText, p.ink);
        row->setPalette(pal);
        row->setLiftPx(8);
        row->setReveal(0.0); /* 从"还没出现"开始,才看得见错峰 */
        if (layout != nullptr)
            layout->insertWidget(insertAt + i, row);
        else
            row->setParent(rail);
        p.rows.append(row);
        targets.append(row);
    }
    shell->update();

    /* 只给"屏内可见"的行建轨道(§10.2.1 性能纪律):这台子栏就是视口。 */
    const QVector<QObject *> visible = Anim::rowsInView(targets, rail);
    p.anim = Anim::stagger(visible, p.ms, p.step);
    p.anim.start();
    const int total = p.anim.durationMs();
    logLine(QStringLiteral("start rows=%1 injected=%2 ms=%3 step=%4 total=%5 level=%6 heartbeats=%7 timelines=%8 "
                           "shell=%9x%10 bg=%11 ink=%12")
                .arg(visible.size())
                .arg(targets.size())
                .arg(p.ms)
                .arg(p.step)
                .arg(total)
                .arg(QString::fromLatin1(Motion::levelName(Motion::level())))
                .arg(Motion::heartbeatCount())
                .arg(Motion::timelinesAlive())
                .arg(shell->width())
                .arg(shell->height())
                .arg(colorHex(p.bg))
                .arg(colorHex(p.ink)));
    for (int i = 0; i < p.rows.size(); ++i) {
        const QPoint at = p.rows.at(i)->mapTo(shell, QPoint(0, 0));
        logLine(QStringLiteral("ROW i=%1 x=%2 y=%3 w=%4 h=%5 text=%6")
                    .arg(i)
                    .arg(at.x())
                    .arg(at.y())
                    .arg(p.rows.at(i)->width())
                    .arg(p.rows.at(i)->height())
                    .arg(p.rows.at(i)->text()));
    }

    /* 30% 与 100% 两个进度各截一张(时间线走真钟,截的就是外壳自己 render 出来的像素)。 */
    QTimer::singleShot(static_cast<int>(total * 0.30), qApp, [] { takeShot("p30"); });
    QTimer::singleShot(total + 150, qApp, [] {
        takeShot("p100");
        logLine(QStringLiteral("done heartbeats=%1 timelines=%2 frames=%3")
                    .arg(Motion::heartbeatCount())
                    .arg(Motion::timelinesAlive())
                    .arg(Motion::frameCount()));
        QCoreApplication::quit();
    });
}

void pollShell() {
    Probe &p = probe();
    if (p.started)
        return;
    if (++p.tries > 150) { /* 6s 还没等到外壳:如实报错,别把脚本挂死 */
        logLine(QStringLiteral("FAIL: 6s 内没找到新外壳窗口(#sxcl2Root)"));
        p.poll.stop();
        QCoreApplication::exit(3);
        return;
    }
    QWidget *shell = nullptr;
    const QList<QWidget *> tops = QApplication::topLevelWidgets();
    for (QWidget *w : tops) {
        if (w->objectName() == QLatin1String("sxcl2Root") && w->isVisible()) {
            shell = w;
            break;
        }
    }
    if (shell == nullptr)
        return;
    QWidget *rail = findChildByName(shell, QLatin1String("sxcl2SubRail"));
    if (rail == nullptr)
        return;
    startDemo(shell, rail);
}

} // namespace

namespace {

/** 真正武装探针:读环境变量 + 起轮询(在事件循环里跑,不跟启动例程抢时序)。 */
void armProbe() {
    Probe &p = probe();
    const int ms = qEnvironmentVariableIntValue("SXCL_UI2_ANIM_MS");
    const int step = qEnvironmentVariableIntValue("SXCL_UI2_ANIM_STEP");
    if (ms > 0)
        p.ms = ms;
    if (step > 0)
        p.step = step;
    p.shotDir = qEnvironmentVariable("SXCL_UI2_ANIM_SHOT_DIR");
    if (p.shotDir.isEmpty())
        p.shotDir = QDir::currentPath();
    QDir().mkpath(p.shotDir);
    logLine(QStringLiteral("probe armed ms=%1 step=%2 dir=%3").arg(p.ms).arg(p.step).arg(p.shotDir));
    p.poll.setInterval(40);
    QObject::connect(&p.poll, &QTimer::timeout, [] { pollShell(); });
    p.poll.start();
}

} // namespace

void animProbeStart() {
    if (qEnvironmentVariableIntValue("SXCL_UI2_ANIM_DEMO") != 1)
        return; /* 平时一行都不做 */
    QCoreApplication *app = QCoreApplication::instance();
    if (app == nullptr) {
        std::fprintf(stderr, "[ui2-anim-demo] FAIL: 启动例程早于 QCoreApplication\n");
        return;
    }
    /* 投一条队列消息再武装:启动例程比事件分发器还早,定时器要等事件循环起来才可靠。 */
    QMetaObject::invokeMethod(app, [] { armProbe(); }, Qt::QueuedConnection);
}

} // namespace sxcl::ui2
