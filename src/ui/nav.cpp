#include "nav.h"

#include <QCursor>
#include <QEasingCurve>
#include <QFont>
#include <QPainter>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QVBoxLayout>

#include "fluent_theme.h"
#include "sxcl_icons.h"
#include "theme_bridge.h"

namespace sxcl::ui {

namespace {

// 图标几何:qf navigation_widget.py:204 drawIcon(self._icon, painter, QRectF(11.5+pl, 10, 16, 16))
constexpr qreal kIconLeft = 11.5;
constexpr qreal kIconTop = 10.0;
constexpr qreal kIconSide = 16.0;

// 文字几何:qf navigation_widget.py:207-214(展开态)
//   left = 44 + pl(有图标) / pl + 16(无图标)
//   drawText(QRectF(left, 0, width() - 13 - left - pr, height()), Qt.AlignVCenter, text)
constexpr qreal kTextLeftWithIcon = 44.0;
constexpr qreal kTextLeftNoIcon = 16.0;
constexpr qreal kTextRightReserve = 13.0;
constexpr int kTextPixelSize = 14; // qf setFont(self) 默认 14px(与 libqf 展开态一致)

// qf 图标名 -> svg 文本。
// qf 的 FIF.* 是 FluentIconBase,drawIcon 走 icon.render(painter, rect) = QSvgRenderer 直接
// 渲染到**非整数**矩形(common/icon.py:229-230,299-341),所以图标是亚像素定位、任意缩放下都清晰;
// 这条路径必须照抄,不能用 QIcon::paint(它会先把矩形取整,图标会整体右移 0.5 逻辑像素)。
// light 取 *_black.svg、dark 取 *_white.svg(common/icon.py:241-254 path(theme));
// 颜色就是 svg 自带的 #000000/#ffffff,不做二次着色(qf 亦然)。
QByteArray qfIconSvg(const QString &name, bool dark) {
    const QString path = QStringLiteral(":/qfluentwidgets/images/icons/%1_%2.svg")
                             .arg(name, dark ? QStringLiteral("white") : QStringLiteral("black"));
    return writeSvg(path, dark ? QColor(Qt::white) : QColor(Qt::black));
}

// 画 16x16 图标盒里的图标(盒的左上角已由调用方给出)
void paintIcon(QPainter &p, const QRectF &box, const QString &qfIconName,
               const QString &blockKind, bool dark) {
    if (!blockKind.isEmpty()) {
        // Python: grass_block_icon(24) 得到的是 **QIcon**,drawIcon 走
        // icon.paint(painter, QRectF(rect).toRect(), Qt.AlignCenter) —— 整数矩形 + 居中。
        // 取 16(逻辑)= 24 物理像素那一档,正是 Python QIcon 在 16x16@DPR1.5 下会选中的那张
        //(icons.py:203-208 的 size 列表 {24,16,48})。
        const int side = int(qRound(box.width()));
        const QPixmap pm = SxclIcons::instance().blockPixmap(blockKind, side);
        if (!pm.isNull()) {
            const QRect target = box.toRect(); // qf: QRectF(11.5,10,16,16).toRect() = QRect(12,10,16,16)
            p.drawPixmap(target, pm);
        }
        return;
    }
    if (!qfIconName.isEmpty()) {
        const QByteArray svg = qfIconSvg(qfIconName, dark);
        if (!svg.isEmpty())
            drawSvgIcon(svg, &p, box); // QSvgRenderer.render(painter, rect)
    }
}

// qf navigation_widget.py:200 / 194 的悬停判定:
//   (self.isEnter and globalRect.contains(QCursor.pos())) or self.isAboutSelected
// 悬停底色只在**指针真的在按钮上**时画(截图/自动跑时指针通常不在,所以是确定性的)。
bool pointerInside(const QWidget *w) {
    return w->rect().contains(w->mapFromGlobal(QCursor::pos()));
}

} // namespace

// ---------------------------------------------------------------- NavToolButton

NavToolButton::NavToolButton(const QString &svgName, QWidget *parent)
    : NavigationWidget(false, parent), m_svgName(svgName) {
    setFixedSize(40, 36); // qf NavigationToolButton.setCompacted 恒为 40x36
}

void NavToolButton::setSvgName(const QString &svgName) {
    m_svgName = svgName;
    update();
}

void NavToolButton::paintEvent(QPaintEvent *) {
    // qf NavigationPushButton.paintEvent(不可选中 -> 没有选中底色与指示条)+ 只画图标
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                     QPainter::SmoothPixmapTransform);
    p.setPen(Qt::NoPen);

    if (m_pressed)
        p.setOpacity(0.7);
    if (!isEnabled())
        p.setOpacity(0.4); // qf: 禁用态 40% —— 返回键就是这个状态

    const bool dark = ThemeBridge::instance().isDark();
    const int c = dark ? 255 : 0;
    if (isEnabled() && m_hover && pointerInside(this)) {
        p.setBrush(QColor(c, c, c, 10));
        p.drawRoundedRect(rect(), 5, 5);
    }
    paintIcon(p, QRectF(kIconLeft, kIconTop, kIconSide, kIconSide), m_svgName, QString(), dark);
}

// ---------------------------------------------------------------- NavButton

NavButton::NavButton(const QIcon &icon, const QString &qfIconName, const QString &blockKind,
                     const QString &text, QWidget *parent)
    : NavigationPushButton(icon, text, true, parent),
      m_qfIconName(qfIconName),
      m_blockKind(blockKind) {
    setFixedSize(40, 36); // qf navigation_widget.py:48 setFixedSize(40, 36)
}

QRect NavButton::indicatorRect() const {
    // qf navigation_widget.py:127-130 indicatorRect = QRectF(m.left(), 10, 3, 16)
    return QRect(0, 10, 3, 16);
}

void NavButton::paintEvent(QPaintEvent *) {
    // qf navigation_widget.py:176-214 逐句复刻:
    //   opacity(按下 0.7 / 禁用 0.4)-> 选中底 + 指示条 / 悬停底 -> 图标 -> 文字
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                     QPainter::SmoothPixmapTransform);
    p.setPen(Qt::NoPen);

    if (m_pressed)
        p.setOpacity(0.7);
    if (!isEnabled())
        p.setOpacity(0.4);

    const bool dark = ThemeBridge::instance().isDark();
    const int c = dark ? 255 : 0; // qf: c = 255 if isDarkTheme() else 0
    const bool hovered = isEnabled() && m_hover && pointerInside(this);

    if (isSelected()) {
        // 选中底色 = 白(深色)/黑(浅色)**10%**(悬停时 6%)——不是强调色底;
        // 参考图上这个像素是 #292929 = #202020 上叠 10% 白,逐像素对得上。
        p.setBrush(QColor(c, c, c, hovered ? 6 : 10));
        p.drawRoundedRect(rect(), 5, 5); // qf: drawRoundedRect(self.rect(), 5, 5)

        // 指示条:3x16,圆角 1.5,颜色 = 主题色(autoFallbackThemeColor 在未显式设色时取 themeColor)
        p.setBrush(ThemeBridge::instance().accent());
        p.drawRoundedRect(indicatorRect(), 1.5, 1.5);
    } else if (hovered) {
        p.setBrush(QColor(c, c, c, 10));
        p.drawRoundedRect(rect(), 5, 5);
    }

    // ---- 图标:16x16,位于 (11.5, 10) ----
    paintIcon(p, QRectF(kIconLeft, kIconTop, kIconSide, kIconSide), m_qfIconName, m_blockKind,
              dark);

    // ---- 文字:折叠态不画 ----
    if (isCompacted())
        return;
    QFont f = font();
    f.setPixelSize(kTextPixelSize);
    p.setFont(f);
    p.setPen(textColor());
    const bool hasIcon = !m_qfIconName.isEmpty() || !m_blockKind.isEmpty();
    const qreal left = hasIcon ? kTextLeftWithIcon : kTextLeftNoIcon;
    p.drawText(QRectF(left, 0, width() - kTextRightReserve - left, height()), Qt::AlignVCenter,
               text());
}

// ---------------------------------------------------------------- NavPanel

NavPanel::NavPanel(QWidget *parent) : QFrame(parent) {
    setObjectName(QStringLiteral("sxclNav")); // 类名不能叫 NavigationPanel(libqf 已占用该全局类名)
    setProperty("menu", false);               // qf navigation_panel.py:138 self.setProperty('menu', False)
    setFixedWidth(kCollapsedWidth);           // qf navigation_panel.py:108 resize(48, height())

    // 面板内容盒:
    //   navigation_panel.py:144 vBoxLayout.setContentsMargins(0, 5, 0, 5)
    //   navigation_interface.qss:8-13 NavigationPanel[menu=false] { border: 1px solid transparent; }
    //   —— QSS 的 1px border 会把布局内容再收进 1px,合起来就是 (1,6,1,6)。
    //   参考图实测:条目左缘 x=5(= 1 + 4),第一个条目上缘 y=6。
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(1, 6, 1, 6);
    lay->setSpacing(4); // navigation_panel.py:148 vBoxLayout.setSpacing(4)

    m_top = new QVBoxLayout;
    m_top->setContentsMargins(4, 0, 4, 0); // navigation_panel.py:145
    m_top->setSpacing(4);                  // navigation_panel.py:149
    m_top->setAlignment(Qt::AlignTop);     // navigation_panel.py:158
    lay->addLayout(m_top, 0);              // navigation_panel.py:153
    lay->addStretch(1);                    // 代替 qf 的 scrollArea(navigation_panel.py:154)

    m_bottom = new QVBoxLayout;
    m_bottom->setContentsMargins(4, 0, 4, 0); // navigation_panel.py:146
    m_bottom->setSpacing(4);                  // navigation_panel.py:150
    m_bottom->setAlignment(Qt::AlignBottom);  // navigation_panel.py:160
    lay->addLayout(m_bottom, 0);              // navigation_panel.py:155

    // [0] 返回键:qf navigation_panel.py:84 self.returnButton = NavigationToolButton(FIF.RETURN, self)
    //     它在 __initWidget 里被 hide()+setDisabled(True),但 NavigationInterface(...,
    //     showReturnButton=True)(navigation_interface.py:19,37,344)随后又把它 setVisible(True)
    //     —— 参考图 py_home.png 顶部 (17.3..30, 18..29.3) 那支 40% 白的左箭头就是它。
    m_returnBtn = new NavToolButton(QStringLiteral("Return"), this);
    m_returnBtn->setEnabled(false);
    m_returnBtn->setFixedSize(40, 36);
    m_returnBtn->setToolTip(QStringLiteral("返回"));

    // [1] 汉堡菜单键:navigation_panel.py:83 + :124 menuButton.clicked.connect(self.toggle)
    m_menuBtn = new NavToolButton(QStringLiteral("Menu"), this);
    m_menuBtn->setFixedSize(40, 36);
    m_menuBtn->setToolTip(QStringLiteral("打开导航"));
    connect(m_menuBtn, &NavigationWidget::clicked, this, [this](bool) {
        setCollapsed(!m_collapsed);
    });

    m_top->addWidget(m_returnBtn, 0, Qt::AlignTop); // navigation_panel.py:162
    m_top->addWidget(m_menuBtn, 0, Qt::AlignTop);   // navigation_panel.py:163
}

NavigationPushButton *NavPanel::addItem(const NavItem &item) {
    // 图标:方块图(grass_block_icon)或 qf 原生图标(FIF.*) —— 见 Python main_window.py:105-119
    QIcon icon;
    if (!item.blockKind.isEmpty()) {
        icon = SxclIcons::instance().blockIcon(item.blockKind, 24); // Python: grass_block_icon(24)
    } else if (!item.qfIcon.isEmpty()) {
        icon = fluent::icon(item.qfIcon, FluentTheme::instance().isDark()); // 深色取 _white.svg
    }
    auto *btn = new NavButton(icon, item.qfIcon, item.blockKind, item.title, this);
    btn->setObjectName(QStringLiteral("nav_") + item.routeKey);
    const QColor accent = ThemeBridge::instance().accent();
    btn->setIndicatorColor(accent, accent); // 指示条跟随 libqf 主题色
    (item.bottom ? m_bottom : m_top)->addWidget(btn, 0, item.bottom ? Qt::AlignBottom : Qt::AlignTop);
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

void NavPanel::setWidgetsCompacted(bool compacted) {
    for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it) {
        NavigationPushButton *b = it.value();
        b->setCompacted(compacted);
        // qf NavigationWidget.setCompacted:折叠 40x36,展开 EXPAND_WIDTH(312)x36
        // (navigation_widget.py:73-84;libqf 的 setCompacted 只改状态不改尺寸,这里补齐)
        b->setFixedSize(compacted ? 40 : NavigationWidget::EXPAND_WIDTH, 36);
        b->setToolTip(compacted ? b->text() : QString());
    }
    update();
}

void NavPanel::setCollapsed(bool collapsed) {
    if (m_collapsed == collapsed)
        return;
    m_collapsed = collapsed;

    // qf navigation_panel.py:502-541 / 543-563:
    //   expand():先 _setWidgetCompacted(False),再动画 48 -> 322;
    //   collapse():先动画,动画结束(_onExpandAniFinished)才 _setWidgetCompacted(True)。
    if (!collapsed)
        setWidgetsCompacted(false);
    m_menuBtn->setToolTip(collapsed ? QStringLiteral("打开导航") : QStringLiteral("关闭导航"));

    // 展开/折叠动画:QPropertyAnimation(geometry) / 150ms / OutQuad / 48 <-> 322
    //   (navigation_panel.py:95,121-122,534-538)。面板在布局里(width 固定),
    //   所以用 maximumWidth + valueChanged 里 setFixedWidth 来表达同一个 48<->322 的几何动画。
    if (!m_widthAni) {
        m_widthAni = new QPropertyAnimation(this, "maximumWidth", this);
        m_widthAni->setDuration(kExpandDurationMs);
        m_widthAni->setEasingCurve(QEasingCurve::OutQuad);
        connect(m_widthAni, &QPropertyAnimation::valueChanged, this,
                [this](const QVariant &v) { setFixedWidth(v.toInt()); });
        connect(m_widthAni, &QPropertyAnimation::finished, this, [this] {
            if (m_collapsed)
                setWidgetsCompacted(true);
        });
    }
    m_widthAni->stop();
    m_widthAni->setStartValue(width());
    m_widthAni->setEndValue(collapsed ? kCollapsedWidth : kExpandedWidth);
    m_widthAni->start();
    update();
}

} // namespace sxcl::ui
