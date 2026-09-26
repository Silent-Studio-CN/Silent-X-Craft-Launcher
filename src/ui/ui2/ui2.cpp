/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 新界面 M1 骨架:外壳(标题行 / 主导航 / 子栏槽 / 内容区) + 令牌与 QSS 生成 + 折叠动画。
//
// 三条纪律从第一天就落到代码里(docs/27 §0):
//   1. 控件只带**角色**(objectName),颜色全由**令牌 -> QSS** 生成 —— 页面里不许 setStyleSheet(写死色);
//   2. I/O 不进界面线程(下一步的 Task 原语;M1 里还没有任何 I/O);
//   3. 动效只有一个时钟、一条曲线表(§10.2.1),页面不许自己起动画。

#define _CRT_SECURE_NO_WARNINGS 1

#include "ui2.h"

#include <cstdio>
#include <cstdlib>

#include <QApplication>
#include <QCursor>
#include <QEasingCurve>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QFile>
#include <QDateTime>

namespace sxcl::ui2 {
namespace {

/* ── 令牌(§1:一个底色、两层浮起;强调色只出现在能动手的地方) ── */
struct Tokens {
    const char *bg;
    const char *nav;
    const char *surface;
    const char *surfaceHover;
    const char *text;
    const char *text2;
    const char *accent;
    const char *line;
};
const Tokens kDark{"#18181c", "#1e1e23", "#24242a", "#2e2e36", "#f0f0f5",
                    "#a8a8b2", "#299cff", "#36363e"};
const Tokens kLight{"#f6f6f8", "#eeeef1", "#ffffff", "#f0f0f4", "#16161a",
                    "#5c5c66", "#0f6cbd", "#e2e2e8"};

bool g_dark = true;

/* ── 标题行那两枚小图标:**自绘**(用户 2026-09-26:界面里不许再拿 "☰ / ◐" 这类字符当图标 ——
 *    它们最终由系统字体决定字形,大小/颜色/基线都不受控)。颜色取当前主题的文字令牌,
 *    与原来那条 QSS 里给字符上色的令牌是同一个(t.text2)。 ── */
QPixmap ui2Glyph(int size, const QColor &color, bool hamburger) {
    const qreal scale = 2.0; // 小图标按 2x 画,缩放屏上也清楚
    QPixmap pm(int(size * scale), int(size * scale));
    pm.setDevicePixelRatio(scale);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    if (hamburger) { // 三横
        for (int i = 0; i < 3; ++i) {
            const qreal y = size * (0.28 + 0.22 * i);
            p.drawLine(QPointF(size * 0.22, y), QPointF(size * 0.78, y));
        }
        return pm;
    }
    // 主题切换:一个"半明半暗"的圆(与原来的 "◐" 同义)
    const QRectF box(size * 0.2, size * 0.2, size * 0.6, size * 0.6);
    p.drawEllipse(box);
    QPainterPath half;
    half.moveTo(box.center().x(), box.top());
    half.arcTo(box, 90, -180);
    half.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(half);
    return pm;
}

/* ── 全站唯一曲线表(§10.2.1 第 2 条:页面只写语义名) ── */
QEasingCurve ease(const char *name) {
    const QString n = QString::fromLatin1(name);
    if (n == QLatin1String("back"))
        return QEasingCurve(QEasingCurve::OutBack);
    if (n == QLatin1String("in"))
        return QEasingCurve(QEasingCurve::InCubic);
    return QEasingCurve(QEasingCurve::OutCubic); // 默认 = Out
}

/* ── QSS:唯一来源(§1/§0 第 1 条)。改主题 = 重跑这一趟 ── */
QString qss() {
    const Tokens &t = g_dark ? kDark : kLight;
    return QString::fromLatin1(R"(
#sxcl2Root { background: %1; }
#sxcl2Head { background: %2; }
#sxcl2Title { color: %5; font-size: 15px; font-weight: 600; }
#sxcl2Sub { color: %6; font-size: 11px; }
#sxcl2Nav { background: %2; }
#sxcl2NavBtn { color: %6; background: transparent; border: none; font-size: 11px; padding-top: 26px; }
#sxcl2NavBtn:hover { color: %5; }
#sxcl2NavBtn[active="true"] { color: %5; background: %4; border-radius: 10px; }
#sxcl2SubRail { background: %2; }
#sxcl2SubTitle { color: %5; font-size: 13px; font-weight: 600; }
#sxcl2SubRow { color: %6; font-size: 13px; padding: 8px 10px; border-radius: 8px; }
#sxcl2SubRow:hover { background: %4; color: %5; }
#sxcl2SubRow[active="true"] { background: %4; color: %5; }
#sxcl2Card { background: %3; border-radius: 12px; }
#sxcl2CardTitle { color: %5; font-size: 30px; font-weight: 600; }
#sxcl2CardSub { color: %6; font-size: 12px; }
#sxcl2Primary { background: %7; color: #ffffff; border: none; border-radius: 10px;
                font-size: 14px; font-weight: 600; padding: 12px 22px; }
#sxcl2Ghost { background: %3; color: %5; border: none; border-radius: 10px;
              font-size: 13px; padding: 12px 18px; }
#sxcl2Hamburger { background: transparent; color: %6; border: none; font-size: 18px; }
#sxcl2Hamburger:hover { color: %5; }
)")
        .arg(QString::fromLatin1(t.bg), QString::fromLatin1(t.nav), QString::fromLatin1(t.surface),
             QString::fromLatin1(t.surfaceHover), QString::fromLatin1(t.text),
             QString::fromLatin1(t.text2), QString::fromLatin1(t.accent));
}

class Shell : public QWidget {
public:
    explicit Shell(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("sxcl2Root"));
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        /* 标题行(§2):左边汉堡 = 子栏展开/收起;中间标题;右边主题切换(验收用,正式版挪进设置) */
        auto *head = new QWidget(this);
        head->setObjectName(QStringLiteral("sxcl2Head"));
        head->setFixedHeight(46);
        auto *hl = new QHBoxLayout(head);
        hl->setContentsMargins(10, 0, 12, 0);
        hl->setSpacing(10);
        auto *burger = new QToolButton(head);
        burger->setObjectName(QStringLiteral("sxcl2Hamburger"));
        burger->setIconSize(QSize(18, 18));
        m_burger = burger;
        burger->setFixedSize(28, 28);
        burger->setCursor(Qt::PointingHandCursor);
        connect(burger, &QToolButton::clicked, this, [this] { toggleSub(); });
        hl->addWidget(burger);
        auto *title = new QLabel(QStringLiteral("Silent X Craft Launcher"), head);
        title->setObjectName(QStringLiteral("sxcl2Title"));
        hl->addWidget(title);
        auto *sub = new QLabel(QStringLiteral("0.2.0"), head);
        sub->setObjectName(QStringLiteral("sxcl2Sub"));
        hl->addWidget(sub);
        hl->addStretch(1);
        auto *theme = new QToolButton(head);
        theme->setObjectName(QStringLiteral("sxcl2Hamburger"));
        theme->setIconSize(QSize(18, 18));
        m_themeButton = theme;
        theme->setFixedSize(28, 28);
        theme->setCursor(Qt::PointingHandCursor);
        connect(theme, &QToolButton::clicked, this, [this] {
            g_dark = !g_dark;
            applyTheme();   // 换主题 = 只重套这一趟(没有一处把颜色写进控件)
        });
        hl->addWidget(theme);
        outer->addWidget(head);

        auto *body = new QWidget(this);
        auto *bl = new QHBoxLayout(body);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(0);
        bl->addWidget(buildNav());
        bl->addWidget(buildSubRail());
        bl->addWidget(buildContent(), 1);
        outer->addWidget(body, 1);

        applyTheme();
    }

    /** 子栏展开/收起:220ms OutCubic —— 全站唯一的动画时钟(§10.2.1)。 */
    void toggleSub() {
        const int from = m_sub->width();
        const int to = (from > 48) ? 48 : 240;
        auto *ani = new QPropertyAnimation(m_sub, "fixedWidth", this);
        ani->setStartValue(from);
        ani->setEndValue(to);
        ani->setDuration(220);
        ani->setEasingCurve(ease("out"));
        ani->start(QAbstractAnimation::DeleteWhenStopped);
        // 收起时把内容淡出(同一根时间线;这一版先做宽度,淡入留给 M2)
        m_subTitle->setVisible(to > 48);
    }

private:
    QWidget *buildNav() {
        auto *nav = new QWidget(this);
        nav->setObjectName(QStringLiteral("sxcl2Nav"));
        nav->setFixedWidth(64);
        auto *v = new QVBoxLayout(nav);
        v->setContentsMargins(8, 10, 8, 10);
        v->setSpacing(6);
        const char *names[] = {"主页", "版本", "下载", "设置"};
        for (int i = 0; i < 4; ++i) {
            auto *b = new QToolButton(nav);
            b->setObjectName(QStringLiteral("sxcl2NavBtn"));
            b->setText(QString::fromUtf8(names[i]));
            b->setFixedHeight(52);
            b->setCursor(Qt::PointingHandCursor);
            b->setProperty("active", i == 0);
            b->setToolButtonStyle(Qt::ToolButtonTextOnly);
            v->addWidget(b);
        }
        v->addStretch(1);
        return nav;
    }

    QWidget *buildSubRail() {
        m_sub = new QWidget(this);
        m_sub->setObjectName(QStringLiteral("sxcl2SubRail"));
        m_sub->setFixedWidth(240);
        auto *v = new QVBoxLayout(m_sub);
        v->setContentsMargins(12, 12, 12, 12);
        v->setSpacing(4);
        m_subTitle = new QLabel(QStringLiteral("收藏版本"), m_sub);
        m_subTitle->setObjectName(QStringLiteral("sxcl2SubTitle"));
        v->addWidget(m_subTitle);
        const char *rows[] = {"1.20.1-Forge", "1.20.1-Quilt", "1.12.1", "26.3"};
        for (int i = 0; i < 4; ++i) {
            auto *r = new QLabel(QString::fromUtf8(rows[i]), m_sub);
            r->setObjectName(QStringLiteral("sxcl2SubRow"));
            r->setProperty("active", i == 0);
            r->setCursor(Qt::PointingHandCursor);
            v->addWidget(r);
        }
        v->addStretch(1);
        return m_sub;
    }

    QWidget *buildContent() {
        auto *wrap = new QWidget(this);
        auto *v = new QVBoxLayout(wrap);
        v->setContentsMargins(24, 20, 24, 24);
        v->setSpacing(16);
        m_contentTitle = new QLabel(QStringLiteral("主页"), wrap);
        m_contentTitle->setObjectName(QStringLiteral("sxcl2SubTitle"));
        v->addWidget(m_contentTitle);

        auto *card = new QWidget(wrap);
        card->setObjectName(QStringLiteral("sxcl2Card"));
        auto *cv = new QVBoxLayout(card);
        cv->setContentsMargins(24, 20, 24, 20);
        cv->setSpacing(10);
        auto *cap = new QLabel(QStringLiteral("当前版本"), card);
        cap->setObjectName(QStringLiteral("sxcl2CardSub"));
        cv->addWidget(cap);
        auto *name = new QLabel(QStringLiteral("1.20.1-Forge"), card);
        name->setObjectName(QStringLiteral("sxcl2CardTitle"));
        cv->addWidget(name);
        auto *meta = new QLabel(QStringLiteral("Forge 47.2.0 · Java 17"), card);
        meta->setObjectName(QStringLiteral("sxcl2CardSub"));
        cv->addWidget(meta);
        auto *row = new QHBoxLayout;
        auto *go = new QToolButton(card);
        go->setObjectName(QStringLiteral("sxcl2Primary"));
        go->setText(QStringLiteral("启动游戏"));
        go->setCursor(Qt::PointingHandCursor);
        auto *sw = new QToolButton(card);
        sw->setObjectName(QStringLiteral("sxcl2Ghost"));
        sw->setText(QStringLiteral("切换版本"));
        sw->setCursor(Qt::PointingHandCursor);
        connect(sw, &QToolButton::clicked, this, [this] { toggleSub(); }); // M1 先接到子栏开关
        row->addWidget(go);
        row->addWidget(sw);
        row->addStretch(1);
        cv->addLayout(row);
        v->addWidget(card);
        v->addStretch(1);
        return wrap;
    }

    void applyTheme() {
        setStyleSheet(qss());     // 唯一一处样式入口
        refreshIcons();           // 图标颜色也跟主题(自绘,不吃 QSS 的 color)
        update();
    }

    /** 标题行两枚自绘图标按当前主题重画(t.text2 = 原来 QSS 给那两个字符上的色)。 */
    void refreshIcons() {
        const Tokens &t = g_dark ? kDark : kLight;
        const QColor color(QString::fromLatin1(t.text2));
        if (m_burger != nullptr)
            m_burger->setIcon(QIcon(ui2Glyph(18, color, true)));
        if (m_themeButton != nullptr)
            m_themeButton->setIcon(QIcon(ui2Glyph(18, color, false)));
    }

    QToolButton *m_burger = nullptr;
    QToolButton *m_themeButton = nullptr;
    QWidget *m_sub = nullptr;
    QLabel *m_subTitle = nullptr;
    QLabel *m_contentTitle = nullptr;
};

} // namespace

int run(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Silent X Craft Launcher"));
    Shell shell;
    shell.resize(1100, 720);
    shell.setWindowTitle(QStringLiteral("Silent X Craft Launcher 0.2.0 (UI2 M1)"));
    shell.show();
    std::fprintf(stderr, "[sxcl-ui2] 新外壳已就绪(令牌/外壳/折叠动画)\n");

    /* 验收通路:截图后退出(SXCL_UI_SHOT / SXCL_UI_SHOT_DELAY),与老界面同一套环境变量。 */
    const QString shot = qEnvironmentVariable("SXCL_UI_SHOT");
    if (!shot.isEmpty()) {
        int delay = qEnvironmentVariableIntValue("SXCL_UI_SHOT_DELAY");
        if (delay <= 0)
            delay = 2500;
        QTimer::singleShot(delay, &app, [&shell, shot]() {
            const bool ok = shell.grab().save(shot);
            std::fprintf(stderr, "[sxcl-ui2] 截图 %s %s\n", shot.toUtf8().constData(),
                         ok ? "OK" : "FAILED");
            QCoreApplication::quit();
        });
    }
    return app.exec();
}

} // namespace sxcl::ui2
