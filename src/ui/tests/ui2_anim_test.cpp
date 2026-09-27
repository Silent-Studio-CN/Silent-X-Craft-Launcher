/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 *
 * 新界面动画基座(docs/27 §10.2 / §10.2.1 / §13.1)的无头验收 —— 数字说话,不看截图:
 *   a) 时序:500ms 的动画在 0/250/500ms 采样,进度单调且在容差内(假钟给精确值 + 真钟给实测值);
 *   b) then/after 串行顺序:记每次改值的时间戳,断言第二段在第一段**结束之后**才开始;
 *   c) 错峰:直接算延迟表,每行起始差 = 25ms(容差 ±5ms);再用假钟扫一遍,实测首次出现的时刻也是 25ms;
 *   d) ui.motion=off:终值立即就位、0 帧、没有时间线;并与系统无障碍取更严的那个(8 组组合逐个断言);
 *   e) 心跳:全局只创建 1 个 QTimer(可查计数),64 条时间线也还是 1 个,空闲时停表、复用不重建;
 *   f) 自绘属性:动画只写 reveal,MotionRow 的像素随 reveal 变(0 时一个墨点都没有),几何一动不动。
 *
 * 设置文件重定向到构建目录下的临时点(SXCL_UI_SETTINGS),绝不碰用户真实配置。
 */

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <cmath>
#include <cstdio>
#include <functional>

#include "ui2/anim.h"
#include "ui2/motion_row.h"

using namespace sxcl::ui2;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const QString &what) {
    if (ok) {
        ++g_pass;
        std::printf("  [ok] %s\n", what.toUtf8().constData());
    } else {
        ++g_fail;
        std::printf("  [!!] %s\n", what.toUtf8().constData());
    }
}

static QString num(double v, int digits = 4) {
    return QString::number(v, 'f', digits);
}

static void checkNear(double got, double want, double tol, const QString &what) {
    check(std::fabs(got - want) <= tol,
          QStringLiteral("%1(实测 %2,期望 %3 ±%4)").arg(what, num(got, 6), num(want, 6), num(tol, 6)));
}

/* ── 假钟:把"现在"换成我们自己的数,采样点精确可复现(不睡、不等) ── */
static qint64 g_fakeNow = 0;
static void useFakeClock() {
    Motion::setClockForTest([] { return g_fakeNow; });
}
static void useRealClock() {
    Motion::setClockForTest(std::function<qint64()>());
}
static void pumpAt(qint64 t) {
    g_fakeNow = t;
    Motion::pump();
}

static void waitMs(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/* 假钟下从 0 扫到 until,记录每个属性"第一次不等于初值"的时刻 */
static QVector<qint64> sweepFirstChange(Anim &anim, QObject &subject, const QVector<QByteArray> &props,
                                        qint64 until) {
    QVector<qint64> first(props.size(), -1);
    g_fakeNow = 0;
    anim.start();
    for (qint64 t = 0; t <= until; ++t) {
        pumpAt(t);
        for (int i = 0; i < props.size(); ++i) {
            if (first[i] < 0 && std::fabs(subject.property(props[i].constData()).toDouble()) > 1e-9)
                first[i] = t;
        }
    }
    return first;
}

static QString tmpDir() {
    return QDir::currentPath() + QStringLiteral("/_ui2_anim_tmp");
}

static void writeMotionSetting(const char *value) {
    const QString dir = tmpDir();
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/settings.conf");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::printf("  [!!] 写不了设置文件: %s\n", path.toUtf8().constData());
        return;
    }
    if (f.write(QByteArray("ui.motion=") + value + "\n") < 0)
        std::printf("  [!!] 设置文件写入失败\n");
    f.close();
    qputenv("SXCL_UI_SETTINGS", path.toUtf8());
}

/* 一行"有多少墨"(像素扫描的无头版:与 tools/ui2_anim.ps1 同一个判据)
 *   count = 亮度 > 100 的像素数(可见/不可见的粗判据)
 *   sum   = Σ max(0, 亮度 - 底亮度):与文字 alpha 成正比 —— 所以 sum(半程)/sum(全程) ≈ 进度,
 *           这正是"第 N 行已可见到什么程度"的量化依据(不是只看图)。
 *   centroidY = 墨点重心:reveal 越低文字越靠下(§4 的 8px 上移)。 */
struct Ink {
    int count = 0;
    double sum = 0.0;
    double centroidY = -1.0;
};

static Ink scanInk(MotionRow &row) {
    const qreal dpr = row.devicePixelRatioF() > 0 ? row.devicePixelRatioF() : 1.0;
    QImage canvas(row.size() * dpr, QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(QColor(30, 30, 35)); /* 暗色令牌的底:与真机截图同一档 */
    QPainter p(&canvas);
    row.render(&p);
    p.end();
    Ink ink;
    double sumY = 0.0;
    /* 底亮度用同一个整数公式算出来(30,30,35 -> 31):底色像素贡献恰好是 0,
     * 墨量就正比于文字 alpha —— 半程的墨量除以满格的墨量 ≈ reveal。 */
    const int bgLum = (30 + 30 + 35) / 3;
    for (int y = 0; y < canvas.height(); ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const QColor c = canvas.pixelColor(x, y);
            const int lum = (c.red() + c.green() + c.blue()) / 3;
            if (lum > 100)
                ++ink.count;
            if (lum > bgLum) {
                ink.sum += lum - bgLum;
                sumY += (lum - bgLum) * y;
            }
        }
    }
    if (ink.sum > 0.0)
        ink.centroidY = sumY / ink.sum;
    return ink;
}

static MotionRow *makeRow(int width, const QString &text) {
    auto *row = new MotionRow;
    row->setText(text);
    row->resize(width, 34);
    /* 暗色主题那一档:文字浅、底深 —— 像素扫描的判据(墨点 = 亮度 > 100)就建立在这上面,
     * 与 tools/ui2_anim.ps1 对真机截图的判据一致。真机上这两个颜色来自主题 QSS。 */
    QPalette pal = row->palette();
    pal.setColor(QPalette::Window, QColor(30, 30, 35));
    pal.setColor(QPalette::WindowText, QColor(240, 240, 245));
    row->setPalette(pal);
    row->show();
    return row;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    QDir(tmpDir()).removeRecursively();
    QDir().mkpath(tmpDir());
    std::printf("== sxcl ui2 动画基座无头验收 ==\n");

    /* ── 曲线表:Ease::Out 这些语义名背后的形状 ── */
    {
        bool monotonic = true;
        for (int i = 0; i <= 200; ++i) {
            if (Ease::at(Ease::Out, (i + 1) / 200.0) < Ease::at(Ease::Out, i / 200.0))
                monotonic = false;
        }
        check(monotonic, QStringLiteral("Ease::Out 在 [0,1] 上单调不减"));
        checkNear(Ease::at(Ease::Out, 0.0), 0.0, 1e-12, QStringLiteral("Ease::Out(0)"));
        checkNear(Ease::at(Ease::Out, 1.0), 1.0, 1e-12, QStringLiteral("Ease::Out(1)"));
        for (int e = int(Ease::Out); e <= int(Ease::Glide); ++e) {
            const Ease::Name n = static_cast<Ease::Name>(e);
            const QString nm = QString::fromLatin1(Ease::name(n));
            checkNear(Ease::at(n, 0.0), 0.0, 1e-9, QStringLiteral("曲线 %1 起步在 0").arg(nm));
            checkNear(Ease::at(n, 1.0), 1.0, 1e-9, QStringLiteral("曲线 %1 收在 1").arg(nm));
        }
        const double outEarly = Ease::at(Ease::Out, 0.25);
        const double inEarly = Ease::at(Ease::In, 0.25);
        check(outEarly > inEarly,
              QStringLiteral("出场比入场起步快(0.25 处 %1 > %2)").arg(num(outEarly), num(inEarly)));
        const double fluentTail = Ease::at(Ease::FluentOut, 0.5);
        const double outTail = Ease::at(Ease::Out, 0.5);
        check(fluentTail > outTail,
              QStringLiteral("FluentOut 尾巴比 Out 长(0.5 处 %1 > %2)").arg(num(fluentTail), num(outTail)));
        double peak = 0.0;
        for (int i = 0; i <= 400; ++i)
            peak = std::fmax(peak, Ease::at(Ease::Back, i / 400.0));
        check(peak > 1.0 && peak <= 1.02,
              QStringLiteral("Back 过冲峰值 %1 ∈ (1, 1.02](§4 不许弹跳)").arg(num(peak)));
        const double slow = Ease::glide(0.0, 300.0, 400, 0.25);
        const double fast = Ease::glide(3000.0, 300.0, 400, 0.25);
        check(fast > slow, QStringLiteral("初速越大起步越陡(0.25 处 %1 > %2)").arg(num(fast), num(slow)));
        checkNear(Ease::glide(1200.0, 300.0, 400, 1.0), 1.0, 1e-12, QStringLiteral("初速曲线收在 1"));
    }

    /* ── a) 时序:0/250/500 采样(假钟 = 精确可复现) ── */
    {
        useFakeClock();
        qputenv("SXCL_UI_MOTION", "standard");
        qputenv("SXCL_UI_MOTION_SYSTEM", "standard");
        Motion::refresh();
        QObject subject;
        subject.setProperty("v", 0.0);
        Anim anim = Anim::to(&subject, "v", 1.0, 500, Ease::Out);
        anim.start();
        double p[3] = {0, 0, 0};
        double v[3] = {0, 0, 0};
        const qint64 stamps[3] = {0, 250, 500};
        for (int i = 0; i < 3; ++i) {
            pumpAt(stamps[i]);
            p[i] = anim.progress();
            v[i] = subject.property("v").toDouble();
        }
        std::printf("  [..] 500ms 假钟采样 t=0/250/500 -> p=%s/%s/%s v=%s/%s/%s\n",
                    num(p[0], 3).toUtf8().constData(), num(p[1], 3).toUtf8().constData(),
                    num(p[2], 3).toUtf8().constData(), num(v[0], 3).toUtf8().constData(),
                    num(v[1], 3).toUtf8().constData(), num(v[2], 3).toUtf8().constData());
        check(p[0] <= p[1] && p[1] <= p[2], QStringLiteral("进度单调不减"));
        checkNear(p[0], 0.0, 1e-9, QStringLiteral("t=0 进度"));
        checkNear(p[1], 0.5, 1e-9, QStringLiteral("t=250 进度"));
        checkNear(p[2], 1.0, 1e-9, QStringLiteral("t=500 进度"));
        checkNear(v[0], 0.0, 1e-9, QStringLiteral("t=0 值 = 起点"));
        checkNear(v[1], Ease::at(Ease::Out, 0.5), 1e-9, QStringLiteral("t=250 值 = 曲线(0.5)"));
        checkNear(v[2], 1.0, 1e-12, QStringLiteral("t=500 值 = 终值"));
        check(anim.durationMs() == 500, QStringLiteral("总时长 500ms(实测 %1)").arg(anim.durationMs()));
    }

    /* ── a2) 同一条曲线走真钟:进度单调、在容差内,走完精确落到终值 ── */
    {
        useRealClock();
        QObject subject;
        subject.setProperty("v", 0.0);
        Anim anim = Anim::to(&subject, "v", 1.0, 500, Ease::Out);
        QElapsedTimer clock;
        clock.start();
        anim.start();
        double p[3] = {0, 0, 0};
        double v[3] = {0, 0, 0};
        const qint64 stamps[3] = {0, 250, 500};
        for (int i = 0; i < 3; ++i) {
            const qint64 wait = stamps[i] - clock.elapsed();
            if (wait > 0)
                waitMs(static_cast<int>(wait));
            p[i] = anim.progress();
            v[i] = subject.property("v").toDouble();
        }
        std::printf("  [..] 500ms 真钟采样 t=0/250/500 -> p=%s/%s/%s v=%s/%s/%s\n",
                    num(p[0], 3).toUtf8().constData(), num(p[1], 3).toUtf8().constData(),
                    num(p[2], 3).toUtf8().constData(), num(v[0], 3).toUtf8().constData(),
                    num(v[1], 3).toUtf8().constData(), num(v[2], 3).toUtf8().constData());
        check(p[0] <= p[1] && p[1] <= p[2], QStringLiteral("真钟:进度单调不减"));
        checkNear(p[0], 0.0, 0.02, QStringLiteral("真钟:t=0 进度"));
        checkNear(p[1], 0.5, 0.15, QStringLiteral("真钟:t=250 进度在容差内"));
        checkNear(p[2], 1.0, 1e-9, QStringLiteral("真钟:t=500 进度 = 1"));
        check(v[1] > 0.5 && v[1] < 1.0, QStringLiteral("真钟:t=250 值已经过半(%1)").arg(num(v[1])));
        waitMs(80);
        checkNear(subject.property("v").toDouble(), 1.0, 1e-12, QStringLiteral("真钟:走完精确落在终值"));
        check(Motion::timelinesAlive() == 0, QStringLiteral("真钟:走完自动下线"));
    }

    /* ── b) then / after 串行:记时间戳,第二段在第一段之后开始 ── */
    {
        useFakeClock();
        QObject subject;
        subject.setProperty("a", 0.0);
        subject.setProperty("b", 0.0);
        subject.setProperty("c", 0.0);
        Anim anim = Anim::to(&subject, "a", 1.0, 200)
                        .after(50)
                        .then(Anim::to(&subject, "b", 1.0, 100))
                        .then(Anim::to(&subject, "c", 1.0, 100));
        const QVector<qint64> first = sweepFirstChange(anim, subject, {"a", "b", "c"}, 500);
        const QVector<int> offs = anim.offsetsMs();
        std::printf("  [..] 偏移表 = %d, %d, %d ms;首次改值 = %lld, %lld, %lld ms\n", offs.value(0),
                    offs.value(1), offs.value(2), static_cast<long long>(first.value(0)),
                    static_cast<long long>(first.value(1)), static_cast<long long>(first.value(2)));
        check(offs == QVector<int>({0, 250, 350}),
              QStringLiteral("偏移表 = {0, 250, 350}(after(50) 把第二段推到 250)"));
        check(first.value(0) >= 0 && first.value(0) <= 2, QStringLiteral("第一段 0ms 就开始"));
        check(first.value(1) >= 250 && first.value(1) <= 252,
              QStringLiteral("第二段在第一段(200ms)+50ms 之后才开始"));
        check(first.value(2) >= 350 && first.value(2) <= 352, QStringLiteral("第三段在第二段之后才开始"));
        check(first.value(1) > 200 && first.value(2) > first.value(1) + 98,
              QStringLiteral("串行顺序正确(没有回调套回调)"));
        check(anim.stepCount() == 4 && anim.trackCount() == 3,
              QStringLiteral("段表 = 3 段动画 + 1 段等待(实测 %1 段 / %2 轨)")
                  .arg(anim.stepCount())
                  .arg(anim.trackCount()));
    }

    /* ── c) 错峰模板:延迟表直接算,再实测一遍 ── */
    {
        useFakeClock();
        QVector<QObject *> owned;
        QVector<QObject *> rows;
        for (int i = 0; i < 6; ++i) {
            auto *o = new QObject;
            o->setProperty("reveal", 0.0);
            owned.append(o);
            rows.append(o);
        }
        Anim anim = Anim::stagger(rows, 100, 25);
        const QVector<int> offs = anim.offsetsMs();
        int worst = 0;
        for (int i = 1; i < offs.size(); ++i)
            worst = qMax(worst, std::abs((offs.at(i) - offs.at(i - 1)) - 25));
        std::printf("  [..] stagger 偏移表 =");
        for (int v : offs)
            std::printf(" %d", v);
        std::printf(" ms(与 25 的最大偏差 %dms)\n", worst);
        check(offs.size() == 6, QStringLiteral("6 行 = 6 段轨道(实测 %1)").arg(offs.size()));
        check(worst <= 5, QStringLiteral("每行起始差 = 25ms ±5ms(最大偏差 %1ms)").arg(worst));
        check(anim.trackCount() == 6 && anim.durationMs() == 225,
              QStringLiteral("轨道 6 / 总时长 225ms(实测 %1 / %2)")
                  .arg(anim.trackCount())
                  .arg(anim.durationMs()));

        g_fakeNow = 0;
        Anim again = Anim::stagger(rows, 100, 25);
        again.start();
        check(Motion::timelinesAlive() == 1,
              QStringLiteral("6 行共用 1 条时间线(实测 %1)").arg(Motion::timelinesAlive()));
        check(Motion::heartbeatCount() == 1,
              QStringLiteral("仍然是那 1 个心跳(实测 %1)").arg(Motion::heartbeatCount()));
        QVector<qint64> born(6, -1);
        for (qint64 t = 0; t <= 260; ++t) {
            pumpAt(t);
            for (int i = 0; i < rows.size(); ++i) {
                if (born[i] < 0 && rows.at(i)->property("reveal").toDouble() > 1e-9)
                    born[i] = t;
            }
        }
        int bornWorst = 0;
        for (int i = 1; i < born.size(); ++i)
            bornWorst = qMax(bornWorst, std::abs(int(born.at(i) - born.at(i - 1)) - 25));
        std::printf("  [..] 实测首次出现 =");
        for (qint64 v : born)
            std::printf(" %lld", static_cast<long long>(v));
        std::printf(" ms(与 25 的最大偏差 %dms)\n", bornWorst);
        check(born.value(0) >= 0 && born.value(0) <= 2, QStringLiteral("第一行 0ms 就开始"));
        check(bornWorst <= 5, QStringLiteral("实测每行起始差 = 25ms ±5ms(最大偏差 %1ms)").arg(bornWorst));
        /* 可见行过滤:滚出屏幕 / 被隐藏的行连轨道都不建(§10.2.1 性能纪律的可查落点) */
        {
            QWidget viewport;
            viewport.resize(200, 70);
            QVector<QObject *> all;
            QVector<QWidget *> ownedWidgets;
            for (int i = 0; i < 6; ++i) {
                auto *w = new QWidget(&viewport);
                w->setGeometry(0, i * 30, 200, 30);
                ownedWidgets.append(w);
                all.append(w);
            }
            const int inView = int(Anim::rowsInView(all, &viewport).size());
            const int withMargin = int(Anim::rowsInView(all, &viewport, 30).size());
            std::printf("  [..] 可见行过滤 viewport 高 70 -> %d 行;margin=30 -> %d 行\n", inView, withMargin);
            check(inView == 3, QStringLiteral("视口内 3 行(y=0/30/60,实测 %1)").arg(inView));
            check(withMargin == 4, QStringLiteral("预热 margin=30 时 4 行(实测 %1)").arg(withMargin));
            check(Anim::rowsInView(all, nullptr).size() == 6, QStringLiteral("viewport 为空 = 不过滤"));
            ownedWidgets.at(1)->hide();
            check(Anim::rowsInView(all, &viewport).size() == 2, QStringLiteral("显式隐藏的行不算可见"));
            check(Anim::stagger(all, 100, 25).trackCount() == 5,
                  QStringLiteral("stagger 跳过隐藏的行(实测轨道 %1)").arg(Anim::stagger(all, 100, 25).trackCount()));
            for (QWidget *w : ownedWidgets)
                delete w;
        }
        for (QObject *o : owned)
            delete o;
    }

    /* ── d) 全局动效开关:ui.motion 与系统无障碍取更严的 ── */
    {
        struct Case {
            const char *setting;
            const char *system;
            Motion::Level want;
        };
        const Case cases[] = {
            {"follow", "standard", Motion::Standard},   {"follow", "reduced", Motion::Reduced},
            {"standard", "standard", Motion::Standard}, {"standard", "reduced", Motion::Reduced},
            {"reduced", "standard", Motion::Reduced},   {"reduced", "reduced", Motion::Reduced},
            {"off", "standard", Motion::Off},           {"off", "reduced", Motion::Off},
        };
        qunsetenv("SXCL_UI_MOTION");
        bool all = true;
        QString detail;
        for (const Case &c : cases) {
            writeMotionSetting(c.setting);
            qputenv("SXCL_UI_MOTION_SYSTEM", c.system);
            Motion::refresh();
            all = all && (Motion::level() == c.want);
            detail += QStringLiteral("%1+%2=%3 ")
                          .arg(QString::fromLatin1(c.setting), QString::fromLatin1(c.system),
                               QString::fromLatin1(Motion::levelName(Motion::level())));
        }
        std::printf("  [..] 8 组档位 = %s\n", detail.trimmed().toUtf8().constData());
        check(all, QStringLiteral("8 组(设置 × 系统)都取更严的那个"));
        qputenv("SXCL_UI_MOTION", "follow");
        qunsetenv("SXCL_UI_MOTION_SYSTEM");
        Motion::refresh();
        check(QString::fromLatin1(Motion::settingValue()) == QStringLiteral("follow"),
              QStringLiteral("设置键 %1 的原始值可读(%2)")
                  .arg(QString::fromLatin1(Motion::key()), QString::fromLatin1(Motion::settingValue())));

        /* off:值立即就位、0 帧、没有时间线 */
        useFakeClock();
        writeMotionSetting("off");
        qunsetenv("SXCL_UI_MOTION");
        qputenv("SXCL_UI_MOTION_SYSTEM", "standard");
        Motion::refresh();
        check(Motion::level() == Motion::Off, QStringLiteral("ui.motion=off -> 档位 off"));
        QObject subject;
        subject.setProperty("v", 0.0);
        MotionRow *row = makeRow(200, QStringLiteral("1.20.1-Forge"));
        row->setReveal(0.0);
        QApplication::processEvents();
        const qint64 frames0 = Motion::frameCount();
        Anim a1 = Anim::to(&subject, "v", 1.0, 500, Ease::Out);
        Anim a2 = Anim::stagger({row}, 500, 100);
        a1.start();
        a2.start();
        checkNear(subject.property("v").toDouble(), 1.0, 1e-12, QStringLiteral("off:普通属性立即 = 终值"));
        checkNear(row->reveal(), 1.0, 1e-12, QStringLiteral("off:自绘属性立即 = 终值"));
        check(Motion::frameCount() == frames0,
              QStringLiteral("off:0 帧(帧数 %1 -> %2)").arg(frames0).arg(Motion::frameCount()));
        check(Motion::timelinesAlive() == 0, QStringLiteral("off:没有时间线"));
        check(!a1.running(), QStringLiteral("off:时间线不算在跑"));
        checkNear(a1.progress(), 1.0, 1e-12, QStringLiteral("off:进度 = 1"));

        /* reduced:时长 ×0.35、错峰不排队 */
        writeMotionSetting("reduced");
        Motion::refresh();
        check(Motion::level() == Motion::Reduced, QStringLiteral("ui.motion=reduced -> 档位 reduced"));
        MotionRow *r1 = makeRow(180, QStringLiteral("1.20.1-Quilt"));
        MotionRow *r2 = makeRow(180, QStringLiteral("1.12.1"));
        MotionRow *r3 = makeRow(180, QStringLiteral("26.3"));
        QApplication::processEvents();
        Anim a3 = Anim::to(&subject, "v", 0.0, 1000, Ease::Out);
        Anim a4 = Anim::stagger({r1, r2, r3}, 1000, 200);
        check(a3.durationMs() == 350, QStringLiteral("reduced:1000ms -> 350ms(实测 %1)").arg(a3.durationMs()));
        const QVector<int> rOffs = a4.offsetsMs();
        std::printf("  [..] reduced 错峰偏移表 = %d, %d, %d ms\n", rOffs.value(0), rOffs.value(1), rOffs.value(2));
        check(rOffs == QVector<int>({0, 0, 0}), QStringLiteral("reduced:错峰延迟归零"));
        a3.start();
        a4.start();
        check(Motion::timelinesAlive() == 2, QStringLiteral("reduced:仍然有动画(不是关掉)"));
        a3.stop();
        a4.stop();
        delete row;
        delete r1;
        delete r2;
        delete r3;
    }

    /* ── e) 心跳:全局唯一;多条时间线不重建 ── */
    {
        useRealClock();
        qputenv("SXCL_UI_MOTION", "standard");
        qputenv("SXCL_UI_MOTION_SYSTEM", "standard");
        Motion::refresh();
        check(Motion::level() == Motion::Standard, QStringLiteral("档位 standard"));
        check(Motion::heartbeatCount() == 1,
              QStringLiteral("全局心跳定时器只有 1 个(实测 %1)").arg(Motion::heartbeatCount()));
        QVector<QObject *> owned;
        QVector<Anim> anims;
        for (int i = 0; i < 64; ++i) {
            auto *o = new QObject;
            o->setProperty("v", 0.0);
            owned.append(o);
            anims.append(Anim::to(o, "v", 1.0, 120, Ease::In));
        }
        const int hbBefore = Motion::heartbeatCount();
        const qint64 framesBefore = Motion::frameCount();
        for (Anim &a : anims)
            a.start();
        check(Motion::heartbeatCount() == hbBefore && Motion::heartbeatCount() == 1,
              QStringLiteral("64 条时间线之后心跳还是 1 个(实测 %1)").arg(Motion::heartbeatCount()));
        check(Motion::timelinesAlive() == 64,
              QStringLiteral("64 条时间线同时在跑(实测 %1)").arg(Motion::timelinesAlive()));
        check(Motion::heartbeatActive(), QStringLiteral("心跳在跑"));
        waitMs(300);
        check(Motion::frameCount() > framesBefore,
              QStringLiteral("心跳真的推了帧(%1 -> %2)").arg(framesBefore).arg(Motion::frameCount()));
        check(Motion::timelinesAlive() == 0,
              QStringLiteral("走完自动下线(实测 %1)").arg(Motion::timelinesAlive()));
        check(!Motion::heartbeatActive(), QStringLiteral("没动画时心跳停表(省电)"));
        Anim again = Anim::to(owned.value(0), "v", 0.0, 60, Ease::Out);
        again.start();
        check(Motion::heartbeatActive(), QStringLiteral("再来一条:复用同一个心跳重新起表"));
        check(Motion::heartbeatCount() == 1,
              QStringLiteral("仍然是 1 个心跳(实测 %1)").arg(Motion::heartbeatCount()));
        waitMs(120);
        for (QObject *o : owned)
            delete o;
    }

    /* ── f) 自绘属性:像素随 reveal 走,几何一动不动 ── */
    {
        MotionRow *row = makeRow(220, QStringLiteral("1.20.1-Forge"));
        QApplication::processEvents();
        const QRect geo0 = row->geometry();

        row->setReveal(0.0);
        const Ink ink0 = scanInk(*row);
        row->setReveal(1.0);
        const Ink ink100 = scanInk(*row);
        row->setReveal(0.5);
        const Ink ink50 = scanInk(*row);
        const double ratio = ink100.sum > 0 ? ink50.sum / ink100.sum : 0.0;
        std::printf("  [..] 像素扫描 reveal 0.0/0.5/1.0 -> 墨点 %d/%d/%d,墨量 %s/%s/%s(半程比 %s);"
                    "文字重心 y = %s -> %s\n",
                    ink0.count, ink50.count, ink100.count, num(ink0.sum, 0).toUtf8().constData(),
                    num(ink50.sum, 0).toUtf8().constData(), num(ink100.sum, 0).toUtf8().constData(),
                    num(ratio, 3).toUtf8().constData(), num(ink50.centroidY, 1).toUtf8().constData(),
                    num(ink100.centroidY, 1).toUtf8().constData());
        check(ink0.sum == 0.0 && ink0.count == 0,
              QStringLiteral("reveal=0 时一个墨点都没有(墨量 %1,墨点 %2)").arg(num(ink0.sum, 1)).arg(ink0.count));
        check(ink100.count > 50 && ink100.sum > 1000,
              QStringLiteral("reveal=1 时行是完整可见的(墨点 %1,墨量 %2)").arg(ink100.count).arg(num(ink100.sum, 0)));
        check(ratio > 0.35 && ratio < 0.65,
              QStringLiteral("reveal=0.5 时墨量约为满格的一半(实测比 %1)").arg(num(ratio, 3)));
        check(ink50.centroidY > ink100.centroidY,
              QStringLiteral("进度越低文字越靠下(§4 的 8px 上移;重心 %1 > %2)")
                  .arg(num(ink50.centroidY, 1), num(ink100.centroidY, 1)));

        check(row->property("reveal").isValid() && row->property("reveal").toDouble() == 0.5,
              QStringLiteral("reveal 是 Qt 属性,动画写的就是它"));
        useFakeClock();
        g_fakeNow = 0;
        row->setReveal(0.0);
        Anim anim = Anim::to(row, "reveal", 1.0, 200, Ease::Out);
        anim.start();
        for (qint64 t = 0; t <= 200; t += 25)
            pumpAt(t);
        check(row->geometry() == geo0, QStringLiteral("动画全程几何不变(不 setGeometry)"));
        checkNear(row->reveal(), 1.0, 1e-9, QStringLiteral("自绘属性走到终值"));
        useRealClock();
        delete row;
    }

    std::printf("RESULT: %s(%d 项通过 / %d 项失败)\n", g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
