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
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
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
    // 窗口边缘的缩放命中带(**物理像素**):
    //   qframelesswindow/windows/__init__.py:22  BORDER_WIDTH = 5
    //   :111-145 WM_NCHITTEST 里用 ScreenToClient + GetClientRect 的物理像素比较,
    //            x < 5 左边 / x > w-5 右边 / y < 5 上边 / y > h-5 下边,角优先,最大化时 0。
    // libqf 的 FluentWindowBase 用的是它自己 30px 阴影带(fluent_window.cpp:152,599),
    // 我们的窗口内没有阴影带,所以按参考实现覆写命中(见 .cpp nativeEvent)。
    static constexpr int kResizeBandPx = 5;

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // ---- 会话保持的临时页(等价物见 Python src/app/main_window.py:153-286)----
    // 三个临时页(下载配置 / 下载进度 / 启动进度)不在导航里:它们由下面的 switch_to_* 按需
    // 创建,创建后一直挂在内容栈里(键见 _register_page 的 key 规则),再进来时不重建,
    // 页面状态因此跨导航保持。
    Q_INVOKABLE void switchToDownloadConfig(const QString &versionId);
    Q_INVOKABLE void switchToDownloadProgress(const QString &versionId, const QString &versionName,
                                              const QString &loaderType = QStringLiteral("none"),
                                              const QString &loaderVersion = QString());
    Q_INVOKABLE void switchToLaunch(const QString &versionId);
    // 临时页自己的"返回"入口:回**版本列表页**并结束会话(main_window.py:266-277)
    Q_INVOKABLE void goBackToVersions();
    Q_INVOKABLE void goBackFromLaunch();
    // 任务页点任务卡片时的回调(main_window.py:279-283;tasks_page.cpp:419 调的)
    Q_INVOKABLE void navigateToTask(const QString &taskId);

    NavPanel *navPanel() const { return m_nav; }
    QStackedWidget *pageStack() const { return m_stack; }
    const QVector<NavItem> &navItems() const;
    QString currentRouteKey() const;

    // 会话池里已登记的键(自检/报告用)
    QStringList sessionPageKeys() const;
    QWidget *sessionPage(const QString &key) const { return m_pages.value(key, nullptr); }

    void switchToRoute(const QString &routeKey);

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void changeEvent(QEvent *) override;

private:
    void buildUi();
    void layoutTitleBar();
    QWidget *makePlaceholderPage(const NavItem &item);

    // ---- 临时页机制(main_window.py:153-264)----
    void showTempPage(QWidget *page, const QString &key);   // _show_temp_page
    void hideTempPage(bool endSession);                     // _hide_temp_page
    void registerSessionPage(const QString &key, QWidget *page); // _register_page
    void addOrUpdateTask(const QString &taskId, const QString &title, const QString &status);

    // _session_pages 超过这个数就淘汰一个临时页(以前只增不减,长时间用会一直涨内存)
    static constexpr int kMaxSessionPages = 24;             // main_window.py:249

    FluentTitleBar *m_titleBar = nullptr;
    QLabel *m_iconLabel = nullptr; // qf 的 18x18 窗口图标位(未设窗口图标 -> 空白占位)
    NavPanel *m_nav = nullptr;
    StackedWidget *m_stack = nullptr; // libqf 的 StackedWidget(类名要能被 QSS 命中,见 .cpp)
    // 会话页面池(main_window.py:59 self._session_pages):6 个常驻页 + 全部临时页
    QHash<QString, QWidget *> m_pages;

    // main_window.py:61-65
    QWidget *m_activeTempPage = nullptr; // _active_temp_page
    QString m_tempPageKey;               // _temp_page_key
    bool m_sessionActive = false;        // _session_active
    QString m_lastNavItem;               // _last_nav_item
};

} // namespace sxcl::ui
