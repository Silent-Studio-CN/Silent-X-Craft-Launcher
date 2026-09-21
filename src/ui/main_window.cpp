#include "main_window.h"

#include "pages/page_factory.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSet>
#include <QShowEvent>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>

#include "fluent/fluent_controls.h" // Min/Max/CloseButton(qf 三键的 libqf 基类)

// 窗口边缘命中(WM_NCHITTEST)要用的 Win32 常量/macro。windows.h 自带 min/max 宏会污染
// 后面的 C++ 代码,先关掉(NOMINMAX);Windows SDK 头在 /W4 下有杂音,整段静音。
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "theme_bridge.h"
#include "tray.h"             // 任务栏托盘(最小化=隐藏后的恢复入口;新增设计)
#include "workers/ui_paths.h" // uiSettingsFilePath / uiTrace(与各页同口径)

#include "sxcl/settings.h" // 读写 ui.close_mode(关闭时提示还是直接退)

// 版本号:标题栏显示的文字 = f"{APP_NAME} {APP_VERSION}"(Python src/app/main_window.py:54)。
// 走相对路径包含 —— include/sxcl/version.h 是版本契约,但 sxcl_ui_core 不链接 sxcl
// (只用编译期宏,不给 UI 冒烟测试增加链接依赖)。
#include "../../include/sxcl/version.h"

namespace sxcl::ui {

const char *MainWindow::kTitle = "Silent X Craft Launcher";

namespace {

// 导航项(顺序即界面顺序,逐条对应 Python src/app/main_window.py:105-119):
//   addSubInterface(home, FIF.HOME, "主页")
//   addSubInterface(versions, grass_block_icon(24), "版本")
//   addSubInterface(tasks, FIF.UPDATE, "任务")
//   addSubInterface(keymap, FIF.LAYOUT, "按键映射")
//   addSubInterface(multiplayer, FIF.GLOBE, "联机")
//   addSubInterface(settings, FIF.SETTING, "设置", BOTTOM)
const NavItem kNavSpec[] = {
    {QStringLiteral("home"), QStringLiteral("Home"), QString(), QStringLiteral("主页"), false},
    {QStringLiteral("versions"), QString(), QStringLiteral("vanilla"), QStringLiteral("版本"), false},
    {QStringLiteral("tasks"), QStringLiteral("Update"), QString(), QStringLiteral("任务"), false},
    {QStringLiteral("keymap"), QStringLiteral("Layout"), QString(), QStringLiteral("按键映射"), false},
    {QStringLiteral("multiplayer"), QStringLiteral("Globe"), QString(), QStringLiteral("联机"), false},
    {QStringLiteral("settings"), QStringLiteral("Setting"), QString(), QStringLiteral("设置"), true},
};

// ---------------------------------------------------------------------------
// 窗口三键:qf 的实现是 qframelesswindow/titlebar/title_bar_buttons.py:230-305
//   * 按钮 46x32(不是 libqf 的 46x37)
//   * 最小化:(18,16)->(28,16) 的 1px 线(cosmetic,画在设备坐标)
//   * 最大化:(18,11) 起的 10x10 方框(按 DPR 缩放到设备坐标画);最大化态画"还原"双层框
//   * 关闭  :把 :/qframelesswindow/close.svg 的 stroke 换成当前颜色后渲染进整块按钮
//     (svg viewBox 15.875x10.583,默认 xMidYMid meet -> 居中 10x10 的 X)
// libqf 同名类的图标盒固定为 rect().adjusted(10,10,-10,-10)(26x12),比参考图大一倍多,
// 这里继承 libqf 的类、只把尺寸与图标改回 qf 的几何:不写 Q_OBJECT,
// metaObject 类名仍是 MinimizeButton/MaximizeButton/CloseButton,
// fluent_window.qss 里的 qproperty-normalColor/hoverBackgroundColor 照旧生效。
class ShellMinimizeButton : public MinimizeButton {
public:
    explicit ShellMinimizeButton(QWidget *parent) : MinimizeButton(parent) { setFixedSize(46, 32); }

protected:
    void drawGlyph(QPainter &p, const QRect &r) override {
        Q_UNUSED(r)
        // qf 的 MinimizeButton.paintEvent 没有开抗锯齿 -> 1px cosmetic 线落在整设备像素上
        // (参考图那行线是纯 #ffffff 的单行;开着抗锯齿会摊成两行 50% 灰)
        p.setRenderHint(QPainter::Antialiasing, false);
        p.drawLine(18, 16, 28, 16);
    }
};

class ShellMaximizeButton : public MaximizeButton {
public:
    explicit ShellMaximizeButton(QWidget *parent) : MaximizeButton(parent) { setFixedSize(46, 32); }

protected:
    void drawGlyph(QPainter &p, const QRect &r) override {
        Q_UNUSED(r)
        // 同上:qf 的 MaximizeButton.paintEvent 也没有开抗锯齿
        p.setRenderHint(QPainter::Antialiasing, false);
        const qreal dpr = devicePixelRatioF();
        p.save();
        p.scale(1.0 / dpr, 1.0 / dpr);
        if (!(window() && window()->isMaximized())) {
            p.drawRect(int(18 * dpr), int(11 * dpr), int(10 * dpr), int(10 * dpr));
        } else {
            p.drawRect(int(18 * dpr), int(13 * dpr), int(8 * dpr), int(8 * dpr));
            const qreal x0 = int(18 * dpr) + int(2 * dpr);
            const qreal y0 = 13 * dpr;
            const qreal dw = int(2 * dpr);
            QPainterPath path(QPointF(x0, y0));
            path.lineTo(x0, y0 - dw);
            path.lineTo(x0 + 8 * dpr, y0 - dw);
            path.lineTo(x0 + 8 * dpr, y0 - dw + 8 * dpr);
            path.lineTo(x0 + 8 * dpr - dw, y0 - dw + 8 * dpr);
            p.drawPath(path);
        }
        p.restore();
    }
};

class ShellCloseButton : public CloseButton {
public:
    explicit ShellCloseButton(QWidget *parent) : CloseButton(parent) { setFixedSize(46, 32); }

protected:
    void drawGlyph(QPainter &p, const QRect &r) override {
        Q_UNUSED(r)
        QColor color = normalColor();
        if (isDown())
            color = pressedColor();
        else if (underMouse())
            color = hoverColor();
        static const QByteArray kRaw = [] {
            QFile f(QStringLiteral(":/qframelesswindow/close.svg"));
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        }();
        if (kRaw.isEmpty())
            return;
        QByteArray svg = kRaw;
        svg.replace("stroke=\"#000\"", "stroke=\"" + color.name().toLatin1() + "\"");
        drawSvgIcon(svg, &p, QRectF(rect()));
    }
};

// ─────────────────────────────────────────────────────────────────────────
// 窗口行为用的小工具(四条语义的规则见 docs/13-窗口行为.md)
// ─────────────────────────────────────────────────────────────────────────

// 自检脚本每一步之间的默认间隔:够窗口系统把最大化/还原/隐藏做完
constexpr int kCheckStepDelayMs = 900;
// 「当前任务/进度」的刷新间隔(小窗口面板与托盘菜单共用同一份取值)
constexpr int kTaskWatchIntervalMs = 500;

// 会话页下面正在运行的 worker 线程数。
// **"这个任务还在跑"只认这一个判据**:install_worker.cpp:118 与 launch_worker.cpp:58
// 都是把 QThread 以 worker 自己为父建出来的,而 worker 挂在页面上 —— 于是"页面子树里
// isRunning() 的 QThread"就是正在跑的任务,不需要 include workers/*.h、也不依赖它们的类型。
// (不拿"界面在不在"当判据:窗口藏起来之后界面本来就不可见,那恰恰是要证明任务还在跑的场景。)
int runningThreadsUnder(QWidget *page) {
    if (page == nullptr)
        return 0;
    int n = 0;
    const QList<QThread *> threads = page->findChildren<QThread *>();
    for (QThread *thread : threads) {
        if (thread != nullptr && thread->isRunning())
            ++n;
    }
    return n;
}

// 页面自己的进度条(不另建一份计数:用户看到的进度就是它)
QProgressBar *pageProgressBar(QWidget *page) {
    return page != nullptr ? page->findChild<QProgressBar *>() : nullptr;
}

// 未完成任务的状态文件路径。放在**设置文件旁边**:SXCL_UI_SETTINGS 一钉,取证就落在
// 临时目录里,不污染用户真实配置(与 workers/ui_paths.cpp 的 uiSettingsFilePath() 同口径)。
QString taskStatePath() {
    const QFileInfo info(uiSettingsFilePath());
    const QString dir = info.absolutePath();
    QDir().mkpath(dir);
    return dir + QStringLiteral("/pending_tasks.json");
}

// 用户配置里的"关闭时怎么处理还在跑的任务":ask = 弹窗问,其它一律 = 直接退。
// **默认直接退**(用户明确要求);设置文件里写 ui.close_mode=ask 才会问。
// SXCL_UI_CLOSE_MODE 是取证通路(钉条件,不改用户配置)。
bool closeNeedsConfirm() {
    const QString pinned = qEnvironmentVariable("SXCL_UI_CLOSE_MODE");
    if (!pinned.isEmpty())
        return pinned.compare(QStringLiteral("ask"), Qt::CaseInsensitive) == 0;
    if (sxcl_settings *st = sxcl_settings_open(uiSettingsFilePath().toUtf8().constData())) {
        const char *mode = sxcl_settings_get(st, "ui.close_mode", "exit");
        const bool ask = mode != nullptr && qstrcmp(mode, "ask") == 0;
        sxcl_settings_free(st);
        return ask;
    }
    return false;
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : FluentWindowBase(true, parent) {
    buildUi();
    ThemeBridge::instance().attach(this);

    // 托盘:只有当前会话**真的**有系统托盘才建。没有托盘时不建、也绝不隐藏窗口
    // (理由见 buildTray:藏了就没有任何入口能把窗口叫回来)。
    buildTray();

    // 任务/进度低频刷新:小窗口面板那一行与托盘菜单的「当前任务」共用这一份取值。
    m_taskWatch = new QTimer(this);
    m_taskWatch->setInterval(kTaskWatchIntervalMs);
    connect(m_taskWatch, &QTimer::timeout, this, &MainWindow::syncTray);
    m_taskWatch->start();

    // 「下次启动能恢复」:把上次没跑完的任务读回来(有显示会话时自动续上)
    loadTaskState(true);

    // 取证通路:脚本化驱动四条窗口行为,每步打一行客观读数(不设这个变量就完全不启用)。
    // 与 main.cpp 的 SXCL_UI_SHOT / SXCL_ANIM_TRACE 同一类通路 —— 只驱动产品路径,不造假状态。
    const QString check = qEnvironmentVariable("SXCL_UI_WINCHECK");
    if (!check.isEmpty())
        startWindowSelfCheck(check);
}

MainWindow::~MainWindow() {
    // 拆卸阶段不再改窗口状态:这时再去 hide()/show() 只会让析构顺序更难说清。
    m_inShutdown = true;
    if (m_taskWatch != nullptr)
        m_taskWatch->stop();
    ThemeBridge::instance().detach(this);
}

void MainWindow::buildUi() {
    setObjectName(QStringLiteral("sxclRoot"));
    setWindowTitle(QString::fromUtf8(kTitle));
    resize(kInitialWidth, kInitialHeight);
    setMinimumSize(kMinimumWidth, kMinimumHeight);

    // ---- 窗口根布局:左导航(整窗高)+ 右内容列(qf fluent_window.py:263-271)----
    // 窗口内**没有阴影留白**:qf 在 Windows 上靠 DWM 画窗口阴影,窗口内容从 (0,0) 起算;
    // libqf 的 FluentWindow::kWinMargin=30 是它自带 FluentWindow 形态的留白,这里不用,
    // 否则内容区会整体内缩 30px(参考图实测内容框左上角 = 逻辑 (48,48))。
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    // 导航面板:占满整窗高(0..H),含标题栏那 48px 那一段(qf 的 navigationInterface 也是这样)
    m_nav = new NavPanel(this);
    row->addWidget(m_nav);

    auto *body = new QWidget(this);
    auto *bodyLay = new QVBoxLayout(body);
    // qf fluent_window.py:271 self.widgetLayout.setContentsMargins(0, 48, 0, 0)
    bodyLay->setContentsMargins(0, kTitleBarHeight, 0, 0);
    bodyLay->setSpacing(0);

    // 内容框必须是 libqf 的 StackedWidget:qf 的 fluent_window.qss:63-69 用 StackedWidget
    // 这个类选择器画边框/圆角,Qt 自带的 QStackedWidget 类名对不上(选择器按 metaObject 类名匹配),
    // 换成它边框才会出现。
    m_stack = new StackedWidget(body);
    m_stack->setObjectName(QStringLiteral("sxclStack"));
    // qf FluentWindowBase.__init__: FluentStyleSheet.FLUENT_WINDOW.apply(self.stackedWidget)
    //   -> border: 1px solid rgba(0,0,0,0.18); border-right: none; border-bottom: none;
    //      border-top-left-radius: 10px; background-color: rgba(255,255,255,0.0314)
    //      (assets/theme/qf_exact/dark/fluent_window.qss:63-69,贴住窗口右边与底边)
    FluentStyleSheet::apply(m_stack, FluentStyleSheet::FLUENT_WINDOW);
    // 这里**不能**在控件级 qss 后面再补一条 "StackedWidget { background-color: transparent; }"。
    // 曾经补过,因为它让内容区看起来"正好"是 #202020(见 fluent_theme.cpp:380 同样的思路),
    // 但那条规则会连 qf 自己那层 rgba(255,255,255,0.0314) 一起抹掉,而**窗口左上角那个
    // 10px 圆角正是靠它画出来的**:圆角内侧、栈自己的 1px 边框与页面 1px 边框之间露出的
    // 那半像素,在参考图里是 #24(36 = 39 抗锯齿),抹掉后只剩窗口底 #202020 的 #1d(29) →
    // 圆弧本身(=23)还在,但圆角里侧那条亮带没了,肉眼就是"圆角被内容盖平了"。
    // 实测证据(build/ref):py_home.png 该处 x=72/73/74 = 28/36/25,c_home.png = 26/29/25。
    // 两条必须一起成立的结论:
    //   * 参考图里的页面底色是**不透明**的 —— 抓图脚本 grab_reference_ui.py:141 自己钉了
    //     page.setStyleSheet("QWidget { background: %s }" % token("bg")),所以内容区仍是
    //     #202020,圆角那半像素才轮到内容栈自己上色(它的底色是故意留着的,不是漏配)。
    //   * 控件级 qss 压得住应用级:参考实现已实证 —— py_download_config_apptheme.png
    //     (补了 apply_theme() 的全局 QSS)与 py_download_config.png 的圆角逐像素相同,
    //     即 fluent_theme.cpp:380 的 "QStackedWidget { background: transparent; }" 压不过 qf 这条。
    // 结论:栈保留 qf 的边框 + 左上圆角 + 自身底色;页面各自钉不透明底色(与参考图同口径)。
    bodyLay->addWidget(m_stack);
    row->addWidget(body, 1);

    // ---- 标题栏:浮层,盖在导航面板右侧那 2px 与内容列之上(qf fluent_window.py:273,344-346)----
    m_titleBar = new FluentTitleBar(this);
    m_titleBar->setFixedHeight(kTitleBarHeight); // qf FluentTitleBar.__init__: setFixedHeight(48)

    // qf FluentTitleBar 的布局 = [iconLabel(18x18), titleLabel, 弹性, 最小化/最大化/关闭]。
    // libqf 在第一位放的是自绘汉堡按钮 —— 那是 libqf 自己 FluentWindow 形态的东西,qf 的
    // 窗口标题栏里没有它(汉堡在导航面板顶部)。换成 qf 的 18x18 窗口图标标签:本窗口没设
    // windowIcon,它就是一块空白占位,正好把标题文字顶到 qf 的位置(46 + 18 + padding 4)。
    if (auto *lay = qobject_cast<QBoxLayout *>(m_titleBar->layout())) {
        QWidget *first = lay->count() > 0 && lay->itemAt(0) ? lay->itemAt(0)->widget() : nullptr;
        if (first && first != m_titleBar->titleLabel()) {
            lay->removeWidget(first);
            first->hide();
        }
        lay->setContentsMargins(0, 0, 0, 0); // qf TitleBar: hBoxLayout(0,0,0,0) + spacing 0
        lay->setSpacing(0);
        m_iconLabel = new QLabel(m_titleBar);
        m_iconLabel->setObjectName(QStringLiteral("titleIcon"));
        m_iconLabel->setFixedSize(18, 18); // qf: self.iconLabel.setFixedSize(18, 18)
        lay->insertWidget(0, m_iconLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
    }

    // 标题文字:Python 是 f"{APP_NAME} {APP_VERSION}"。窗口标题(Win32 属性)保持工程既有
    // 契约 "Silent X Craft Launcher"(冒烟测试断言的就是它),标题栏显示的文字按参考图带版本号。
    m_titleBar->titleLabel()->setText(
        QStringLiteral("%1 %2.%3.%4")
            .arg(QString::fromUtf8(kTitle))
            .arg(SXCL_VERSION_MAJOR)
            .arg(SXCL_VERSION_MINOR)
            .arg(SXCL_VERSION_PATCH));

    // qf: FluentTitleBar.__init__ 与 FluentWidgetTitleBar 都会把 fluent_window.qss 套到
    // 标题栏和三键上 -> 标题栏透明、#titleLabel 13px + padding 0 4px + 白字、
    // 三键 normalColor=white / hover rgba(255,255,255,26) / close hover rgb(232,17,35)。
    FluentStyleSheet::apply(m_titleBar, FluentStyleSheet::FLUENT_WINDOW);

    // 三键:把 libqf 原来的三个(46x37、26x12 图标)换成 qf 几何的同类,位置与顺序不变
    const QList<QAbstractButton *> oldButtons = m_titleBar->findChildren<QAbstractButton *>();
    for (QAbstractButton *b : oldButtons)
        b->hide(); // 尺寸/图标都不对(46x37 / 26x12):藏起来,换成下面三个
    auto *minBtn = new ShellMinimizeButton(m_titleBar);
    auto *maxBtn = new ShellMaximizeButton(m_titleBar);
    auto *closeBtn = new ShellCloseButton(m_titleBar);
    if (auto *lay = qobject_cast<QBoxLayout *>(m_titleBar->layout())) {
        // qf TitleBar 把三键 addWidget(..., Qt::AlignRight) 放在弹性项之后;
        // 垂直方向必须是**顶端**(qf 的按钮在 y 0..32,标题栏 48 高,不是居中)。
        lay->addWidget(minBtn, 0, Qt::AlignTop);
        lay->addWidget(maxBtn, 0, Qt::AlignTop);
        lay->addWidget(closeBtn, 0, Qt::AlignTop);
    }
    const QList<QAbstractButton *> chromeButtons{minBtn, maxBtn, closeBtn};
    for (QAbstractButton *b : chromeButtons)
        FluentStyleSheet::apply(b, FluentStyleSheet::FLUENT_WINDOW);

    // ── 三键 = 用户定义的语义(docs/13-窗口行为.md)────────────────────────────
    // 三个都**不**再直连 QWidget 的默认动作,一律走本类的方法 —— 窗口状态只有一个地方改
    // (托盘与自检都调同一批方法,不会出现"谁把窗口藏了/谁又把它显示出来"互相覆盖):
    //   最小化键 -> hideToTray()     隐藏窗口;进程与后台 worker 照跑(不是 showMinimized)
    //   最大化键 -> toggleMaximize() 最大化 + 申请置顶;再点 = 还原并取消置顶
    //   关闭键   -> requestClose()   真关闭:先把任务状态落盘再退出
    // **视觉一个字节没动**:按钮仍是上面那三个 qf 几何的类(46x32 / 图标位置 / 悬停色),
    // 这里换的只是"点了之后发生什么"。
    connect(minBtn, &QAbstractButton::clicked, this, &MainWindow::hideToTray);
    connect(maxBtn, &QAbstractButton::clicked, this, &MainWindow::toggleMaximize);
    connect(closeBtn, &QAbstractButton::clicked, this, &MainWindow::requestClose);

    // 标题栏自己的三个信号也要接上:双击标题栏(FluentTitleBar::mouseDoubleClickEvent)
    // 发的就是 maximizeRequested —— 走这条路的"最大化"必须和点按钮完全同语义。
    // libqf 原来把这三个信号接给 QWidget 的默认槽(那是它自己 FluentWindow 形态的接法),
    // 我们的窗口不用那个形态,所以显式接一遍。
    connect(m_titleBar, &FluentTitleBar::minimizeRequested, this, &MainWindow::hideToTray);
    connect(m_titleBar, &FluentTitleBar::maximizeRequested, this, &MainWindow::toggleMaximize);
    connect(m_titleBar, &FluentTitleBar::closeRequested, this, &MainWindow::requestClose);

    // ---- 小窗口(「缩成小窗口」)的内容条 ----
    // 它是**窗口**的子控件,不是内容栈里的页面:小窗口形态下导航面板与内容栈整个藏起来,
    // 只留标题栏(三键照旧)和这一条(当前任务/进度 + 恢复键)。绝对定位,不进根布局 ——
    // 根布局是 qf 的 1:1 摆法(导航 + 内容列),不动它一个小数点。
    buildMiniPanel();

    // ---- 六个页面(占位:只证明路由与栈是对的) ----
    for (const NavItem &item : kNavSpec) {
        QWidget *page = createPageForRoute(item.routeKey, nullptr);
        if (!page)
            page = makePlaceholderPage(item);
        m_pages.insert(item.routeKey, page);
        m_stack->addWidget(page);
        m_nav->addItem(item);
    }
    connect(m_nav, &NavPanel::routeChanged, this, &MainWindow::switchToRoute);
    switchToRoute(QStringLiteral("home"));


    layoutTitleBar();
}

void MainWindow::layoutTitleBar() {
    if (!m_titleBar)
        return;
    // qf fluent_window.py:344-346 resizeEvent:
    //   self.titleBar.move(46, 0); self.titleBar.resize(self.width() - 46, self.titleBar.height())
    //
    // 小窗口形态是唯一的例外:导航面板整块藏起来了,x<46 那一段没有东西可让位,
    // 标题栏因此铺满整宽(否则小窗口左边会空出 46px 的洞)。**只有几何变了** ——
    // 三键的尺寸/图标几何/悬停色仍是 qf 原样(上面 ShellMinimize/Maximize/Close 三个类),
    // 参考图那套 1:1 像素对齐走的是正常/最大化形态,不受这里影响。
    const int left = (m_mode == WindowMode::Mini) ? 0 : kTitleBarLeft;
    m_titleBar->setGeometry(left, 0, qMax(0, width() - left), kTitleBarHeight);
    m_titleBar->raise(); // qf: self.titleBar.raise_()(Qt6 里就是 QWidget::raise)
}

QWidget *MainWindow::makePlaceholderPage(const NavItem &item) {
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("sxclPage_") + item.routeKey);
    // 页面**透明**:底色由内容框/页面自己负责(QSS 的 StackedWidget 底 + 页面卡片)
    page->setAttribute(Qt::WA_StyledBackground, true);

    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(6);

    auto *title = new QLabel(item.title, page);
    title->setObjectName(QStringLiteral("sxclPageTitle"));
    QFont tf = title->font();
    tf.setPixelSize(28); // qf TitleLabel = 28px / 600
    tf.setWeight(QFont::DemiBold);
    title->setFont(tf);

    auto *hint = new QLabel(QStringLiteral("本页内容尚未移植(见 docs/05-UI-1to1规格.md)"), page);
    hint->setObjectName(QStringLiteral("sxclPageHint"));
    hint->setWordWrap(true);

    lay->addWidget(title);
    lay->addWidget(hint);
    lay->addStretch(1);
    return page;
}

const QVector<NavItem> &MainWindow::navItems() const { return m_nav->items(); }

QString MainWindow::currentRouteKey() const { return m_nav->currentRouteKey(); }

void MainWindow::switchToRoute(const QString &routeKey) {
    // ---- 验收/自检通路:三个临时页不在导航里,但要能像常驻页一样被 SXCL_UI_ROUTE 直接打开 ----
    // (main.cpp:60-63 只做 window.switchToRoute(SXCL_UI_ROUTE);临时页的创建参数需要版本 id,
    //  所以这里用 SXCL_UI_VERSION 传,默认值就是参考图抓取脚本用的那个 1.21.11。)
    if (routeKey == QLatin1String("download_config") ||
        routeKey == QLatin1String("download_progress") || routeKey == QLatin1String("launch")) {
        const QString versionId =
            qEnvironmentVariable("SXCL_UI_VERSION", QStringLiteral("1.21.11"));
        if (routeKey == QLatin1String("download_config"))
            switchToDownloadConfig(versionId);
        else if (routeKey == QLatin1String("download_progress"))
            switchToDownloadProgress(versionId, versionId);
        else
            switchToLaunch(versionId);
        return;
    }

    QWidget *page = m_pages.value(routeKey, nullptr);
    if (!page)
        return;

    // Python main_window.py:127-147 _onCurrentInterfaceChanged(挂在 FluentWindow 的
    // stackedWidget.currentChanged 上):切到"版本"且会话没结束时,**恢复活动临时页**并把
    // 导航选中态清空(None)。用户从下载/启动页点侧栏"版本"回到的就是那一页,不是版本列表。
    if (routeKey == QLatin1String("versions") && m_sessionActive && m_activeTempPage) {
        m_stack->setCurrentWidget(m_activeTempPage);
        m_nav->setCurrent(QString());
        return;
    }

    m_lastNavItem = routeKey; // main_window.py:147
    m_stack->setCurrentWidget(page);
    m_nav->setCurrent(routeKey);
}

// ─────────────────── 临时页机制(main_window.py:153-283)───────────────────

void MainWindow::showTempPage(QWidget *page, const QString &key) { // :153-166
    if (!page)
        return;
    // 换页时把上一个临时页从内容栈摘下来(**不销毁**:它还在会话池里,再进来状态照旧)
    if (m_activeTempPage && m_activeTempPage != page) {
        m_activeTempPage->setParent(nullptr);
        m_stack->removeWidget(m_activeTempPage);
    }
    if (m_stack->indexOf(page) < 0)
        m_stack->addWidget(page); // :159-160

    m_activeTempPage = page;
    m_tempPageKey = key;
    m_sessionActive = true;
    m_stack->setCurrentWidget(page);
    m_nav->setCurrent(QString()); // setCurrentItem(None):临时页不选中任何导航项
}

void MainWindow::hideTempPage(bool endSession) { // :168-182
    if (!m_activeTempPage)
        return;
    if (endSession)
        m_sessionActive = false;

    // Python: target = self._last_nav_item or "versions";在会话池里找这个名字的页面
    const QString target =
        m_lastNavItem.isEmpty() ? QStringLiteral("versions") : m_lastNavItem;
    if (m_pages.contains(target)) { // 会话池里有这个名字的页面 -> 回它
        switchToRoute(target);
        return;
    }
    switchToRoute(QStringLiteral("versions")); // 兜底:回版本列表页
}

void MainWindow::registerSessionPage(const QString &key, QWidget *page) { // :245-264
    m_pages.insert(key, page);
    // 常驻页不参与淘汰
    static const QSet<QString> persistent{QStringLiteral("home"), QStringLiteral("versions"),
                                          QStringLiteral("tasks"), QStringLiteral("settings")};
    if (m_pages.size() <= kMaxSessionPages)
        return;
    for (auto it = m_pages.begin(); it != m_pages.end(); ++it) {
        if (persistent.contains(it.key()) || it.key() == key)
            continue;
        QWidget *stale = it.value();
        m_pages.erase(it);
        if (m_activeTempPage == stale)
            m_activeTempPage = nullptr;
        m_stack->removeWidget(stale);
        stale->setParent(nullptr);
        stale->deleteLater();
        break; // Python 也只淘汰一个
    }
}

void MainWindow::addOrUpdateTask(const QString &taskId, const QString &title,
                                 const QString &status) {
    // Python: self.tasks_page.add_or_update_task(...)(main_window.py:220-223,238-241)。
    // 任务页的登记 API 是 Q_INVOKABLE(tasks_page.cpp:333),用 invokeMethod 调,
    // 免得主窗口为了一个调用把 TasksPage 的私有类型拖进头文件。
    // **先**记进本窗口的任务登记表:任务状态落盘(见 persistTaskState)靠它,
    // 和任务页在不在无关 —— 任务页没了不代表这个任务没在跑。
    TaskRecord record;
    record.id = taskId;
    record.title = title;
    record.status = status;
    rememberTask(record);

    QWidget *tasks = m_pages.value(QStringLiteral("tasks"), nullptr);
    if (!tasks)
        return;
    QMetaObject::invokeMethod(tasks, "addOrUpdateTask", Qt::DirectConnection,
                              Q_ARG(QString, taskId), Q_ARG(QString, title), Q_ARG(int, 0),
                              Q_ARG(QString, status), Q_ARG(QString, QString()));
}

void MainWindow::switchToDownloadConfig(const QString &versionId) { // :188-199
    const QString key = QStringLiteral("download_config_") + versionId;
    QWidget *page = m_pages.value(key, nullptr);
    if (!page) {
        page = createDownloadConfigPage(VersionRef{versionId}, this);
        registerSessionPage(key, page);
    }
    showTempPage(page, key);
}

void MainWindow::switchToDownloadProgress(const QString &versionId, const QString &versionName,
                                          const QString &loaderType,
                                          const QString &loaderVersion) { // :201-225
    const QString key = QStringLiteral("download_progress_") + versionName;
    QWidget *page = m_pages.value(key, nullptr);
    if (!page) {
        page = createDownloadProgressPage(VersionRef{versionId}, versionName, loaderType,
                                          loaderVersion, this);
        registerSessionPage(key, page);
    }
    // 在任务页登记(:220-223)
    addOrUpdateTask(key, QStringLiteral("下载 %1").arg(versionName),
                    QStringLiteral("准备中"));
    // 恢复信息(版本 id / 版本名 / 加载器)只有这里知道,补进登记表 ——
    // 退出时落盘靠它,下次启动才"能恢复"(见 loadTaskState / docs/13 §4)。
    TaskRecord resume;
    resume.id = key;
    resume.kind = QStringLiteral("download");
    resume.versionId = versionId;
    resume.versionName = versionName;
    resume.loaderType = loaderType;
    resume.loaderVersion = loaderVersion;
    rememberTask(resume);
    showTempPage(page, key);
}

void MainWindow::switchToLaunch(const QString &versionId) { // :227-243
    const QString key = QStringLiteral("launch_") + versionId;
    QWidget *page = m_pages.value(key, nullptr);
    if (!page) {
        page = createLaunchPage(VersionRef{versionId}, this);
        registerSessionPage(key, page);
    }
    addOrUpdateTask(key, QStringLiteral("启动 %1").arg(versionId), QStringLiteral("启动中"));
    TaskRecord resume;
    resume.id = key;
    resume.kind = QStringLiteral("launch");
    resume.versionId = versionId;
    resume.versionName = versionId;
    rememberTask(resume);
    showTempPage(page, key);
}

void MainWindow::goBackToVersions() { // :266-273
    // 返回**版本列表页**并结束会话:下载/安装/启动都发生在版本页,回它就对了
    m_lastNavItem = QStringLiteral("versions");
    hideTempPage(true);
}

void MainWindow::goBackFromLaunch() { // :275-277
    hideTempPage(true);
}

void MainWindow::navigateToTask(const QString &taskId) { // :279-283
    if (QWidget *page = m_pages.value(taskId, nullptr))
        showTempPage(page, QString());
}

QStringList MainWindow::sessionPageKeys() const { return m_pages.keys(); }

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
#ifdef Q_OS_WIN
    // 边缘缩放命中:按参考实现 qframelesswindow/windows/__init__.py:22,111-145 覆写。
    //   * qf 用 ScreenToClient + GetClientRect 的**物理像素**判定,带宽 = BORDER_WIDTH = 5;
    //     角优先,其次上/下,再次左/右;最大化/全屏时 bw = 0(不接管)。
    //   * libqf 的 FluentWindowBase::nativeEvent(fluent_window.cpp:133-176)把
    //     FluentWindow::kWinMargin=30(fluent_window.h:221)整条阴影带当 resize 热区,
    //     那是给它自己的 FluentWindow 形态用的(窗口内留 30px 阴影带,:599)。
    //     我们的窗口内容贴边(根布局边距 0),30 物理像素 = 逻辑 20px 会把
    //     导航面板左侧 42% 的宽度、标题栏上半部整片变成 resize 热区 → 用户点不到汉堡/拖动不了标题栏。
    // 注意:不在命中带内时**不能**退回基类(基类的 30px 带会命中),要直接返回 false 交给系统默认(HTCLIENT)。
    MSG *msg = static_cast<MSG *>(message);
    if (msg->message != WM_NCHITTEST || isMaximized() || isFullScreen())
        return FluentWindowBase::nativeEvent(eventType, message, result);

    HWND hwnd = reinterpret_cast<HWND>(winId());
    POINT pt{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
    if (!::ScreenToClient(hwnd, &pt))
        return false;
    RECT cr{};
    if (!::GetClientRect(hwnd, &cr))
        return false;
    const int w = int(cr.right - cr.left), h = int(cr.bottom - cr.top);
    const int x = int(pt.x), y = int(pt.y);
    const bool lx = x < kResizeBandPx;
    const bool rx = x > w - kResizeBandPx;
    const bool ty = y < kResizeBandPx;
    const bool by = y > h - kResizeBandPx;
    if (lx && ty) {
        *result = HTTOPLEFT;
        return true;
    }
    if (rx && by) {
        *result = HTBOTTOMRIGHT;
        return true;
    }
    if (rx && ty) {
        *result = HTTOPRIGHT;
        return true;
    }
    if (lx && by) {
        *result = HTBOTTOMLEFT;
        return true;
    }
    if (ty) {
        *result = HTTOP;
        return true;
    }
    if (by) {
        *result = HTBOTTOM;
        return true;
    }
    if (lx) {
        *result = HTLEFT;
        return true;
    }
    if (rx) {
        *result = HTRIGHT;
        return true;
    }
    return false;
#else
    return FluentWindowBase::nativeEvent(eventType, message, result);
#endif
}

void MainWindow::paintEvent(QPaintEvent *) {
    // qf FluentWidget.paintEvent(fluent_window.py:66-71):整窗铺一层后台色
    // (SXCL 侧:main_window.py:298 FluentWindow { background-color: token(bg) } = #202020)
    QPainter p(this);
    p.fillRect(rect(), ThemeBridge::instance().token(QStringLiteral("bg")));
}

void MainWindow::changeEvent(QEvent *e) {
    FluentWindowBase::changeEvent(e);
    // 最大化/还原时三键的图标要跟着变(qf 的 TitleBar.eventFilter 也是在 WindowStateChange
    // 时刷 maxBtn 的图标;我们的最大化键在绘制时读 window()->isMaximized(),所以要主动重绘)。
    if (e->type() == QEvent::WindowStateChange && m_titleBar) {
        const QList<QAbstractButton *> buttons = m_titleBar->findChildren<QAbstractButton *>();
        for (QAbstractButton *b : buttons)
            b->update();
    }
}

void MainWindow::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    layoutTitleBar();
    layoutMiniPanel(); // 小窗口面板贴在标题栏下面,跟着窗口一起变
    // qf navigation_panel.py:722-725:窗口宽度掉到 minimumExpandWidth(1008)以下时,
    // 展开态的导航自动收起(EXPAND 模式只在够宽时成立)。
    // 小窗口形态下导航面板整个是藏起来的,没有"展开态"要收 —— 提前返回省一次动画。
    if (m_mode == WindowMode::Mini)
        return;
    if (m_nav && width() < NavPanel::kMinimumExpandWidth && !m_nav->collapsed())
        m_nav->setCollapsed(true);
}

} // namespace sxcl::ui
