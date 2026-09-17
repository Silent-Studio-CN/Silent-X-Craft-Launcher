#include "nav.h"

#include <QPainter>
#include <QVBoxLayout>

#include "theme_bridge.h"

namespace sxcl::ui {

// ---------------------------------------------------------------- NavButton

NavButton::NavButton(const QIcon &icon, const QString &text, QWidget *parent)
    : NavigationPushButton(icon, text, true, parent) {
    setFixedHeight(40);
}

QRect NavButton::indicatorRect() const {
    // 左缘 3px 指示条(选中时才画;基类 paintEvent 负责绘制与配色)
    const int h = qMin(18, qMax(8, height() - 12));
    return QRect(0, (height() - h) / 2, 3, h);
}

void NavButton::paintBackground(QPainter &p) {
    const ThemeBridge &tb = ThemeBridge::instance();
    QColor bg;
    if (isSelected()) {
        // 选中:强调色低透明度底 —— 强调色来自 libqf 主题单例,不写死
        const QColor a = tb.accent();
        bg = QColor(a.red(), a.green(), a.blue(), tb.isDark() ? 66 : 34);
    } else if (m_pressed) {
        bg = tb.token(QStringLiteral("hoverStrong"));
    } else if (m_hover) {
        bg = tb.token(QStringLiteral("hover"));
    } else {
        return;
    }
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(rect()).adjusted(4, 1, -4, -1), 5, 5);
}

// ---------------------------------------------------------------- NavPanel

NavPanel::NavPanel(QWidget *parent) : QFrame(parent) {
    setObjectName(QStringLiteral("sxclNav"));
    setFixedWidth(kExpandedWidth);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 8, 0, 8);
    lay->setSpacing(0);

    m_top = new QVBoxLayout;
    m_top->setContentsMargins(0, 0, 0, 0);
    m_top->setSpacing(2);
    lay->addLayout(m_top);
    lay->addStretch(1);

    m_separator = new NavigationSeparator(this);
    m_separator->hide(); // 有底部项时才露出来
    lay->addWidget(m_separator);

    m_bottom = new QVBoxLayout;
    m_bottom->setContentsMargins(0, 0, 0, 0);
    m_bottom->setSpacing(2);
    lay->addLayout(m_bottom);
}

NavigationPushButton *NavPanel::addItem(const NavItem &item) {
    IconRegistry &icons = IconRegistry::instance();
    auto *btn = new NavButton(icons.themedIcon(item.icon), item.title, this);
    btn->setObjectName(QStringLiteral("nav_") + item.routeKey);
    const QColor accent = ThemeBridge::instance().accent();
    btn->setIndicatorColor(accent, accent); // 指示条跟随 libqf 主题色
    (item.bottom ? m_bottom : m_top)->addWidget(btn);
    if (item.bottom) {
        m_hasBottom = true;
        m_separator->setVisible(!m_collapsed);
    }
    const int id = m_items.size();
    m_items.push_back(item);
    m_buttons.insert(item.routeKey, btn);
    connect(btn, &NavigationWidget::clicked, this, [this, id](bool) {
        const QString key = m_items[id].routeKey;
        setCurrent(key);
        emit routeChanged(key);
    });
    return btn;
}

void NavPanel::setCurrent(const QString &routeKey) {
    if (m_current == routeKey)
        return;
    m_current = routeKey;
    for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it)
        it.value()->setSelected(it.key() == routeKey);
}

NavigationPushButton *NavPanel::button(const QString &routeKey) const {
    return m_buttons.value(routeKey, nullptr);
}

void NavPanel::setCollapsed(bool collapsed) {
    if (m_collapsed == collapsed)
        return;
    m_collapsed = collapsed;
    setFixedWidth(collapsed ? kCollapsedWidth : kExpandedWidth);
    for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it) {
        NavigationPushButton *b = it.value();
        b->setCompacted(collapsed);
        b->setFixedHeight(collapsed ? 48 : 40);
        b->setToolTip(collapsed ? b->text() : QString());
    }
    // 折叠态下分隔线跟着收起(顶部组/底部组的间距由弹性项保证)
    m_separator->setVisible(!collapsed && m_hasBottom);
    update();
}

void NavPanel::paintEvent(QPaintEvent *) {
    // 面板底色走主题令牌(libqf FluentBackgroundTheme 的 DefaultBlue 画布)
    QPainter p(this);
    p.fillRect(rect(), ThemeBridge::instance().token(QStringLiteral("bgNav")));
}

} // namespace sxcl::ui
