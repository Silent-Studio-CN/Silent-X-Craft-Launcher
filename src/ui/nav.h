#pragma once
// nav — 主导航(阶段 6 地基)
//
// 结构(已定,照做):主页 / 下载 / 任务 / 联机 / 更多 五项在主导航,
// 设置单独放在导航底部。
//
// 用的是 libqf 的导航控件:条目本体是 NavigationPushButton
// (fluent_navigation.h —— 即它移植的 navigation_widget.py 那族),分隔线是
// NavigationSeparator。我们只重写两个虚函数(选中底色 + 左缘指示条),
// 悬停/点击/选中/紧凑态/文字与图标排版全部沿用库的实现。
//
// 为什么不用 FluentWindow 内建的 NavigationPanel:它按"名字"去 qrc 里取图标
// (fluent::icon(name, white) → :/qfluentwidgets/images/icons/<name>_black.svg),
// 塞不进我们自己的 PCL svg;而这批 svg 要按主题实时着色,所以条目用能接收
// QIcon 的 NavigationPushButton(见 icon_registry)。
#include <QFrame>
#include <QHash>
#include <QString>
#include <QVector>

#include "libqf.h"

#include "icon_registry.h"

class QVBoxLayout;

namespace sxcl::ui {

// 一个导航项的声明(路由键 / 语义图标 / 标题 / 是否在底部)
struct NavItem {
    QString routeKey;
    IconRegistry::Semantic icon = IconRegistry::Home;
    QString title;
    bool bottom = false;
};

// 导航条目:libqf 的 NavigationPushButton + 强调色选中态
class NavButton : public NavigationPushButton {
    Q_OBJECT
public:
    NavButton(const QIcon &icon, const QString &text, QWidget *parent = nullptr);

protected:
    QRect indicatorRect() const override;
    void paintBackground(QPainter &p) override;
};

// 导航面板:顶部组 + 弹性 + [分隔线] + 底部组
class NavPanel : public QFrame {
    Q_OBJECT
public:
    static constexpr int kExpandedWidth = 240;
    static constexpr int kCollapsedWidth = 50;

    explicit NavPanel(QWidget *parent = nullptr);

    NavigationPushButton *addItem(const NavItem &item);
    void setCurrent(const QString &routeKey);
    QString currentRouteKey() const { return m_current; }
    const QVector<NavItem> &items() const { return m_items; }
    NavigationPushButton *button(const QString &routeKey) const;

    bool collapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed); // 汉堡按钮切换;折叠后仅图标(库的 compacted 态)

signals:
    void routeChanged(const QString &routeKey);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QVBoxLayout *m_top = nullptr;
    QVBoxLayout *m_bottom = nullptr;
    NavigationSeparator *m_separator = nullptr;
    QVector<NavItem> m_items;
    QHash<QString, NavigationPushButton *> m_buttons;
    QString m_current;
    bool m_collapsed = false;
    bool m_hasBottom = false;
};

} // namespace sxcl::ui
