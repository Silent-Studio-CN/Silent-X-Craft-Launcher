/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 新界面 M2:外壳(标题行 / 主导航 / 子栏槽 / 内容区)+ 主页四件套
// (当前版本大卡 / 收藏版本手风琴 / 玩家卡 / 公告卡)+ 任务原语接线。
//
// 三条纪律从第一天就落到代码里(docs/27 §0):
//   1. 控件只带**角色**(objectName),颜色全由**令牌 -> QSS** 生成 —— 页面里不许 setStyleSheet;
//   2. I/O 不进界面线程:主页的取数全走 task.*(工作线程),页面一行阻塞调用都没有;
//   3. 动效只有一个时钟、一条曲线表(§10.2.1):外壳折叠与手风琴都是 220ms OutCubic。

#define _CRT_SECURE_NO_WARNINGS 1

#include "ui2.h"

#include <cstdio>
#include <cstdlib>

#include <QApplication>
#include <QEasingCurve>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "../crash_handler.h" // 崩溃取证 + 界面卡住看门狗(§14:装完必须**武装**)
#include "pages/home.h"
#include "sources.h"
#include "sxcl/log.h"
#include "task.h"
#include "tokens.h"
#include "widgets/accordion.h"
#include "widgets/hero_card.h"
#include "widgets/icon_button.h"
#include "widgets/player_card.h"

namespace sxcl::ui2 {
namespace {

/* ── 标题行那两枚小图标:**自绘**(界面里不拿 "☰ / ◐" 这类字符当图标 ——
 *    它们最终由系统字体决定字形,大小/颜色/基线都不受控)。颜色取当前主题的文字令牌。 ── */
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
    // 主题切换:一个"半明半暗"的圆
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
    const Tokens &t = tokens();
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
#sxcl2ContentTitle { color: %5; font-size: 20px; font-weight: 600; }
#sxcl2Placeholder { color: %6; font-size: 13px; }
#sxcl2Primary { background: %7; color: #ffffff; border: none; border-radius: 10px;
                font-size: 14px; font-weight: 600; padding: 12px 22px; }
#sxcl2Primary:disabled { background: %4; color: %6; }
#sxcl2Ghost { background: %3; color: %5; border: none; border-radius: 10px;
              font-size: 13px; padding: 12px 18px; }
#sxcl2Ghost:hover { background: %4; }
#sxcl2Hamburger { background: transparent; color: %6; border: none; font-size: 18px; }
#sxcl2Hamburger:hover { color: %5; }
#sxcl2HeroCard { background: %3; border-radius: 12px; }
#sxcl2HeroCaption { color: %6; font-size: 11px; }
#sxcl2HeroTitle { color: %5; font-size: 30px; font-weight: 600; }
#sxcl2HeroMeta { color: %6; font-size: 12px; }
#sxcl2PlayerCard { background: %3; border-radius: 12px; }
#sxcl2PlayerCaption { color: %6; font-size: 11px; }
#sxcl2PlayerName { color: %5; font-size: 20px; font-weight: 600; }
#sxcl2VersionAccordion { background: %3; border-radius: 12px; }
#sxcl2AccordionTitle { color: %6; font-size: 11px; }
#sxcl2AccordionAllText { color: %6; font-size: 13px; }
#sxcl2AccordionAll:hover #sxcl2AccordionAllText { color: %5; }
#sxcl2VersionRow { background: transparent; border-radius: 8px; }
#sxcl2VersionRow:hover { background: %4; }
#sxcl2VersionRowName { color: %5; font-size: 13px; }
#sxcl2VersionRowMeta { color: %6; font-size: 11px; }
#sxcl2NoticeCard { background: %3; border-radius: 12px; }
#sxcl2NoticeTitle { color: %5; font-size: 13px; font-weight: 600; }
#sxcl2NoticeBody { color: %6; font-size: 13px; }
)")
        .arg(QString::fromLatin1(t.bg), QString::fromLatin1(t.nav), QString::fromLatin1(t.surface),
             QString::fromLatin1(t.surfaceHover), QString::fromLatin1(t.text),
             QString::fromLatin1(t.text2), QString::fromLatin1(t.accent));
}

/* ── 控件树 dump(与老界面同一行格式,验收脚本能同一套正则读它) ──
 *   ClassName #objectName (x,y WxH) [hidden] [disabled] "文字"
 *   x/y 是**窗口坐标**(mapTo(window)),所以任何一层都能直接与截图对齐。 */
void dumpTree(QWidget *root, int maxDepth) {
    struct Walker {
        static void walk(QWidget *widget, int depth, int maxDepth) {
            if (widget == nullptr || depth > maxDepth)
                return;
            const QPoint topLeft = widget->mapTo(widget->window(), QPoint(0, 0));
            QString text;
            if (auto *label = qobject_cast<QLabel *>(widget))
                text = label->text();
            else if (auto *button = qobject_cast<QAbstractButton *>(widget))
                text = button->text();
            text.replace(QLatin1Char('\n'), QLatin1Char(' '));
            if (text.size() > 80)
                text = text.left(80) + QStringLiteral("…");
            std::fprintf(stderr, "%*s%s", depth * 2, "", widget->metaObject()->className());
            if (!widget->objectName().isEmpty())
                std::fprintf(stderr, " #%s", widget->objectName().toUtf8().constData());
            std::fprintf(stderr, " (%d,%d %dx%d)%s%s", topLeft.x(), topLeft.y(), widget->width(),
                         widget->height(), widget->isVisible() ? "" : " hidden",
                         widget->isEnabled() ? "" : " disabled");
            if (!text.isEmpty())
                std::fprintf(stderr, " \"%s\"", text.toUtf8().constData());
            std::fprintf(stderr, "\n");
            const QList<QWidget *> children =
                widget->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
            for (QWidget *child : children)
                walk(child, depth + 1, maxDepth);
        }
    };
    Walker::walk(root, 0, maxDepth);
}

/* ── 外壳(docs/27 §2:主导航与子栏都是**外壳的槽**;内容区标题属于外壳) ── */
class Shell : public QWidget {
public:
    explicit Shell(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("sxcl2Root"));
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

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
            setDarkTheme(!darkTheme());
            applyTheme(); // 换主题 = 只重套这一趟(没有一处把颜色写进控件)
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
        setRoute(QStringLiteral("home"));
    }

    HomePage *home() const { return m_home; }
    QString route() const { return m_route; }

    /** 切路由(§2:页面切换是外壳的事;M2 只有主页,其余路由占位到 M3。 */
    void setRoute(const QString &route) {
        const QString wanted = route.isEmpty() ? QStringLiteral("home") : route;
        const bool known = (wanted == QLatin1String("home") || wanted == QLatin1String("versions") ||
                            wanted == QLatin1String("download") ||
                            wanted == QLatin1String("settings"));
        const QString target = known ? wanted : QStringLiteral("home");
        m_route = target;
        const bool isHome = (target == QLatin1String("home"));
        if (m_stack != nullptr)
            m_stack->setCurrentWidget(isHome ? static_cast<QWidget *>(m_home) : m_placeholder);
        if (m_placeholderLabel != nullptr)
            m_placeholderLabel->setText(routeTitle(target));
        if (m_contentTitle != nullptr)
            m_contentTitle->setText(routeTitle(target));
        for (QToolButton *button : m_navButtons) {
            const bool active = button->property("route").toString() == target;
            if (button->property("active").toBool() == active)
                continue;
            button->setProperty("active", active);
            button->style()->unpolish(button);
            button->style()->polish(button);
        }
        std::fprintf(stderr, "[sxcl-ui2] route=%s\n", m_route.toUtf8().constData());
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
        m_subTitle->setVisible(to > 48);
    }

    void dump(const QString &state) const {
        std::fprintf(stderr, "[sxcl-ui2] dump: state=%s route=%s violations=%d\n",
                     state.toUtf8().constData(), m_route.toUtf8().constData(),
                     mainThreadViolations());
        dumpTree(const_cast<Shell *>(this), 10); // 深度 10:主页 -> 卡片 -> 行 -> 行里的标签都看得见
    }

private:
    static QString routeTitle(const QString &route) {
        if (route == QLatin1String("versions"))
            return QStringLiteral("版本");
        if (route == QLatin1String("download"))
            return QStringLiteral("下载");
        if (route == QLatin1String("settings"))
            return QStringLiteral("设置");
        return QStringLiteral("主页");
    }

    QWidget *buildNav() {
        auto *nav = new QWidget(this);
        nav->setObjectName(QStringLiteral("sxcl2Nav"));
        nav->setFixedWidth(64);
        auto *v = new QVBoxLayout(nav);
        v->setContentsMargins(8, 10, 8, 10);
        v->setSpacing(6);
        const char *routes[] = {"home", "versions", "download", "settings"};
        const char *names[] = {"主页", "版本", "下载", "设置"};
        for (int i = 0; i < 4; ++i) {
            auto *b = new QToolButton(nav);
            b->setObjectName(QStringLiteral("sxcl2NavBtn"));
            b->setText(QString::fromUtf8(names[i]));
            b->setFixedHeight(52);
            b->setCursor(Qt::PointingHandCursor);
            b->setProperty("route", QString::fromLatin1(routes[i]));
            b->setProperty("active", i == 0);
            b->setToolButtonStyle(Qt::ToolButtonTextOnly);
            const QString route = QString::fromLatin1(routes[i]);
            connect(b, &QToolButton::clicked, this, [this, route] { setRoute(route); });
            m_navButtons.append(b);
            v->addWidget(b);
        }
        v->addStretch(1);
        return nav;
    }

    QWidget *buildSubRail() {
        /* 子栏是**外壳的槽**(§2),内容由页面按路由填(版本页/M3)。M1 里那几行假版本
         * 已经删掉 —— 假数据比空栏更贵(shell 里不该有"看起来像真数据"的东西)。
         * 默认收起(48):主页的"收藏版本"是那张大卡下面的手风琴(§11.3)。 */
        m_sub = new QWidget(this);
        m_sub->setObjectName(QStringLiteral("sxcl2SubRail"));
        m_sub->setFixedWidth(48);
        auto *v = new QVBoxLayout(m_sub);
        v->setContentsMargins(12, 12, 12, 12);
        v->setSpacing(4);
        m_subTitle = new QLabel(QStringLiteral("收藏版本"), m_sub);
        m_subTitle->setObjectName(QStringLiteral("sxcl2SubTitle"));
        m_subTitle->setVisible(false);
        v->addWidget(m_subTitle);
        v->addStretch(1);
        return m_sub;
    }

    QWidget *buildContent() {
        auto *wrap = new QWidget(this);
        auto *v = new QVBoxLayout(wrap);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        // 内容区标题属于**外壳**(§2),与子栏无关
        auto *titleWrap = new QWidget(wrap);
        auto *tw = new QVBoxLayout(titleWrap);
        tw->setContentsMargins(24, 18, 24, 0);
        m_contentTitle = new QLabel(QStringLiteral("主页"), titleWrap);
        m_contentTitle->setObjectName(QStringLiteral("sxcl2ContentTitle"));
        tw->addWidget(m_contentTitle);
        v->addWidget(titleWrap);

        m_stack = new QStackedWidget(wrap);
        m_stack->setObjectName(QStringLiteral("sxcl2Stack"));
        m_home = new HomePage(m_stack);
        m_stack->addWidget(m_home);
        m_placeholder = new QWidget(m_stack);
        m_placeholder->setObjectName(QStringLiteral("sxcl2PlaceholderPage"));
        auto *pv = new QVBoxLayout(m_placeholder);
        pv->setContentsMargins(24, 20, 24, 24);
        m_placeholderLabel = new QLabel(m_placeholder);
        m_placeholderLabel->setObjectName(QStringLiteral("sxcl2Placeholder"));
        pv->addWidget(m_placeholderLabel);
        pv->addStretch(1);
        m_stack->addWidget(m_placeholder);
        v->addWidget(m_stack, 1);
        return wrap;
    }

    void applyTheme() {
        setStyleSheet(qss()); // 唯一一处样式入口
        refreshIcons();       // 图标颜色也跟主题(自绘,不吃 QSS 的 color)
        // 自绘控件(图标钮/头像)在 paintEvent 里现取令牌,这里只要把它们排一次重画
        for (QWidget *child : findChildren<QWidget *>())
            child->update();
        update();
    }

    /** 标题行两枚自绘图标按当前主题重画。 */
    void refreshIcons() {
        const QColor color = tokenColor(tokens().text2);
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
    QStackedWidget *m_stack = nullptr;
    HomePage *m_home = nullptr;
    QWidget *m_placeholder = nullptr;
    QLabel *m_placeholderLabel = nullptr;
    QVector<QToolButton *> m_navButtons;
    QString m_route;
};

/** 窗口尺寸:SXCL_UI_WINDOW=宽x高(与老界面同一个开关);默认 1100x750。 */
void applyWindowSize(QWidget *window) {
    int w = 1100;
    int h = 750;
    const QString spec = qEnvironmentVariable("SXCL_UI_WINDOW").trimmed().toLower();
    if (!spec.isEmpty()) {
        const QStringList parts = spec.split(QLatin1Char('x'));
        if (parts.size() == 2) {
            const int pw = parts.at(0).toInt();
            const int ph = parts.at(1).toInt();
            if (pw >= 640 && ph >= 480) {
                w = pw;
                h = ph;
            }
        }
    }
    window->resize(w, h);
}

/** 给 LinkRow 送一次真正的鼠标点击(产品路径,不是直接调信号)。 */
void clickWidget(QWidget *widget) {
    if (widget == nullptr)
        return;
    const QPointF local(widget->width() / 2.0, widget->height() / 2.0);
    const QPointF global = widget->mapToGlobal(local.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QApplication::sendEvent(widget, &release);
}

} // namespace

int run(int argc, char **argv) {
    /* 运行日志**先开**:看门狗落报告的目录(crashDir)读的就是运行日志路径 ——
     * 不开日志,那份 hang 报告会静默不写,验收就成了"0 份报告"的假绿(docs/27 §14.4 第 3 条)。 */
    char logError[SXCL_LOG_ERROR_MAX];
    logError[0] = '\0';
    const int logRc = sxcl_log_init(nullptr, logError, sizeof(logError));
    SXCL_LOG_I("startup", "===== SXCL 启动(新界面 ui2 / M2) 构建=%s =====", __DATE__);
    if (logRc != SXCL_LOG_OK)
        std::fprintf(stderr, "[sxcl-ui2] 运行日志打不开(%s),看门狗报告也会跟着写不出来\n",
                     logError);
    sxcl::ui::installCrashHandler();
    sxcl::ui::installHangWatchdog(3000);

    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion")); // 与老界面同一口径(控件度量一致)
    QApplication::setApplicationName(QStringLiteral("Silent X Craft Launcher"));
    sxcl::ui::armHangWatchdog(); // 必须在 QApplication 之后、exec 之前(§14.4 第 2 条)

    Shell shell;
    applyWindowSize(&shell);
    shell.setWindowTitle(QStringLiteral("Silent X Craft Launcher 0.2.0 (UI2 M2)"));
    // 版本形态偏好(game.edition;键沿用下载页那个)—— 先落高亮,再取数
    shell.home()->setPremium(readPremiumEdition());
    shell.show();
    std::fprintf(stderr,
                 "[sxcl-ui2] 新外壳已就绪(令牌/外壳/主页四件套) 逻辑=%dx%d dpr=%.2f 物理=%dx%d\n",
                 shell.width(), shell.height(), shell.devicePixelRatioF(),
                 int(shell.width() * shell.devicePixelRatioF()),
                 int(shell.height() * shell.devicePixelRatioF()));

    // 取数:工作线程(构造期一个阻塞调用都没有)
    shell.home()->startLoad();

    const bool dumpOn = qEnvironmentVariableIntValue("SXCL_UI_DUMP") == 1;
    const bool hoverHook = qEnvironmentVariableIntValue("SXCL_UI2_HOVER_NAME") == 1;
    const bool accordionHook = qEnvironmentVariableIntValue("SXCL_UI2_CLICK_VERSION") == 1;
    const bool clickAll = qEnvironmentVariableIntValue("SXCL_UI2_CLICK_ALL") == 1;
    const QString editionHook = qEnvironmentVariable("SXCL_UI2_EDITION").trimmed().toLower();
    const QString navHook = qEnvironmentVariable("SXCL_UI2_NAV").trimmed().toLower();
    const QString shot = qEnvironmentVariable("SXCL_UI_SHOT");
    const int settle = 900; // 布局/图标/工作线程结果落定(与老界面钩子同一个 900ms)

    auto state = [&shell, dumpOn](const QString &name) {
        if (dumpOn)
            shell.dump(name);
    };

    QTimer::singleShot(settle, &shell, [&state] { state(QStringLiteral("idle")); });

    int lastStep = settle;
    if (hoverHook) {
        HomePage *home = shell.home();
        QTimer::singleShot(settle + 200, &shell,
                           [home] { home->player()->nameRow()->simulateHover(true); });
        QTimer::singleShot(settle + 400, &shell, [&state] { state(QStringLiteral("hover")); });
        QTimer::singleShot(settle + 700, &shell,
                           [home] { home->player()->nameRow()->simulateHover(false); });
        QTimer::singleShot(settle + 900, &shell, [&state] { state(QStringLiteral("leave")); });
        lastStep = settle + 900;
    }
    if (accordionHook) {
        HomePage *home = shell.home();
        QTimer::singleShot(settle + 1200, &shell,
                           [&state] { state(QStringLiteral("accordion-closed")); });
        QTimer::singleShot(settle + 1400, &shell, [home] { home->hero()->swapButton()->click(); });
        QTimer::singleShot(settle + 2000, &shell,
                           [&state] { state(QStringLiteral("accordion-open")); });
        if (clickAll) {
            // 「查看全部版本」= 产品路径(真鼠标事件)-> 路由切到版本页(M3 填内容)
            QTimer::singleShot(settle + 2200, &shell,
                               [home] { clickWidget(home->accordion()->allButton()); });
            QTimer::singleShot(settle + 2500, &shell, [&state] { state(QStringLiteral("all")); });
            lastStep = settle + 2500;
        } else {
            QTimer::singleShot(settle + 2200, &shell,
                               [home] { home->hero()->swapButton()->click(); });
            QTimer::singleShot(settle + 2800, &shell,
                               [&state] { state(QStringLiteral("accordion-close")); });
            lastStep = settle + 2800;
        }
    }
    if (!editionHook.isEmpty()) {
        HomePage *home = shell.home();
        const bool premium = editionHook != QLatin1String("offline");
        QTimer::singleShot(settle + 3200, &shell, [home, premium] {
            if (premium)
                home->player()->premiumButton()->click();
            else
                home->player()->offlineButton()->click();
        });
        QTimer::singleShot(settle + 3500, &shell,
                           [&state, editionHook] { state(QStringLiteral("edition-") + editionHook); });
        lastStep = settle + 3500;
    }
    if (!navHook.isEmpty()) {
        QTimer::singleShot(settle + 3800, &shell, [&shell, navHook] { shell.setRoute(navHook); });
        QTimer::singleShot(settle + 4000, &shell,
                           [&state, navHook] { state(QStringLiteral("nav-") + navHook); });
        lastStep = settle + 4000;
    }

    /* 截图通路(与老界面同一个环境变量):SXCL_UI_SHOT + SXCL_UI_SHOT_DELAY(默认 4.5s,
     * 要落在所有钩子之后)。抓完就退出。 */
    if (!shot.isEmpty()) {
        int delay = qEnvironmentVariableIntValue("SXCL_UI_SHOT_DELAY");
        if (delay <= 0)
            delay = 4500;
        QTimer::singleShot(delay, &app, [&shell, shot]() {
            const QPixmap pm = shell.grab();
            const bool ok = pm.save(shot);
            std::fprintf(stderr, "[sxcl-ui2] 截图 %s %dx%d dpr=%.2f %s\n",
                         shot.toUtf8().constData(), pm.width(), pm.height(),
                         pm.devicePixelRatio(), ok ? "OK" : "FAILED");
            QCoreApplication::quit();
        });
    } else if (hoverHook || accordionHook || !editionHook.isEmpty() || !navHook.isEmpty()) {
        // 钩子跑完自己退出(没有截图通路时)—— 验收脚本靠进程退出码说话
        QTimer::singleShot(lastStep + 600, &app, [] { QCoreApplication::quit(); });
    }
    return app.exec();
}

} // namespace sxcl::ui2
