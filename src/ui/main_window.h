#pragma once
// main_window — 主窗口(阶段 6 地基)
//
// 只做"壳":标题栏 + 左侧主导航 + 右侧页面栈。页面内容是下一批的事,
// 这里每页只放一个占位标题,用来证明导航切换真的把栈切过去了。
//
// 窗口骨架直接用 libqf 的 FluentWindowBase(无边框 + 原生边缘缩放命中)
// 与 FluentTitleBar(拖动/双击最大化/最小化/关闭/汉堡按钮),不自己造窗口类。
// 底色与留白来自 ThemeBridge 的主题令牌。
#include <QHash>
#include <QString>
#include <QVector>

#include "libqf.h"

#include "nav.h"

class QEvent;
class QShowEvent;
class QStackedWidget;
class QVBoxLayout;

namespace sxcl::ui {

class MainWindow : public FluentWindowBase {
    Q_OBJECT
public:
    // 标题与尺寸参考 Python 版 src/app/main_window.py(1100x750 / 最小 900x600)
    static const char *kTitle;
    static constexpr int kInitialWidth = 1100;
    static constexpr int kInitialHeight = 750;
    static constexpr int kMinimumWidth = 900;
    static constexpr int kMinimumHeight = 600;

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    NavPanel *navPanel() const { return m_nav; }
    QStackedWidget *pageStack() const { return m_stack; }
    const QVector<NavItem> &navItems() const;
    QString currentRouteKey() const;

    void switchToRoute(const QString &routeKey);

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void changeEvent(QEvent *) override;

private:
    void buildUi();
    void updateChromeMargins();
    QWidget *makePlaceholderPage(const NavItem &item);

    QVBoxLayout *m_outer = nullptr;
    FluentTitleBar *m_titleBar = nullptr;
    NavPanel *m_nav = nullptr;
    QStackedWidget *m_stack = nullptr;
    QHash<QString, QWidget *> m_pages;
    int m_shadowMargin = 0;
};

} // namespace sxcl::ui
