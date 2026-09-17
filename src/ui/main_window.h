#pragma once
// main_window — 主窗口外壳(1:1 复刻 Python 版 src/app/main_window.py 的 FluentWindow 骨架)
//
// 窗口骨架来自 libqf(FluentWindowBase 无边框窗口 + FluentTitleBar 标题栏 + StackedWidget 内容框),
// 但**布局关系照 qf 的 FluentWindow** 摆(qfluentwidgets/window/fluent_window.py:255-274,344-346):
//
//   FluentWindow
//     hBoxLayout(0 边距, 0 间距)
//       [0] navigationInterface  <- 整窗高(0..H),宽 48(折叠)/322(展开);左边缘,含标题栏那一段
//       [1] widgetLayout(拉伸 1)
//             setContentsMargins(0, 48, 0, 0)   <- 顶部给标题栏让位
//               stackedWidget                    <- 内容框 (48,48)-(1100,750),占满余下空间
//     titleBar  <- **浮层**,不进布局:resizeEvent 里 move(46,0) / resize(W-46,48),然后 raise_()
//
// 所以标题栏与导航面板在 y<48 的那一段是**重叠**的:面板占 x 0..48(含标题栏高度),
// 标题栏从 x=46 起盖在上面(参考图 py_home.png 里左上是导航面板的返回键 + 汉堡键,
// 不是标题栏的按钮)。窗口**没有任何阴影留白**:qf 在 Windows 上靠 DWM 画阴影,
// 窗口内不留 30px 边距,内容才能落在 (48,48) 1052x702。
#include <QHash>
#include <QString>
#include <QVector>

#include "libqf.h"

#include "nav.h"

class QEvent;
class QLabel;
class QResizeEvent;

namespace sxcl::ui {

class MainWindow : public FluentWindowBase {
    Q_OBJECT
public:
    // 标题与尺寸参考 Python 版 src/app/main_window.py:54-56(1100x750 / 最小 900x600)
    static const char *kTitle;
    static constexpr int kInitialWidth = 1100;
    static constexpr int kInitialHeight = 750;
    static constexpr int kMinimumWidth = 900;
    static constexpr int kMinimumHeight = 600;

    // qf FluentTitleBar.__init__: self.setFixedHeight(48)
    static constexpr int kTitleBarHeight = 48;
    // qf FluentWindow.resizeEvent: self.titleBar.move(46, 0)
    static constexpr int kTitleBarLeft = 46;

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    NavPanel *navPanel() const { return m_nav; }
    QStackedWidget *pageStack() const { return m_stack; }
    const QVector<NavItem> &navItems() const;
    QString currentRouteKey() const;

    void switchToRoute(const QString &routeKey);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void changeEvent(QEvent *) override;

private:
    void buildUi();
    void layoutTitleBar();
    QWidget *makePlaceholderPage(const NavItem &item);

    FluentTitleBar *m_titleBar = nullptr;
    QLabel *m_iconLabel = nullptr; // qf 的 18x18 窗口图标位(未设窗口图标 -> 空白占位)
    NavPanel *m_nav = nullptr;
    StackedWidget *m_stack = nullptr; // libqf 的 StackedWidget(类名要能被 QSS 命中,见 .cpp)
    QHash<QString, QWidget *> m_pages;
};

} // namespace sxcl::ui
