#include "main_window.h"

#include <QEvent>
#include <QFont>
#include <QLabel>
#include <QPainter>
#include <QShowEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "theme_bridge.h"

namespace sxcl::ui {

const char *MainWindow::kTitle = "Silent X Craft Launcher";

namespace {

// 导航项(顺序即界面顺序):主页 / 下载 / 任务 / 联机 / 更多,设置单独在底部
const NavItem kNavSpec[] = {
    {QStringLiteral("home"), IconRegistry::Home, QStringLiteral("主页"), false},
    {QStringLiteral("download"), IconRegistry::Download, QStringLiteral("下载"), false},
    {QStringLiteral("tasks"), IconRegistry::Tasks, QStringLiteral("任务"), false},
    {QStringLiteral("multiplayer"), IconRegistry::Multiplayer, QStringLiteral("联机"), false},
    {QStringLiteral("more"), IconRegistry::More, QStringLiteral("更多"), false},
    {QStringLiteral("settings"), IconRegistry::Settings, QStringLiteral("设置"), true},
};

} // namespace

MainWindow::MainWindow(QWidget *parent)
    // 传 true = "合成可用时按 libqf 的规格留阴影边距";offscreen 下 libqf 自己会
    // 回落(acrylicEnabled() 变 false),留白随之变 0,截图因此是确定的。
    : FluentWindowBase(true, parent) {
    buildUi();
    ThemeBridge::instance().attach(this);
}

MainWindow::~MainWindow() { ThemeBridge::instance().detach(this); }

void MainWindow::buildUi() {
    setObjectName(QStringLiteral("sxclRoot"));
    setWindowTitle(QString::fromUtf8(kTitle));
    resize(kInitialWidth, kInitialHeight);
    setMinimumSize(kMinimumWidth, kMinimumHeight);

    m_outer = new QVBoxLayout(this);
    m_outer->setContentsMargins(0, 0, 0, 0);
    m_outer->setSpacing(0);

    // ---- 标题栏(libqf) ----
    m_titleBar = new FluentTitleBar(this);
    m_titleBar->titleLabel()->setText(QString::fromUtf8(kTitle));
    m_outer->addWidget(m_titleBar);
    connect(m_titleBar, &FluentTitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(m_titleBar, &FluentTitleBar::maximizeRequested, this,
            [this] { isMaximized() ? showNormal() : showMaximized(); });
    connect(m_titleBar, &FluentTitleBar::closeRequested, this, &QWidget::close);
    connect(m_titleBar, &FluentTitleBar::navMenuRequested, this,
            [this] { m_nav->setCollapsed(!m_nav->collapsed()); });

    // ---- 内容行:左侧导航 + 右侧页面栈 ----
    auto *content = new QWidget(this);
    content->setObjectName(QStringLiteral("sxclContent"));
    auto *row = new QHBoxLayout(content);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    m_nav = new NavPanel(content);
    row->addWidget(m_nav);

    auto *body = new QWidget(content);
    auto *bodyLay = new QVBoxLayout(body);
    bodyLay->setContentsMargins(12, 12, 12, 12);
    bodyLay->setSpacing(0);
    m_stack = new QStackedWidget(body);
    m_stack->setObjectName(QStringLiteral("sxclStack"));
    bodyLay->addWidget(m_stack);
    row->addWidget(body, 1);
    m_outer->addWidget(content, 1);

    // ---- 六个页面(占位:只证明路由与栈是对的) ----
    for (const NavItem &item : kNavSpec) {
        QWidget *page = makePlaceholderPage(item);
        m_pages.insert(item.routeKey, page);
        m_stack->addWidget(page);
        m_nav->addItem(item);
    }
    connect(m_nav, &NavPanel::routeChanged, this, &MainWindow::switchToRoute);
    switchToRoute(QStringLiteral("home"));

    updateChromeMargins();
}

QWidget *MainWindow::makePlaceholderPage(const NavItem &item) {
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("sxclPage_") + item.routeKey);
    // 页面底色交给 ThemeBridge 的令牌(QSS 底色需要这个属性才会真的画出来)
    page->setAttribute(Qt::WA_StyledBackground, true);
    page->setProperty("sxclCard", true);

    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(28, 24, 28, 24);
    lay->setSpacing(6);

    auto *title = new QLabel(item.title, page);
    title->setObjectName(QStringLiteral("sxclPageTitle"));
    QFont tf = title->font();
    tf.setPixelSize(26);
    tf.setBold(true);
    title->setFont(tf);

    auto *hint = new QLabel(
        QStringLiteral("导航骨架已就位(阶段 6)。本页内容在后续批次实现。"), page);
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
    QWidget *page = m_pages.value(routeKey, nullptr);
    if (!page)
        return;
    m_stack->setCurrentWidget(page);
    m_nav->setCurrent(routeKey);
}

void MainWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor bg = ThemeBridge::instance().token(QStringLiteral("bg"));
    if (m_shadowMargin <= 0) {
        p.fillRect(rect(), bg); // 离屏/无合成:整窗纯色,截图确定
        return;
    }
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(rect()).adjusted(m_shadowMargin, m_shadowMargin,
                                              -m_shadowMargin, -m_shadowMargin),
                      10, 10);
}

void MainWindow::showEvent(QShowEvent *e) {
    FluentWindowBase::showEvent(e);
    updateChromeMargins();
}

void MainWindow::changeEvent(QEvent *e) {
    if (e->type() == QEvent::WindowStateChange)
        updateChromeMargins();
    FluentWindowBase::changeEvent(e);
}

void MainWindow::updateChromeMargins() {
    // libqf 的无边框窗口把窗口边缘 30px 当作(透明)阴影/缩放的预留带;
    // 合成不可用(offscreen)时它不留边,我们跟着留 0,保证尺寸断言与截图确定。
    const bool compositing = acrylicEnabled();
    const int margin = (compositing && !isMaximized() && !isFullScreen())
                           ? FluentWindow::kWinMargin
                           : 0;
    if (margin == m_shadowMargin)
        return;
    m_shadowMargin = margin;
    if (m_outer)
        m_outer->setContentsMargins(margin, margin, margin, margin);
    update();
}

} // namespace sxcl::ui
