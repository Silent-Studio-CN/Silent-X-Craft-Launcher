/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "main_window.h"

#include <QAbstractAnimation> // 淘汰会话页前先停掉页面上的动画(见 registerSessionPage 的注释)

#include "pages/page_factory.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
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
#include "workers/install_worker.h" // 退出时主动取消安装(InstallWorker::cancel)要用它的类型
#include "workers/ui_paths.h" // uiSettingsFilePath / uiTrace(与各页同口径)

#include "sxcl/fs.h"      // sxcl_fs_remove_tree(退出时清掉本次新建的半成品实例目录)
#include "sxcl/install.h"  // 目标已存在判定(sxcl_install_target_probe/describe,与引擎同一份)
#include "sxcl/settings.h" // 读写 ui.close_mode(关闭时提示还是直接退)

// 安卓三键(最小化 = 退到后台,最大化 = 全屏 <-> 悬浮窗)的 JNI 出口在打包层的
// SxclActivity(仓库 android/java/com/silentstudio/sxcl/SxclActivity.java)。
// 调用风格与 settings_page.cpp 的 hasAllFilesAccess/requestAllFilesAccess 一致。
// 桌面工具链上没有 QJniObject,所以这个头只在安卓下包含。
#if defined(Q_OS_ANDROID)
#include <QJniObject>
#include <android/log.h> // 安卓上 stderr 不进 logcat(实测),取证行必须显式走这里
#endif

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
// **2026-09-22 用户点名重构**(docs/25 §1):主页 / 下载 / 团队 / 联机 / 更多 | 设置。
// 与 Python 版那一份(上面引的 main_window.py:105-119)**故意不同** —— 这是用户要求的偏离,
// 1:1 那套规格降级成内容参考。原"版本/任务/按键映射"三个页面不在这里了:
//   版本  -> 进"下载"的第一格(草方块)
//   任务  -> 进"更多"
//   按键映射 -> 进"更多"(用户原话:手机键位归到更多)
// 团队图标暂用 IconRegistry 的"个性化"位(docs/25 §5:语义待用户定义,定义后再换)。
const NavItem kNavSpec[] = {
    {QStringLiteral("home"), QStringLiteral("Home"), QString(), QStringLiteral("主页"), false},
    {QStringLiteral("download"), QStringLiteral("Download"), QString(), QStringLiteral("下载"), false},
    {QStringLiteral("team"), QStringLiteral("Personalize"), QString(), QStringLiteral("团队"), false},
    {QStringLiteral("multiplayer"), QStringLiteral("Globe"), QString(), QStringLiteral("联机"), false},
    {QStringLiteral("more"), QStringLiteral("More"), QString(), QStringLiteral("更多"), false},
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

// 「结束后关闭」(电脑端):安装/下载**成功**之后自动退出启动器。默认**关**。
// 判据见 notifyInstallFinished:只有成功才关 —— 失败/取消必须留在界面上。
// 与 closeNeedsConfirm 同一口径:环境变量钉死优先,其次设置文件(ui.close_after_install)。
bool closeAfterInstallEnabled() {
#if defined(Q_OS_ANDROID)
    return false; // 安卓不提供这个设置(系统回收进程,没有"退出启动器"这一说)
#else
    const QString pinned = qEnvironmentVariable("SXCL_UI_CLOSE_AFTER_INSTALL");
    if (!pinned.isEmpty())
        return pinned != QLatin1String("0");
    if (sxcl_settings *st = sxcl_settings_open(uiSettingsFilePath().toUtf8().constData())) {
        const char *value = sxcl_settings_get(st, "ui.close_after_install", "0");
        const bool on = value != nullptr && qstrcmp(value, "1") == 0;
        sxcl_settings_free(st);
        return on;
    }
    return false;
#endif
}

// ═════════ 安卓:三键的 JNI 出口(**只在 Q_OS_ANDROID 下存在**)═════════
// 桌面(Windows)那一套一个字没动:这一段在桌面构建里连符号都不生成。
//
// 三键在安卓上的语义(用户 2026-09-21 定义,**只针对安卓**;理由与实测见 docs/08):
//   最小化 -> 退出桌面、退回后台(moveTaskToBack,等同按 Home)。进程、Qt 事件循环、
//             下载/安装 worker 全部继续跑;恢复入口是底部上滑 / 最近任务。
//             安卓**没有托盘**,所以这里不走桌面那条 hideToTray 的托盘路。
//   最大化 -> 全屏 <-> 悬浮窗:C1 画中画(PiP,不需权限,显示的就是启动器界面)
//             -> C2 SYSTEM_ALERT_WINDOW 原生小面板(设备不支持 PiP 时)
//             -> C3 两条都不成时 InfoBar 人话提示(绝不静默失败)。
//   关闭   -> 真退出(与桌面同一条路:closeEvent -> finishAndQuit,未改动)。
#if defined(Q_OS_ANDROID)

constexpr const char *kAndroidActivity = "com/silentstudio/sxcl/SxclActivity";

// 取证行:安卓上 stderr 不进 logcat(main.cpp 里那条实测注释同一件事),
// 而 uiTrace() 只写 stderr —— 所以安卓分支的每一行都同时走 logcat(tag "sxcl")。
// 验收命令:adb logcat -s sxcl (见 docs/08 §16)
void androidTrace(const QString &line) {
    uiTrace(line);
    __android_log_print(ANDROID_LOG_INFO, "sxcl", "win | %s", line.toUtf8().constData());
}

bool androidMoveTaskToBack() {
    return QJniObject::callStaticMethod<jboolean>(kAndroidActivity, "moveTaskToBack", "()Z");
}
bool androidPipSupported() {
    return QJniObject::callStaticMethod<jboolean>(kAndroidActivity, "isPipSupported", "()Z");
}
bool androidInFloating() {
    return QJniObject::callStaticMethod<jboolean>(kAndroidActivity, "isInFloating", "()Z");
}
bool androidToggleFloating() {
    return QJniObject::callStaticMethod<jboolean>(kAndroidActivity, "toggleFloating", "()Z");
}
bool androidEnterFloating() {
    return QJniObject::callStaticMethod<jboolean>(kAndroidActivity, "enterFloating", "()Z");
}
bool androidHasOverlayPermission() {
    return QJniObject::callStaticMethod<jboolean>(kAndroidActivity, "hasOverlayPermission", "()Z");
}
void androidRequestOverlayPermission() {
    QJniObject::callStaticMethod<void>(kAndroidActivity, "requestOverlayPermission", "()V");
}
bool androidOverlayPanelShown() {
    return QJniObject::callStaticMethod<jboolean>(kAndroidActivity, "isOverlayPanelShown", "()Z");
}
bool androidShowOverlayPanel(const QString &task, int percent) {
    const QJniObject text = QJniObject::fromString(task);
    return QJniObject::callStaticMethod<jboolean>(kAndroidActivity, "showOverlayPanel",
                                                  "(Ljava/lang/String;I)Z", text.object<jstring>(),
                                                  jint(percent));
}

// 最大化键(安卓):全屏 -> 悬浮窗;**再点一次** -> 回到全屏。
// 已在悬浮窗里时不再往下走(C1/C2 只负责"进入")。
void sxclAndroidToggleMaximize(MainWindow *window, int percent) {
    if (window == nullptr)
        return;

    if (androidInFloating()) {
        const bool back = androidToggleFloating();
        androidTrace(QStringLiteral("安卓最大化:悬浮窗 -> 全屏(exitFloating=%1)")
                    .arg(back ? 1 : 0));
        if (!back) {
            InfoBar::push(InfoBar::Type::Info, QStringLiteral("回到全屏"),
                          QStringLiteral("点悬浮窗上的放大图标即可展开成全屏。"), window, 5000);
        }
        return;
    }

    // ---- C1:画中画(首选)------------------------------------------------
    // PiP 是安卓原生的"应用变悬浮窗浮在别的应用之上",**不需要任何权限**,
    // 显示的就是启动器自己的界面(系统把 activity 缩成一个小窗口)。
    if (androidPipSupported()) {
        if (androidEnterFloating()) {
            androidTrace(QStringLiteral(
                "安卓最大化:全屏 -> 画中画悬浮窗(enterPictureInPictureMode=1)"));
            InfoBar::push(InfoBar::Type::Info, QStringLiteral("已变成悬浮窗"),
                          QStringLiteral("启动器现在是画中画悬浮窗,浮在别的应用之上,"
                                         "点它上面的放大图标可以回到全屏。"),
                          window, 5000);
            return;
        }
        androidTrace(QStringLiteral(
            "安卓最大化:系统声明支持画中画,但 enterPictureInPictureMode 返回 false"));
    } else {
        androidTrace(QStringLiteral(
            "安卓最大化:本机/本 ROM 不支持画中画"
            "(hasSystemFeature(FEATURE_PICTURE_IN_PICTURE)=0)"));
    }

    // ---- C2:用户点名的「允许应用在其他应用上显示」(SYSTEM_ALERT_WINDOW)----
    // 硬事实(必须说清楚,不假装):Qt 的界面是 activity 自己的整块 SurfaceView,
    // **搬不进 overlay 窗口**。所以这条路只能承载**原生小面板**(当前任务/进度 +
    // 「回到启动器」按钮),承载不了启动器本体界面。
    if (!androidHasOverlayPermission()) {
        androidRequestOverlayPermission();
        androidTrace(QStringLiteral(
            "安卓最大化:画中画不可用 -> 打开「允许应用在其他应用上显示」设置页"));
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("需要「允许应用在其他应用上显示」"),
                      QStringLiteral("这台设备没能进入画中画悬浮窗。请在弹出的设置页里打开"
                                     "「允许应用在其他应用上显示」,回到启动器后再点一次最大化键,"
                                     "就会看到一个浮在别的应用上的小面板(当前任务与进度)。"),
                      window, 15000);
        return;
    }

    if (androidShowOverlayPanel(window->currentTaskSummary(), percent)) {
        androidTrace(QStringLiteral(
                    "安卓最大化:画中画不可用 -> 已显示原生悬浮面板(overlay,percent=%1)")
                    .arg(percent));
        InfoBar::push(InfoBar::Type::Info, QStringLiteral("悬浮面板已显示"),
                      QStringLiteral("小面板会浮在别的应用之上(可拖动),上面的「回到启动器」"
                                     "把启动器带回前台。"),
                      window, 6000);
        return;
    }

    // ---- C3:两条路都不成:说人话,不静默失败 ------------------------------
    androidTrace(QStringLiteral("安卓最大化:画中画与悬浮面板都失败了"));
    InfoBar::push(InfoBar::Type::Error, QStringLiteral("这台设备上没法变成悬浮窗"),
                  QStringLiteral("画中画没进去,悬浮面板也没能建起来。启动器会保持全屏运行 —— "
                                 "最小化键仍然可以把启动器退到后台,任务继续跑。"),
                  window, 12000);
}

#endif // Q_OS_ANDROID

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
    registerRail(m_nav); // 主栏也进状态机:它展开时,别的栏要收起来
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

    // ---- 侧边栏那 6 页 ----
    for (const NavItem &item : kNavSpec) {
        QWidget *page = createPageForRoute(item.routeKey, nullptr);
        if (!page)
            page = makePlaceholderPage(item);
        m_pages.insert(item.routeKey, page);
        m_stack->addWidget(page);
        m_nav->addItem(item);
    }
    // ---- 不在侧边栏、但**必须和侧边栏一样早就存在**的 3 页(2026-09-22 重构) ----
    // 版本 -> 下载第一格;任务 / 按键映射 -> 更多。
    // 为什么不能"点进去再建"(试过,踩到了):任务页是**构造期**接住"上次未完成"记录的
    // (MainWindow 在构造里读 pending_tasks.json → 登记到任务页)。按需建页会让登记发生在
    // 页面存在之前,记录就丢了(实测:任务页 0 条、卡片文案全空)。
    // 所以这里和上面一样**构造期建好**,只是不往导航里加按钮。
    static const char *const kHiddenRoutes[] = {"versions", "tasks", "keymap"};
    for (const char *route : kHiddenRoutes) {
        const QString key = QString::fromLatin1(route);
        QWidget *page = createPageForRoute(key, nullptr);
        if (!page)
            continue;
        m_pages.insert(key, page);
        m_stack->addWidget(page);
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

void MainWindow::registerRail(NavPanel *rail) {
    if (rail == nullptr) {
        return;
    }
    for (const QPointer<NavPanel> &existing : m_rails) {
        if (existing == rail) {
            return;
        }
    }
    m_rails.push_back(QPointer<NavPanel>(rail));
    /* 只认**状态变化**:谁(鼠标/键盘/程序)把它展开的都一样 —— 手点与脚本必须是一条时间线
     * (用户 2026-09-26 在 demo 上抓到:把钩子挂在某个调用入口上 -> 两条路两条时间线)。 */
    connect(rail, &NavPanel::collapsedChanged, this, [this, rail](bool collapsed) {
        if (collapsed) {
            return;
        }
        for (const QPointer<NavPanel> &other : m_rails) {
            if (other != nullptr && other != rail && !other->collapsed()) {
                other->setCollapsed(true);
            }
        }
    });
}

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
    if (!page) {
        // 界面重构(docs/25)之后有几个路由**不在侧边栏**了(版本 / 任务 / 按键映射):
        // 它们是"下载"和"更多"里的入口,第一次进来时按需建页并挂进内容栈 ——
        // 否则点"更多 → 按键映射"会什么都打不开(以前只认构造期建好的那几页)。
        page = createPageForRoute(routeKey, nullptr);
        if (!page)
            return;
        // **不要**改写 objectName:每一页的构造函数已经把自己那份设好了(TasksPage / KeymapPage …),
        // 而按需建页是"页面自己的身份"最要紧的时候(测试与调试都靠它找页面)。
        // 只有占位页(makePlaceholderPage)才需要按路由拼一个名字。
        m_pages.insert(routeKey, page);
        m_stack->addWidget(page);
        /* 新页面里的侧栏(版本选择页 / 下载页的侧2)**在这里统一登记** ——
         * 页面自己不用知道 MainWindow(它们是自由函数建的),规则也只有一份:
         * 同一时刻最多一条栏展开(docs/27 §11,用户 2026-09-26 口径)。 */
        const QList<NavPanel *> rails = page->findChildren<NavPanel *>();
        for (NavPanel *rail : rails) {
            registerRail(rail);
        }
    }

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
    // 常驻页 = 侧边栏那 6 项 + 重构后不占侧边栏、但从"下载/更多"进的 3 页(docs/25)。
    // 淘汰只该落到真正的会话页(download_config / download_progress / launch)身上。
    static const QSet<QString> persistent{
        QStringLiteral("home"),   QStringLiteral("download"), QStringLiteral("team"),
        QStringLiteral("multiplayer"), QStringLiteral("more"), QStringLiteral("settings"),
        QStringLiteral("versions"), QStringLiteral("tasks"),   QStringLiteral("keymap")};
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
        /* **先把这个页面里还在跑的动画停掉,再摘出窗口**（2026-09-23 真机崩溃的现场:
         * 异常码 0xC0000005,出错指令在 Qt6Core 的 QAbstractAnimation::stop ——
         * 典型的"动画对象被销毁之后又被 stop()"）。
         * qf 的 InfoBar / 提示条淡出动画挂在全局管理器上,页面一走它就悬空:
         * 我们这边能做的就是把**页面自己身上**的动画先停干净,再把页面藏起来、
         * 用 deleteLater 交给事件循环 —— 别在动画正跑的时候把它连父带子一起拆掉。 */
        const QList<QAbstractAnimation *> anims = stale->findChildren<QAbstractAnimation *>();
        for (QAbstractAnimation *anim : anims) {
            if (anim != nullptr) {
                anim->stop();
            }
        }
        stale->hide();
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
#if !defined(Q_OS_ANDROID)
    // 退出时"不留半成品"只认**本次新建**的实例目录:装之前它不存在,才有资格被我们擦掉。
    // (别人的/已经装过的实例目录一个字节都不碰;它们本来也被 R1 的预检挡在外面。)
    {
        const QString instance = versionName.isEmpty() ? versionId : versionName;
        const QString dir = uiGameDirectory() + QStringLiteral("/versions/") + instance;
        if (!QFileInfo::exists(dir)) {
            const QString entry = uiGameDirectory() + QLatin1Char('|') + instance;
            if (!m_sessionCreatedInstances.contains(entry))
                m_sessionCreatedInstances.append(entry);
        }
    }
#endif
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
    resume.instanceName = versionName;
    resume.loaderType = loaderType;
    resume.loaderVersion = loaderVersion;
    resume.gameDir = uiGameDirectory(); // schema 2:恢复时必须核对"还是不是这个目录"
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
    resume.instanceName = versionId;
    resume.gameDir = uiGameDirectory();
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
    if (QWidget *page = m_pages.value(taskId, nullptr)) {
        showTempPage(page, QString());
        return;
    }
    // 页还没建出来 = 这条任务是"上次未完成"里登记的可点击记录:
    // 用户**点了**才恢复(见 loadTaskState / resumePendingTask)。
    (void)resumePendingTask(taskId);
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
    if (e->type() != QEvent::WindowStateChange || m_inShutdown)
        return;

    // 最大化/还原时三键的图标要跟着变(qf 的 TitleBar.eventFilter 也是在 WindowStateChange
    // 时刷 maxBtn 的图标;我们的最大化键在绘制时读 window()->isMaximized(),所以要主动重绘)。
    if (m_titleBar) {
        const QList<QAbstractButton *> buttons = m_titleBar->findChildren<QAbstractButton *>();
        for (QAbstractButton *b : buttons)
            b->update();
    }

    // 「最大化 ⇒ 置顶」必须同生同灭:用户从**任何**入口进出最大化(标题栏双击、
    // 系统快捷键),置顶标志都要跟着走 —— 只接按钮点击那条路一定会漏。
    // 小窗口形态不算最大化(它的规则是"小窗口",不申请置顶)。
    if (m_mode != WindowMode::Mini) {
        const bool wantTopMost = isMaximized();
        if (wantTopMost != isTopMost())
            applyTopMost(wantTopMost);
    }

    // 窗口被系统最小化(任务栏按钮、Win+Down 之类)也按「最小化 = 隐藏界面」处理:
    // 用户给的规则是对"最小化"这个**动作**说的,不只是对标题栏那个按钮说的。
    // 用一个 0 延时排到事件循环之后,免得在状态切换的中途再 hide() 自己。
    if (isMinimized() && m_tray != nullptr)
        QTimer::singleShot(0, this, &MainWindow::hideToTray);
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

// ═══════════════════════════════════════════════════════════════════════════
// 窗口行为:关闭 / 最大化(置顶) / 缩成小窗口 / 最小化(隐藏)
//
// 这一整块都是**新增设计**:Python 版 src/app/main_window.py 的窗口部分没有这套语义 ——
// 它最小化就缩到任务栏、关闭即退出、没有置顶、没有小窗口、没有托盘。规则由用户口述给出
// (「关闭就是关闭;最大化申请悬于其他应用窗口;缩成小窗口;最小化就退出但不杀进程」),
// 这里逐条实现,并保证每条都给出可核对的客观读数。完整说明见 docs/13-窗口行为.md。
//
// 一句话原则:**窗口状态只有一个写入口**。标题栏三键、标题栏双击、托盘菜单、自检脚本
// 走的都是本文件这一组方法,不存在"谁把窗口藏了/谁又把它显示出来"互相覆盖。
// ═══════════════════════════════════════════════════════════════════════════

// ─────────────────────────── 客观读数(只读)───────────────────────────

bool MainWindow::taskRunning(const QString &pageKey) const {
    return runningThreadsUnder(m_pages.value(pageKey, nullptr)) > 0;
}

int MainWindow::runningTaskCount() const {
    int n = 0;
    for (const TaskRecord &record : m_tasks)
        n += runningThreadsUnder(m_pages.value(record.id, nullptr));
    return n;
}

QString MainWindow::currentTaskSummary() const {
    for (const TaskRecord &record : m_tasks) {
        QWidget *page = m_pages.value(record.id, nullptr);
        if (runningThreadsUnder(page) <= 0)
            continue;
        // 进度取页面**自己**那根进度条的值:不另立一份计数,免得两个数字对不上
        const QProgressBar *bar = pageProgressBar(page);
        if (bar != nullptr)
            return QStringLiteral("%1 · %2%").arg(record.title).arg(bar->value());
        return record.status.isEmpty()
                   ? record.title
                   : QStringLiteral("%1 · %2").arg(record.title, record.status);
    }
    return QString();
}

bool MainWindow::isTopMost() const {
#ifdef Q_OS_WIN
    // 直接问窗口管理器,不问 Qt 的 hint 标志:WS_EX_TOPMOST 才是"真的悬在别人上面"的判据。
    // (Qt 的 WindowStaysOnTopHint 走 setWindowFlag,会重建原生窗口 —— 我们不那么做。)
    HWND hwnd = reinterpret_cast<HWND>(const_cast<MainWindow *>(this)->winId());
    if (hwnd == nullptr)
        return false;
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return (style & WS_EX_TOPMOST) != 0;
#else
    return (windowFlags() & Qt::WindowStaysOnTopHint) != 0;
#endif
}

QString MainWindow::windowStateLine() const {
    const char *modeName = m_mode == WindowMode::Maximized ? "maximized"
                           : m_mode == WindowMode::Mini    ? "mini"
                                                           : "normal";
    const QRect rect = geometry();
    const QRect normal = normalGeometry();
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(const_cast<MainWindow *>(this)->winId());
    const LONG_PTR exStyle = hwnd != nullptr ? ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) : 0;
    const int sysVisible = hwnd != nullptr ? (::IsWindowVisible(hwnd) ? 1 : 0) : -1;
#else
    // 非 Windows 分支**只能用可移植类型**:LONG_PTR 是 Win32 的,Android/Linux 上不存在
    // (实测挡住过 Android 构建)。qintptr 在 Windows 上就是指针宽度,和 GetWindowLongPtrW 的返回同宽。
    const qintptr exStyle = 0;
    const int sysVisible = isVisible() ? 1 : 0;
#endif
    // 一行里把"窗口在哪儿/有多大/可不可见/是不是置顶"全摊开 ——
    // 验收要的是读数,不是"看着像":这行和 PowerShell 侧独立取的值是同一批 Win32 常量。
    return QStringLiteral(
               "win | pid=%1 mode=%2 qtVisible=%3 win32IsWindowVisible=%4 hiddenToTray=%5 "
               "exstyle=0x%6 wsExTopmost=%7 rect=(%8,%9 %10x%11) normalRect=(%12,%13 %14x%15) "
               "runningTasks=%16 task=[%17]")
        .arg(QCoreApplication::applicationPid())
        .arg(QString::fromLatin1(modeName))
        .arg(isVisible() ? 1 : 0)
        .arg(sysVisible)
        .arg(m_hiddenToTray ? 1 : 0)
        .arg(qlonglong(exStyle), 0, 16)
        .arg(isTopMost() ? 1 : 0)
        .arg(rect.x())
        .arg(rect.y())
        .arg(rect.width())
        .arg(rect.height())
        .arg(normal.x())
        .arg(normal.y())
        .arg(normal.width())
        .arg(normal.height())
        .arg(runningTaskCount())
        .arg(currentTaskSummary());
}

// ─────────────────────── 最大化 = 最大化 + 置顶 ───────────────────────

void MainWindow::applyTopMost(bool on) {
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr)
        return;
    // HWND_TOPMOST / HWND_NOTOPMOST = 用户要的"申请悬于其他应用窗口之上"。
    // SWP_NOMOVE | SWP_NOSIZE:只改 Z 序与扩展样式,绝不动最大化算好的那套矩形;
    // SWP_NOACTIVATE:拿置顶不该把焦点从别的程序那里抢过来。
    ::SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    if (on == ((windowFlags() & Qt::WindowStaysOnTopHint) != 0))
        return;
    const bool wasVisible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, on);
    if (wasVisible)
        show();
#endif
}

void MainWindow::setWindowMode(WindowMode mode) {
    // 形态的**唯一**写入口(小窗口那条路走 setMiniMode,它还要顺手记 geometry)
    switch (mode) {
    case WindowMode::Maximized:
        showMaximized();
        applyTopMost(true); // 「最大化申请悬于其他应用窗口」——最大化与置顶同生
        break;
    case WindowMode::Normal:
        applyTopMost(false); // 先摘置顶再还原,免得还原那一瞬间还压着别的程序
        showNormal();
        break;
    case WindowMode::Mini:
        return; // 小窗口不从这里进
    }
    m_mode = mode;
    updateChromeForMode();
    syncTray();
    uiTrace(windowStateLine());
}

void MainWindow::toggleMaximize() {
#ifdef Q_OS_ANDROID
    // ── 安卓:最大化 = 全屏 <-> 悬浮窗 ────────────────────────────────────
    // 用户原话:"如果是全屏,单击就变成悬浮窗口,就是涉及到安卓的那个
    // 『允许应用在其他应用上显示』"。桌面那套"最大化 + 置顶"在安卓上没有意义
    // (applyTopMost 只有 Win32 的实现),所以这里换成悬浮窗语义;实现顺序见
    // sxclAndroidToggleMaximize():C1 画中画 -> C2 overlay 小面板 -> C3 InfoBar。
    // 进度取自小窗口面板那条(与托盘菜单同一个取值口),overlay 面板显示它。
    sxclAndroidToggleMaximize(this, m_miniProgress != nullptr ? m_miniProgress->value() : 0);
    return;
#else
    // 小窗口形态下点最大化:先退出小窗口(恢复原 geometry),再按普通规则最大化
    if (m_mode == WindowMode::Mini)
        setMiniMode(false);
    if (m_mode == WindowMode::Maximized || isMaximized())
        setWindowMode(WindowMode::Normal);
    else
        setWindowMode(WindowMode::Maximized);
#endif
}

// ────────────────────── 缩成小窗口 / 恢复原尺寸 ──────────────────────

void MainWindow::buildMiniPanel() {
    // 「小窗口」保留的东西(用户要求"至少保留"):
    //   * 标题栏 —— 就是上面那条真标题栏(三键照旧可用),不是另画的假标题
    //   * 当前任务 + 进度 —— 本面板的两行;没有任务时显示"当前任务:无"
    //   * 恢复键 —— 「恢复原尺寸」,点它回到进入小窗口之前的尺寸与位置
    m_miniPanel = new QWidget(this);
    m_miniPanel->setObjectName(QStringLiteral("sxclMiniPanel"));
    m_miniPanel->setAttribute(Qt::WA_StyledBackground, true);
    m_miniPanel->hide(); // 只在「缩成小窗口」形态里出现

    auto *lay = new QVBoxLayout(m_miniPanel);
    lay->setContentsMargins(16, 12, 16, 12);
    lay->setSpacing(8);

    m_miniTaskLabel = new QLabel(QStringLiteral("当前任务:无"), m_miniPanel);
    m_miniTaskLabel->setObjectName(QStringLiteral("sxclMiniTask"));

    m_miniProgress = new QProgressBar(m_miniPanel);
    m_miniProgress->setObjectName(QStringLiteral("sxclMiniProgress"));
    m_miniProgress->setRange(0, 100);
    m_miniProgress->setValue(0);
    m_miniProgress->setTextVisible(false);
    m_miniProgress->setFixedHeight(6);

    auto *row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    m_miniRestoreButton = new QPushButton(QStringLiteral("恢复原尺寸"), m_miniPanel);
    m_miniRestoreButton->setObjectName(QStringLiteral("sxclMiniRestore"));
    m_miniRestoreButton->setCursor(Qt::PointingHandCursor);
    // "再点一次恢复原尺寸与位置"的那个入口(托盘菜单里还有同一个动作)
    connect(m_miniRestoreButton, &QPushButton::clicked, this, [this] { setMiniMode(false); });
    row->addStretch(1);
    row->addWidget(m_miniRestoreButton);

    lay->addWidget(m_miniTaskLabel);
    lay->addWidget(m_miniProgress);
    lay->addLayout(row);
    lay->addStretch(1);
}

void MainWindow::layoutMiniPanel() {
    if (m_miniPanel == nullptr)
        return;
    // 贴在标题栏下面,占满余下整块 —— 绝对定位,不进根布局(qf 的 1:1 摆法不动)
    m_miniPanel->setGeometry(0, kTitleBarHeight, width(), qMax(0, height() - kTitleBarHeight));
}

void MainWindow::setMiniMode(bool on) {
    if (on == (m_mode == WindowMode::Mini)) {
        syncTray(); // 已经在目标形态里(托盘菜单可能重复点),把菜单文案对齐就够
        return;
    }

    if (on) {
        // 记住"原尺寸与位置"。**不用先 showNormal() 再读 geometry()**:那要等窗口系统把
        // 还原做完,读早了拿到的还是最大化尺寸。QWidget::normalGeometry() 正是"最大化之前
        // 那个普通矩形",Qt 自己记着,读它才是可靠的。
        const bool fromMaximized = (m_mode == WindowMode::Maximized) || isMaximized();
        m_preMiniMode = fromMaximized ? WindowMode::Maximized : WindowMode::Normal;
        m_normalGeometry = fromMaximized ? normalGeometry() : geometry();
        m_haveNormalGeometry = true;
        if (fromMaximized)
            showNormal(); // 小窗口不是"最大化的缩小版":先真的还原

        // 主窗口最小 900x600:不先放开这个下限,窗口根本缩不到 420x168
        setMinimumSize(kMiniWidth, kMiniHeight);
        m_mode = WindowMode::Mini;
        updateChromeForMode();

        // 位置沿用原来的左上角(看起来是"原地缩小"),但**夹回屏幕可用区** ——
        // 原来在屏幕右下角的窗口缩完之后不该跑到屏幕外面去。
        QPoint at = m_normalGeometry.topLeft();
        QScreen *screen = QGuiApplication::screenAt(at);
        if (screen == nullptr)
            screen = QGuiApplication::primaryScreen();
        if (screen != nullptr) {
            const QRect avail = screen->availableGeometry();
            at.setX(qBound(avail.left(), at.x(), qMax(avail.left(), avail.right() - kMiniWidth + 1)));
            at.setY(qBound(avail.top(), at.y(), qMax(avail.top(), avail.bottom() - kMiniHeight + 1)));
        }
        setGeometry(QRect(at, QSize(kMiniWidth, kMiniHeight)));
    } else {
        m_mode = m_preMiniMode;
        // 先把形态摆回去(导航/内容栈回来),再复原矩形 —— 顺序反了的话内容栈会先在
        // 小矩形里布局一次,用户看到的是"先挤一下再展开"。
        setMinimumSize(kMinimumWidth, kMinimumHeight);
        updateChromeForMode();
        if (m_haveNormalGeometry)
            setGeometry(m_normalGeometry); // **原尺寸与位置**,逐值相等
        if (m_preMiniMode == WindowMode::Maximized) {
            showMaximized();
            applyTopMost(true);
        }
    }
    syncTray();
    uiTrace(windowStateLine());
}

void MainWindow::toggleMiniMode() { setMiniMode(m_mode != WindowMode::Mini); }

void MainWindow::updateChromeForMode() {
    const bool mini = (m_mode == WindowMode::Mini);
    if (m_nav != nullptr)
        m_nav->setVisible(!mini);
    if (m_stack != nullptr) {
        m_stack->setVisible(!mini);
        // 内容列的容器就是栈的父控件:栈藏了它也就没内容了,一起藏,免得留一块空列。
        // (不另存成员:buildUi 里 body 就是 m_stack 的父控件。)
        if (QWidget *body = m_stack->parentWidget())
            body->setVisible(!mini);
    }
    if (m_miniPanel != nullptr) {
        m_miniPanel->setVisible(mini);
        if (mini) {
            layoutMiniPanel();
            m_miniPanel->raise(); // 盖在根布局的内容之上(标题栏随后再 raise 一次)
        }
    }
    layoutTitleBar();
    if (m_tray != nullptr)
        m_tray->setMiniMode(mini);
}

// ──────────────────── 最小化 = 隐藏窗口(进程照跑)────────────────────

void MainWindow::hideToTray() {
#ifdef Q_OS_ANDROID
    // ── 安卓:最小化 = 退出桌面、退回后台(**不是**托盘)────────────────────
    // 用户原话:"退出桌面,不用管托盘,就是关闭窗口,用户从底部滑起来能选进来"。
    //   * moveTaskToBack() 就是按 Home 的效果:系统把整个 task 送到后台,主界面
    //     从屏幕上消失(是系统把它送走的,不是我们 hide());
    //   * 进程、Qt 事件循环、下载/安装 worker 全都继续跑;
    //   * 恢复入口是**底部上滑 / 最近任务** —— 启动器的 activity 还在返回栈里。
    //   这里**不能** hide():Qt 窗口一旦 hide(),从最近任务回来时它仍然是不可见的。
    //   也**不能**走上面那条托盘路:安卓没有托盘,m_tray 恒为空,那条路会直接 return
    //   什么都不做(这正是这次要修的行为)。
    const bool ok = androidMoveTaskToBack();
    persistTaskState(true); // 顺手落一次盘:进程虽然没退,状态已经写在磁盘上了
    androidTrace(QStringLiteral("安卓最小化=退到后台(moveTaskToBack=%1,进程与后台任务继续跑) %2")
                .arg(ok ? 1 : 0)
                .arg(windowStateLine()));
    if (!ok) {
        // 系统拒绝(极少见):如实说,不静默失败。
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("没能退到后台"),
                      QStringLiteral("系统拒绝了「最小化」请求,窗口保持原样。"
                                     "可以直接按设备的主屏幕键,启动器仍然在后台运行,任务不会停。"),
                      this, 6000);
    }
    return;
#else
    // ── 桌面(Windows):以下一个字节都没动 ──────────────────────────────
    if (m_tray == nullptr) {
        // 没有托盘就**不能**藏:藏了用户没有任何入口把窗口叫回来,进程会变成不可见的残留。
        // 如实记一行,窗口保持原样 —— 宁可不隐藏,也不能让窗口失踪。
        uiTrace(QStringLiteral("win | 本会话没有系统托盘,忽略「最小化=隐藏」,窗口保持可见"));
        return;
    }
    // 隐藏,而不是 showMinimized():
    //   用户要的是"退出界面但不杀进程" —— 界面就该从桌面上消失,而不是缩到任务栏留个按钮。
    //   hide() 之后 IsWindowVisible(hwnd) = 0,而进程、事件循环、worker 线程全都照跑。
    m_hiddenToTray = true;
    hide();
    // 顺手把还在跑的任务落一次盘:进程虽然没退,但状态已经写在磁盘上了
    persistTaskState(true);
    syncTray();
    uiTrace(QStringLiteral("win | 最小化=隐藏到托盘(进程与后台任务继续跑) %1")
                .arg(windowStateLine()));
#endif
}

void MainWindow::showMainWindow() {
    if (isMinimized())
        showNormal(); // 万一被系统最小化过
    show();
    raise();
    activateWindow();
    m_hiddenToTray = false;
    // 回来时保持原来的形态:最大化状态下退的界面,回来还得是最大化 + 置顶
    if (m_mode == WindowMode::Maximized)
        applyTopMost(true);
    syncTray();
    uiTrace(QStringLiteral("win | 显示主窗口 %1").arg(windowStateLine()));
}

// ───────────────────────── 关闭 = 真关闭 ─────────────────────────

void MainWindow::closeEvent(QCloseEvent *e) {
    // 关闭 = **真关闭**(退出、释放资源)。这里**不是**隐藏 —— 隐藏是"最小化"那条规则。
    if (m_inShutdown) {
        e->accept();
        return;
    }
    const int running = runningTaskCount();
    if (running > 0 && closeNeedsConfirm()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("退出"),
            QStringLiteral("还有 %1 个任务在跑。现在退出会把它们停下 —— 任务状态已经记下来了,"
                           "下次启动可以继续。要退出吗?")
                .arg(running),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            e->ignore();
            uiTrace(QStringLiteral("win | 退出确认里选了「不退出」,继续运行"));
            return;
        }
    }
    e->accept();
    finishAndQuit();
}

void MainWindow::requestClose() { close(); } // -> closeEvent -> finishAndQuit

void MainWindow::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    // 从托盘回来时置顶标志会被系统清掉,这里补回来(最大化形态必须仍然悬在上面)
    if (!m_inShutdown && m_mode == WindowMode::Maximized)
        applyTopMost(true);
}

void MainWindow::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    if (m_inShutdown)
        return;
    // 隐藏本身**什么都不做**:进程、事件循环、worker 线程照跑 —— 这正是"最小化 ≠ 退出"。
    // 这一行日志就是那条规则的证据:窗口已经不可见,后台任务仍在跑。
    uiTrace(QStringLiteral("win | 窗口已隐藏(进程与后台任务继续) %1").arg(windowStateLine()));
}

// 「结束后关闭」的落地处(下载/安装页三条终态都调它一次)。
void MainWindow::notifyInstallFinished(bool ok, bool cancelled) {
#if !defined(Q_OS_ANDROID)
    if (m_quitting || m_inShutdown)
        return;
    if (!closeAfterInstallEnabled())
        return;
    if (cancelled || !ok) {
        // **失败/取消不关**:用户就是要看"为什么没成功" —— 自动退出等于把原因藏起来。
        uiTrace(QStringLiteral("win | 结束后关闭:本次 ok=%1 cancelled=%2 -> 不自动退出(原因留在界面上)")
                    .arg(ok ? 1 : 0)
                    .arg(cancelled ? 1 : 0));
        InfoBar::push(InfoBar::Type::Info, QStringLiteral("「结束后关闭」已开启"),
                      QStringLiteral("这次没成功,先不退出 —— 失败/取消的原因留在界面上"), this,
                      6000);
        return;
    }
    uiTrace(QStringLiteral("win | 结束后关闭:安装完成 -> 3 秒后自动退出(来得及看一眼结果)"));
    InfoBar::push(InfoBar::Type::Success, QStringLiteral("安装完成"),
                  QStringLiteral("「结束后关闭」已开启,3 秒后自动退出"), this, 3000);
    QTimer::singleShot(3000, this, [this] { finishAndQuit(); });
#else
    (void)ok;
    (void)cancelled;
#endif
}

#if !defined(Q_OS_ANDROID)
// 退出前主动取消正在跑的安装(电脑端)。返回被取消的 "<游戏目录>|<实例名>"。
//
// 为什么不能只发 requestInterruption/quit:那两条对"正阻塞在 run() 里的工作对象"没有效果 ——
// 真正让核心库停下的是 InstallWorker::cancel()(置取消位,核心库在文件边界上收工并清理 .part)。
// 这里等它真的停下(有上限,绝不把关闭变成卡死),再交给调用方擦半成品目录。
QStringList MainWindow::cancelRunningInstallsAtExit() {
    QStringList cancelled;
    for (auto it = m_pages.constBegin(); it != m_pages.constEnd(); ++it) {
        if (!it.key().startsWith(QLatin1String("download_progress_")))
            continue;
        QWidget *page = it.value();
        if (page == nullptr)
            continue;
        InstallWorker *worker = page->findChild<InstallWorker *>();
        if (worker == nullptr || !worker->running())
            continue;
        const QString instance = it.key().mid(int(qstrlen("download_progress_")));
        worker->cancel();
        cancelled << (uiGameDirectory() + QLatin1Char('|') + instance);
        uiTrace(QStringLiteral("win | 退出:已请求取消安装 %1(核心库在文件边界收工)").arg(instance));
    }
    if (!cancelled.isEmpty()) {
        QElapsedTimer wait;
        wait.start();
        bool allStopped = true;
        for (const QString &entry : cancelled) {
            const QString instance = entry.section(QLatin1Char('|'), 1);
            QWidget *page = m_pages.value(QStringLiteral("download_progress_") + instance, nullptr);
            InstallWorker *worker = page != nullptr ? page->findChild<InstallWorker *>() : nullptr;
            while (worker != nullptr && worker->running() && wait.elapsed() < 8000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(10);
            }
            if (worker != nullptr && worker->running()) {
                allStopped = false;
                uiTrace(QStringLiteral("win | 退出:安装 %1 8 秒内没停下,半成品这次先不删")
                            .arg(instance));
            }
        }
        uiTrace(QStringLiteral("win | 退出:取消安装 %1 条,%2(%3 ms)")
                    .arg(cancelled.size())
                    .arg(allStopped ? QStringLiteral("都已停下") : QStringLiteral("有没停下的"))
                    .arg(wait.elapsed()));
    }
    return cancelled;
}

// 只删"这次退出时被我们取消掉 **且** 是本会话新建"的实例目录。
// 两个条件缺一不可:取消掉的说明它没装完(半成品),新建的说明它整份都是我们写的 ——
// 别人的实例目录、已经装完的实例,这里一个都不碰。
void MainWindow::removeHalfInstalledDirs(const QStringList &cancelled) {
    for (const QString &entry : cancelled) {
        if (!m_sessionCreatedInstances.contains(entry)) {
            uiTrace(QStringLiteral("win | 退出:实例目录 %1 不是本次新建的,不清理").arg(entry));
            continue;
        }
        const QString gameDir = entry.section(QLatin1Char('|'), 0, 0);
        const QString instance = entry.section(QLatin1Char('|'), 1);
        if (gameDir.isEmpty() || instance.isEmpty())
            continue;
        const QString dir = gameDir + QStringLiteral("/versions/") + instance;
        if (!QFileInfo::exists(dir)) {
            uiTrace(QStringLiteral("win | 退出:实例目录 %1 不在,无需清理").arg(dir));
            continue;
        }
        const int rc = sxcl_fs_remove_tree(dir.toUtf8().constData());
        uiTrace(QStringLiteral("win | 退出:半成品实例目录已清理 %1(rc=%2)").arg(dir).arg(rc));
    }
}
#endif // !Q_OS_ANDROID

void MainWindow::finishAndQuit() {
    if (m_quitting)
        return; // 关闭键与托盘「退出」可能先后到达,收尾只做一次
    m_quitting = true;

    const int running = runningTaskCount();
    uiTrace(QStringLiteral("win | 关闭:开始收尾(runningTasks=%1)").arg(running));

    // 1) **先把任务状态落盘** —— 用户明确要求"默认直接退出,但要先把任务状态落盘"。
    //    顺序不能反:先落盘,再停 worker。
    persistTaskState(true);

#if !defined(Q_OS_ANDROID)
    // 2) **电脑端:主动取消正在跑的安装**,并记下"这次要擦掉的半成品实例目录"。
    //    (安卓不适用:系统回收进程,行为不同 —— 那边照旧只发收工请求。)
    const QStringList cancelledInstalls = cancelRunningInstallsAtExit();
#endif

    // 3) 显式结束后台 worker(不在这里 join:原因见 stopBackgroundWork 的注释)
    stopBackgroundWork();

#if !defined(Q_OS_ANDROID)
    // 3.5) 取消掉的那些安装不留半成品:把本次新建的实例目录擦掉(成功装完的不受影响)。
    removeHalfInstalledDirs(cancelledInstalls);
#endif

    // 3.6) 托盘先收掉:否则任务栏上会留下一个要等鼠标划过才消失的幽灵图标
    if (m_tray != nullptr)
        m_tray->hide();

    // 4) 退出事件循环 -> main() 返回 -> 窗口析构 -> 页面析构 -> worker 析构(取消 + join)
    m_inShutdown = true;
    uiTrace(QStringLiteral("win | 关闭:退出事件循环,进程即将消失"));
    QCoreApplication::quit();
}

int MainWindow::stopBackgroundWork() {
    int stopped = 0;
    for (const TaskRecord &record : m_tasks) {
        QWidget *page = m_pages.value(record.id, nullptr);
        if (page == nullptr)
            continue;
        const QList<QThread *> threads = page->findChildren<QThread *>();
        for (QThread *thread : threads) {
            if (thread == nullptr || !thread->isRunning())
                continue;
            ++stopped;
            // 只发收工请求,不在这里 wait():主线程等下载线程会把"关闭"变成一个卡住的关闭。
            // 真正的"停"由 worker 自己的析构完成 —— install_worker.cpp:104-113 与
            // launch_worker.cpp:45-53:置取消标志 -> quit() -> wait()。页面随窗口析构时执行。
            thread->requestInterruption();
            thread->quit();
        }
    }
    uiTrace(QStringLiteral("win | 退出:后台 worker 线程 %1 条接到收工请求(join 由页面析构负责)")
                .arg(stopped));
    return stopped;
}

// ───────────────────────────── 托盘 ─────────────────────────────

void MainWindow::buildTray() {
    if (!SxclTray::available()) {
        uiTrace(QStringLiteral("win | 本会话没有系统托盘:不建托盘,最小化也不会隐藏窗口"));
        return;
    }
    m_tray = new SxclTray(this);
    connect(m_tray, &SxclTray::showWindowRequested, this, &MainWindow::showMainWindow);
    connect(m_tray, &SxclTray::toggleMiniRequested, this, &MainWindow::toggleMiniMode);
    connect(m_tray, &SxclTray::quitRequested, this, &MainWindow::finishAndQuit);
    m_tray->show();

    // 「最小化 = 隐藏界面但进程活着」的前提:关掉最后一个窗口**不**结束进程。
    // **只有真的有托盘时才打开这条规则** —— 没有托盘还打开的话,窗口一藏用户就再也找不回来了。
    // Qt6 里 setQuitOnLastWindowClosed 在 **QGuiApplication**(QApplication 继承)上,
    // 不在 QCoreApplication 上 —— 写错就是编译错误(实测被 Android 与桌面同时抓到)。
    QGuiApplication::setQuitOnLastWindowClosed(false);
    syncTray();
}

void MainWindow::syncTray() {
    // 小窗口面板那一行与托盘菜单的「当前任务」共用这一份取值(currentTaskSummary),
    // 所以两处永远说同一句话,不会一个说"在下载"一个说"无"。
    const QString summary = currentTaskSummary();

    if (m_miniTaskLabel != nullptr) {
        m_miniTaskLabel->setText(summary.isEmpty()
                                     ? QStringLiteral("当前任务:无")
                                     : QStringLiteral("当前任务:%1").arg(summary));
    }
    int percent = -1;
    for (const TaskRecord &record : m_tasks) {
        QWidget *page = m_pages.value(record.id, nullptr);
        if (runningThreadsUnder(page) <= 0)
            continue;
        if (const QProgressBar *bar = pageProgressBar(page))
            percent = bar->value();
        break;
    }
    if (m_miniProgress != nullptr) {
        m_miniProgress->setVisible(percent >= 0);
        m_miniProgress->setValue(percent >= 0 ? percent : 0);
    }
#ifdef Q_OS_ANDROID
    // 安卓的"悬浮窗"如果走的是 overlay 那条路(C2),面板上的任务/进度必须跟着任务走,
    // 否则它就是一张过期的快照。面板没显示时这一行只做一次 JNI 查询,不碰任何东西。
    // (画中画那条路显示的是启动器界面本身,界面自己会刷新,不需要这里。)
    if (androidOverlayPanelShown())
        androidShowOverlayPanel(summary, percent >= 0 ? percent : 0);
#endif

    if (m_tray != nullptr) {
        m_tray->setMiniMode(m_mode == WindowMode::Mini);
        m_tray->setTaskSummary(summary);
    }
}

// ─────────── 任务状态落盘 / 下次启动恢复(「关掉也能接着下」)───────────

void MainWindow::rememberTask(const TaskRecord &record) {
    for (TaskRecord &existing : m_tasks) {
        if (existing.id != record.id)
            continue;
        // 只覆盖非空字段:addOrUpdateTask 只带标题/状态,不能把恢复用的版本信息抹成空
        if (!record.kind.isEmpty())
            existing.kind = record.kind;
        if (!record.title.isEmpty())
            existing.title = record.title;
        if (!record.status.isEmpty())
            existing.status = record.status;
        if (!record.versionId.isEmpty())
            existing.versionId = record.versionId;
        if (!record.versionName.isEmpty())
            existing.versionName = record.versionName;
        if (!record.loaderType.isEmpty())
            existing.loaderType = record.loaderType;
        if (!record.loaderVersion.isEmpty())
            existing.loaderVersion = record.loaderVersion;
        if (!record.instanceName.isEmpty())
            existing.instanceName = record.instanceName;
        if (!record.gameDir.isEmpty())
            existing.gameDir = record.gameDir;
        return;
    }
    m_tasks.append(record);
}

void MainWindow::persistTaskState(bool interrupted) {
    const QString path = taskStatePath();

    QJsonArray tasks;
    for (TaskRecord &record : m_tasks) {
        record.running = taskRunning(record.id); // **实测**它在不在跑,不是猜
        if (!record.running)
            continue; // 跑完的/没跑的任务不需要恢复
        QJsonObject item;
        item[QStringLiteral("id")] = record.id;
        item[QStringLiteral("kind")] = record.kind;
        item[QStringLiteral("title")] = record.title;
        item[QStringLiteral("status")] = record.status;
        item[QStringLiteral("versionId")] = record.versionId;
        item[QStringLiteral("versionName")] = record.versionName;
        item[QStringLiteral("loaderType")] = record.loaderType;
        item[QStringLiteral("loaderVersion")] = record.loaderVersion;
        // schema 2(1 -> 2 新增):**目标目录**与实例名。没有 gameDir 的旧记录恢复时不敢自动装
        // —— "这次解析出来的目录"和"当时装的那个目录"可能不是一个(实测事故就是这么发生的)。
        item[QStringLiteral("instanceName")] =
            record.instanceName.isEmpty() ? record.versionName : record.instanceName;
        item[QStringLiteral("gameDir")] = record.gameDir;
        tasks.append(item);
    }

    if (tasks.isEmpty()) {
        // 没有没跑完的任务 = 下次启动没什么可恢复的。留着旧文件只会让下次开机去"恢复"
        // 一个早就做完的任务 —— 删掉(不存在也不算错)。
        if (QFile::exists(path) && QFile::remove(path))
            uiTrace(QStringLiteral("win | 任务状态:没有未完成任务,%1 已清除").arg(path));
        return;
    }

    QJsonObject root;
    // schema 2:每个任务多记 gameDir / instanceName(见 TaskRecord 的说明)。旧版读得懂 1。
    root[QStringLiteral("schema")] = 2;
    root[QStringLiteral("app")] = QString::fromUtf8(kTitle);
    root[QStringLiteral("interrupted")] = interrupted;
    root[QStringLiteral("closedAt")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root[QStringLiteral("tasks")] = tasks;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        uiTrace(QStringLiteral("win | 任务状态**落盘失败**:%1 打不开(%2)")
                    .arg(path, file.errorString()));
        return;
    }
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    const qint64 written = file.write(bytes);
    file.close();
    uiTrace(QStringLiteral("win | 任务状态已落盘:%1(%2 个未完成任务,%3 字节)")
                .arg(path)
                .arg(tasks.size())
                .arg(written));
}

// 目录比较用的归一形式(大小写/分隔符/结尾斜杠都不该算"换了目录")。
QString normalizedDir(const QString &raw) {
    if (raw.isEmpty())
        return QString();
    QString s = QDir::cleanPath(QDir::fromNativeSeparators(raw));
    while (s.size() > 1 && s.endsWith(QLatin1Char('/')))
        s.chop(1);
#if defined(Q_OS_WIN)
    return s.toLower();
#else
    return s;
#endif
}

const MainWindow::PendingResume *MainWindow::findPendingResume(const QString &taskId) const {
    for (const PendingResume &pending : m_pendingResumes) {
        if (pending.record.id == taskId)
            return &pending;
    }
    return nullptr;
}

// 任务页登记一条**可点击**的"上次未完成"记录:用户点它才恢复(见 resumePendingTask)。
void MainWindow::registerPendingResume(const PendingResume &pending) {
    QWidget *tasks = m_pages.value(QStringLiteral("tasks"), nullptr);
    if (tasks == nullptr) {
        uiTrace(QStringLiteral("win | 任务页不在,") + pending.record.id +
                QStringLiteral(" 这条恢复记录没能登记"));
        return;
    }
    const QString name = pending.record.versionName.isEmpty() ? pending.record.versionId
                                                              : pending.record.versionName;
    const QString title = QStringLiteral("上次未完成:下载 %1").arg(name);
    const QString status = pending.resumable
                               ? QStringLiteral("点这里继续安装")
                               : QStringLiteral("不能自动恢复:%1").arg(pending.blockedReason);
    const QString detail = pending.record.gameDir.isEmpty()
                               ? QStringLiteral("记录里没有目标目录")
                               : QStringLiteral("目标目录 %1")
                                     .arg(QDir::toNativeSeparators(pending.record.gameDir));
    QMetaObject::invokeMethod(tasks, "addOrUpdateTask", Qt::DirectConnection,
                              Q_ARG(QString, pending.record.id), Q_ARG(QString, title),
                              Q_ARG(int, 0), Q_ARG(QString, status), Q_ARG(QString, detail));
    uiTrace(QStringLiteral("win | 恢复记录已登记到任务页:%1(%2)")
                .arg(pending.record.id,
                     pending.resumable ? QStringLiteral("可点击恢复") : pending.blockedReason));
}

// 用户点了那条记录 -> 真的恢复(走产品路径:打开下载进度页,引擎自己按校验续传)。
bool MainWindow::resumePendingTask(const QString &taskId) {
    const PendingResume *pending = findPendingResume(taskId);
    if (pending == nullptr)
        return false;
    const TaskRecord record = pending->record;
    if (!pending->resumable) {
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("这条任务恢复不了"),
                      pending->blockedReason, this, 8000);
        return true; // 卡片是我们登记的:已经给了人话,不再走页面恢复
    }
    // **动手前再看一眼目标**:已经装过的版本绝不覆盖(与安装引擎共用核心库那一份判定)。
    const QString instance =
        record.instanceName.isEmpty() ? record.versionName : record.instanceName;
    char why[SXCL_INSTALL_ERROR_MAX];
    const int flags = sxcl_install_target_probe(record.gameDir.toUtf8().constData(),
                                                instance.toUtf8().constData());
    if ((flags & SXCL_INSTALL_TARGET_JSON) != 0) {
        sxcl_install_target_describe(why, sizeof(why), record.gameDir.toUtf8().constData(),
                                     instance.toUtf8().constData());
        /* 不覆盖是对的（那是**已经装好**的一份），但别把话说成"拒绝你"。
         * 用户 2026-09-22 晚点名：同一个版本装几份都由用户说了算，界面不评判。 */
        InfoBar::push(InfoBar::Type::Info, QStringLiteral("这条任务不用恢复了"),
                      QStringLiteral("「%1」里现在有一份能启动的版本了 —— 可能你之前已经装好了。")
                          .arg(instance),
                      this, 8000);
        uiTrace(QStringLiteral("win | 恢复被拒(目标已存在):%1 | %2").arg(instance,
                                                                        QString::fromUtf8(why)));
        return true;
    }
    for (int i = 0; i < m_pendingResumes.size(); ++i) {
        if (m_pendingResumes.at(i).record.id == taskId) {
            m_pendingResumes.removeAt(i);
            break;
        }
    }
    uiTrace(QStringLiteral("win | 用户点了恢复:%1(目标目录 %2)")
                .arg(taskId, QDir::toNativeSeparators(record.gameDir)));
    switchToDownloadProgress(record.versionId,
                             record.versionName.isEmpty() ? record.versionId : record.versionName,
                             record.loaderType, record.loaderVersion);
    return true;
}

void MainWindow::loadTaskState(bool autoResume) {
    const QString path = taskStatePath();
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = file.readAll();
    file.close();

    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject()) {
        uiTrace(QStringLiteral("win | 任务状态文件不是 JSON,按没有处理:%1").arg(path));
        return;
    }
    const QJsonObject root = doc.object();
    const int schema = root.value(QStringLiteral("schema")).toInt(1); // 旧文件没有 schema -> 1
    const QJsonArray tasks = root.value(QStringLiteral("tasks")).toArray();
    if (tasks.isEmpty())
        return;

    // **默认不自动续跑**(事故复盘:一唤醒就自己往一个"当时解析出来的目录"里续装,
    // 把用户真实在用的同名版本覆盖了)。现在只登记一条可点击的记录 + InfoBar 告知,
    // 用户点了才恢复。SXCL_UI_RESUME_TASKS=1 = 显式打开自动恢复(验收钉子),仅此一处例外。
    const QString pinned = qEnvironmentVariable("SXCL_UI_RESUME_TASKS");
    const bool autoResumePinned = pinned == QLatin1String("1");
    const QString currentDir = uiGameDirectory();
    const QString currentNorm = normalizedDir(currentDir);

    int resumed = 0;
    int registered = 0;
    int blocked = 0;
    for (const QJsonValue &value : tasks) {
        const QJsonObject item = value.toObject();
        TaskRecord record;
        record.id = item.value(QStringLiteral("id")).toString();
        record.kind = item.value(QStringLiteral("kind")).toString();
        record.title = item.value(QStringLiteral("title")).toString();
        record.status = item.value(QStringLiteral("status")).toString();
        record.versionId = item.value(QStringLiteral("versionId")).toString();
        record.versionName = item.value(QStringLiteral("versionName")).toString();
        record.instanceName = item.value(QStringLiteral("instanceName")).toString();
        record.loaderType = item.value(QStringLiteral("loaderType")).toString();
        record.loaderVersion = item.value(QStringLiteral("loaderVersion")).toString();
        record.gameDir = item.value(QStringLiteral("gameDir")).toString(); // schema 2 才有
        if (record.id.isEmpty())
            continue;
        rememberTask(record);

        PendingResume pending;
        pending.record = record;
        if (record.kind != QLatin1String("download") || record.versionId.isEmpty()) {
            pending.blockedReason = QStringLiteral("这不是一条下载任务,不能接着装");
        } else if (record.gameDir.isEmpty()) {
            // schema 1 的旧记录(或旧版本写的):不知道当时装到哪个目录 -> **不自动恢复**
            pending.blockedReason =
                QStringLiteral("旧记录里没有目标目录(写入时还没记这个字段),不敢替你选目录");
        } else if (normalizedDir(record.gameDir) != currentNorm) {
            pending.blockedReason = QStringLiteral("上次目标目录已变(旧:%1 / 新:%2)")
                                        .arg(QDir::toNativeSeparators(record.gameDir),
                                             QDir::toNativeSeparators(currentDir));
        } else {
            pending.resumable = true;
        }

        // 显式钉子开了 + 目录没变 -> 才自动恢复(老行为,给验收用)
        if (autoResumePinned && autoResume && pending.resumable) {
            switchToDownloadProgress(
                record.versionId,
                record.versionName.isEmpty() ? record.versionId : record.versionName,
                record.loaderType, record.loaderVersion);
            ++resumed;
            continue;
        }
        m_pendingResumes.append(pending);
        registerPendingResume(pending);
        if (pending.resumable)
            ++registered;
        else
            ++blocked;
    }

    uiTrace(QStringLiteral("win | 上次退出时有 %1 个未完成任务(schema=%2):%3")
                .arg(tasks.size())
                .arg(schema)
                .arg(resumed > 0
                         ? QStringLiteral("自动恢复 %1 个(SXCL_UI_RESUME_TASKS=1)").arg(resumed)
                         : QStringLiteral("自动启动 0 个;任务页登记 %1 条可点恢复、%2 条不能恢复")
                               .arg(registered)
                               .arg(blocked)));

    if (resumed > 0) {
        if (QFile::remove(path))
            uiTrace(QStringLiteral("win | 任务状态文件已消费(恢复过一次就不再重复恢复)"));
        return;
    }
    if (!m_pendingResumes.isEmpty()) {
        // InfoBar 只说一次(不逐条刷屏);不能恢复的那些把原因写在任务页的卡片上。
        const int count = m_pendingResumes.size();
        if (registered > 0) {
            InfoBar::push(InfoBar::Type::Info, QStringLiteral("上次有 %1 个任务没做完").arg(count),
                          QStringLiteral("为了不覆盖你可能已经在用的版本,这次没有自动开始。"
                                         "要接着装,就去「任务」页点那条记录。"),
                          this, 12000);
        } else {
            InfoBar::push(InfoBar::Type::Warning,
                          QStringLiteral("上次有 %1 个任务没做完,但不能自动恢复").arg(count),
                          m_pendingResumes.first().blockedReason, this, 12000);
        }
    }
}

// ─────────────── 取证通路:SXCL_UI_WINCHECK 脚本化自检 ───────────────

void MainWindow::startWindowSelfCheck(const QString &script) {
    // 分号分隔的一串命令。用 QString 版本切分,免得为一个小工具引入字符字面量。
    m_checkSteps = script.split(QStringLiteral(";"), Qt::SkipEmptyParts);
    for (QString &step : m_checkSteps)
        step = step.trimmed();
    m_checkIndex = 0;
    std::fprintf(stderr, "[wincheck] 脚本 %d 步: %s\n", int(m_checkSteps.size()),
                 script.toUtf8().constData());
    std::fflush(stderr);
    QTimer::singleShot(kCheckStepDelayMs, this, &MainWindow::runSelfCheckStep);
}

void MainWindow::runSelfCheckStep() {
    if (m_checkIndex >= m_checkSteps.size())
        return;
    const QString step = m_checkSteps.at(m_checkIndex++);
    int delay = kCheckStepDelayMs;

    if (step == QLatin1String("probe")) {
        // 什么也不做:只为让下面那行读数落下来
    } else if (step == QLatin1String("max")) {
        setWindowMode(WindowMode::Maximized);
    } else if (step == QLatin1String("normal")) {
        setWindowMode(WindowMode::Normal);
    } else if (step == QLatin1String("mini")) {
        setMiniMode(true);
    } else if (step == QLatin1String("unmini")) {
        setMiniMode(false);
    } else if (step == QLatin1String("togglemax")) {
        toggleMaximize();
    } else if (step == QLatin1String("min")) {
        hideToTray();
    } else if (step == QLatin1String("show")) {
        showMainWindow();
    } else if (step.startsWith(QLatin1String("download:"))) {
        const QString versionId = step.mid(int(qstrlen("download:")));
        switchToDownloadProgress(versionId, versionId);
    } else if (step.startsWith(QLatin1String("wait:"))) {
        delay = step.mid(int(qstrlen("wait:"))).toInt();
        if (delay < 0)
            delay = 0;
    } else if (step == QLatin1String("quit")) {
        requestClose(); // 走的是产品路径:closeEvent -> finishAndQuit
        return;
    } else {
        std::fprintf(stderr, "[wincheck] 未知命令: %s\n", step.toUtf8().constData());
    }

    std::fprintf(stderr, "[wincheck] step=%d/%d cmd=%s\n", m_checkIndex,
                 int(m_checkSteps.size()), step.toUtf8().constData());
    std::fprintf(stderr, "[wincheck]   %s\n", windowStateLine().toUtf8().constData());
    std::fflush(stderr);
    QTimer::singleShot(delay, this, &MainWindow::runSelfCheckStep);
}
} // namespace sxcl::ui
