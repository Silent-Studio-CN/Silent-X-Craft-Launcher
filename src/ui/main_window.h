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
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include "libqf.h"

#include "nav.h"

class QCloseEvent;
class QEvent;
class QHideEvent;
class QLabel;
class QProgressBar;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QTimer;

namespace sxcl::ui {

class SxclTray;

// 窗口形态(用户对三键的语义要求)。
//
// **新增设计**,不是移植:Python 版 src/app/main_window.py 的窗口部分只有
// "最小化到任务栏 / 关闭即退出",既没有置顶语义、也没有小窗口形态、更没有托盘。
// 用户这次明确给了规则,所以这一套是按规则新写的,详见 docs/13-窗口行为.md。
//
//   Normal     普通窗口:可缩放、**不置顶**
//   Maximized  最大化 + 申请"悬于其他应用窗口之上"(Win32 WS_EX_TOPMOST)
//   Mini       小窗口:缩到 kMiniWidth x kMiniHeight
//
// 「隐藏」**不在**这个枚举里 —— 它和形态正交:最小化 = 隐藏窗口(进程与后台 worker 照跑),
// 一个隐藏的窗口仍然可以是 Normal/Maximized/Mini 中的任何一种,回来时照原样恢复。
enum class WindowMode { Normal, Maximized, Mini };

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

    // ═══════════════ 窗口行为(用户定义的语义;**新增设计**)═══════════════
    // 四条规则的实现都收在本类里:窗口状态只有一个地方改(tray 只发信号、nav 不参与),
    // 否则"谁把窗口藏了/谁又把它显示出来"会变成三处互相覆盖。

    // 关闭 = **真关闭**:退出、释放资源。有在跑的任务时先把任务状态落盘(下次启动可恢复),
    // 提示还是直接退由设置 ui.close_mode 决定(默认 "exit" = 直接退,不弹窗)。
    Q_INVOKABLE void requestClose();

    // 最大化 = 最大化 **并且**申请"悬于其他应用窗口之上"(置顶);
    // 再点一次 = 还原成普通窗口,**同时取消置顶**。
    Q_INVOKABLE void toggleMaximize();
    // 客观读数(直接问 Win32,不问 Qt 的 hint 标志):GWL_EXSTYLE & WS_EX_TOPMOST
    bool isTopMost() const;

    // 缩成小窗口 / 恢复原尺寸与位置(原 geometry 在**进小窗口之前**记下来)
    Q_INVOKABLE void setMiniMode(bool on);
    Q_INVOKABLE void toggleMiniMode();
    bool isMiniMode() const { return m_mode == WindowMode::Mini; }
    // 小窗口形态的尺寸(定义见 docs/13 §3):宽度够放下"标题栏 + 一行任务 + 进度条 + 恢复键"
    static constexpr int kMiniWidth = 420;
    static constexpr int kMiniHeight = 168;

    // 最小化 = **隐藏窗口**(不是最小化到任务栏):进程活着、后台 worker 照跑;
    // 恢复入口是任务栏托盘图标(双击 / 右键菜单「显示主窗口」)。
    Q_INVOKABLE void hideToTray();
    Q_INVOKABLE void showMainWindow();
    bool isHiddenToTray() const { return m_hiddenToTray; }

    WindowMode windowMode() const { return m_mode; }
    SxclTray *tray() const { return m_tray; }

    // ── 客观读数(自检/报告用;只读,不改任何状态)──
    // 一行把"窗口形态/可见性/置顶/geometry/进程内后台线程数"都摊开
    QString windowStateLine() const;
    // 「当前任务:<标题> <状态> <进度>%」;没有在跑的任务 = 空串
    QString currentTaskSummary() const;
    // 在跑的后台任务数(探针:会话页下还挂着正在运行的 QThread —— worker 就在其中)
    int runningTaskCount() const;

    // 取证/自检通路(SXCL_UI_WINCHECK):按脚本逐步驱动上面四条行为,每步打一行客观读数。
    // 命令见 docs/13 §5。**不是**产品功能,和 SXCL_UI_SHOT / SXCL_ANIM_TRACE 同一类通路。
    void startWindowSelfCheck(const QString &script);

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void changeEvent(QEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    // ── 窗口行为内部实现 ──
    void setWindowMode(WindowMode mode); // 形态的唯一写入口
    void applyTopMost(bool on);          // Win32 侧置顶/取消置顶
    void updateChromeForMode();          // 小窗口里藏导航与内容栈,只留标题栏 + mini 面板
    void buildMiniPanel();               // 小窗口面板(标题 + 任务/进度 + 恢复键)
    void layoutMiniPanel();
    void buildTray();                    // 托盘(没有托盘就不建,也绝不隐藏窗口)
    void syncTray();                     // 托盘菜单/小窗口面板跟着当前任务与形态走
    int stopBackgroundWork();            // 退出前显式结束还在跑的 worker 线程(返回结束了几条)
    void finishAndQuit();                // 结束 worker + 落盘 + 真退出(关闭/托盘退出的唯一出口)
    // 某个会话页下面的 worker 线程还在跑吗(不依赖 worker 的具体类型 —— 页面里
    // 正在运行的 QThread 就是"这个任务还在跑"的客观判据)
    bool taskRunning(const QString &pageKey) const;

    // ── 任务状态落盘(「默认直接退出,但要先把任务状态落盘,下次启动能恢复」)──
    struct TaskRecord {
        QString id;           // == 会话页键(download_progress_<名> / launch_<id>)
        QString kind;         // "download" / "launch"
        QString title;        // 界面上的任务名
        QString status;       // 最后一次登记的状态文字
        QString versionId;    // 恢复用:版本 id
        QString versionName;  // 恢复用:版本名(下载页的实例名)
        QString loaderType;   // 恢复用:加载器
        QString loaderVersion;
        bool running = false; // 落盘那一刻它是不是真在跑(探针,不是猜)
    };
    void rememberTask(const TaskRecord &record); // 登记/更新(addOrUpdateTask 里调)
    QString taskStateFilePath() const;
    void persistTaskState(bool interrupted);
    void loadTaskState(bool autoResume); // 读回来;autoResume = 无显示会话时不自动起下载

    // ── 自检脚本 ──
    void runSelfCheckStep();

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

    // ── 窗口行为(见本文件上方 WindowMode 与 docs/13-窗口行为.md)──
    SxclTray *m_tray = nullptr;          // 只有 SxclTray::available() 时才非空
    WindowMode m_mode = WindowMode::Normal;
    // 进小窗口**之前**的形态(小窗口 -> 恢复时要回到"最大化"还是"普通")
    WindowMode m_preMiniMode = WindowMode::Normal;
    // 进小窗口之前那一份 geometry(**记住原尺寸与位置**,恢复后必须逐值相等)
    QRect m_normalGeometry;
    bool m_haveNormalGeometry = false;
    bool m_hiddenToTray = false; // 最小化 = 隐藏(与 m_mode 正交)
    bool m_quitting = false;     // finishAndQuit 只走一次
    bool m_inShutdown = false;   // 析构/退出过程中不再写盘、不再建托盘

    QWidget *m_miniPanel = nullptr;      // 小窗口里的内容条
    QLabel *m_miniTaskLabel = nullptr;   // 「当前任务:…」
    QProgressBar *m_miniProgress = nullptr;
    QPushButton *m_miniRestoreButton = nullptr;
    QTimer *m_taskWatch = nullptr;       // 任务/进度变化的低频刷新(小窗口 + 托盘菜单)

    QVector<TaskRecord> m_tasks;         // 任务登记表(落盘与"有没有任务在跑"都靠它)

    QStringList m_checkSteps;            // SXCL_UI_WINCHECK 脚本
    int m_checkIndex = 0;
};

} // namespace sxcl::ui
