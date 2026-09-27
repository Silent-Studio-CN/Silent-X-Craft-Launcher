/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 启动页 —— **2026-09-27 重写**(用户口述)。
//
// 用户原话:
//   「启动页重写:现在上面是启动加版本号,下面一堆东西;改成:上面是"启动什么",下面变成一个加载动画,
//     命令行、游戏输出、Java 版本全去掉;等待游戏下面的进度条保留,等待游戏也保留,
//     下面换成一些动态切换的小知识小 tips,不要给用户看代码。」
//
// 所以这一页现在**只有四样东西**,从上到下:
//   ① 启动 <版本号>          —— "启动什么"
//   ② 一个转圈的加载动画     —— 自绘(LoadingSpinner),它的存在就是"在动,别急"
//   ③ 一句状态 + 进度条      —— 状态里保留"等待游戏";进度条保留
//   ④ 小贴士                 —— 从我们自己的表里轮播(launch_tips.h)
// 再加上底下的三个按钮(取消 / 导出日志 / 返回)。
//
// 删掉的东西(**一个都不许回来**):Java 探测结果、最终命令行、游戏输出、阶段行(检测 Java 运行时 /
// 构建启动命令 / …)、状态徽标("退出码 0")。这些都是给排查的人看的,用户在这一页只想知道
// "游戏起来了没有"。诊断信息仍然一条不少地进**运行日志**与**错误剪贴板**(pushUiError),
// 需要的人拿得到 —— 但不再糊在用户脸上。
//
// 出问题(启动失败)时只在状态下面多**一句人话原因**(核心库给的),那是必要的,不是技术面板。

#include "page_factory.h"

#include <QAbstractButton>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstring>
#include <utility>

#include "sxcl/log.h"       // SXCL_LOG_I/E:导出结果进运行日志
#include "sxcl/logexport.h" // 一键导出日志包(token/uuid/用户名在写盘前就打码)

#include "game_folders.h"   // offlinePlayerName():主页那个离线 ID 输入框存下来的
#include "launch_tips.h"    // 小贴士表(页面只轮播,不自己编文案)
#include "main_window.h"    // nextLaunchOffline():主页"离线启动"键的那一次
#include "dialogs/account.h" // loadAccountSnapshot / accountCanLaunch(现成入口,本页不改账户代码)
#include "workers/launch_worker.h"
#include "workers/ui_error.h"
#include "workers/ui_paths.h"

#if defined(_MSC_VER)
#pragma warning(push, 0) // libqf 是外部依赖,头文件在 /W4 下不干净(见 libqf.h 的说明)
#endif
#include "fluent/fluent_cards.h"
#include "fluent/fluent_controls.h"
#include "fluent/fluent_labels.h"
#include "fluent/fluent_scroll.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "fluent_theme.h"

namespace sxcl::ui {
namespace {

// qf PushButton 构造里有一句 setFont(self)(button.py:36 -> 14px),libqf 的 PushButton::init()
// 只套 QSS 没设字号,这里按 docs/05-UI-1to1规格.md §4「PushButton 高 32,字体 14」钉回去。
void applyButtonFont(QPushButton *button) {
    QFont font = button->font();
    font.setPixelSize(14);
    font.setWeight(QFont::Normal);
    button->setFixedHeight(32);
}

/** 小贴士多久换一条。4.5 秒:够读完一句,又不至于盯着同一句发呆。
 *  顺序**固定**(表里的顺序),不随机 —— 随机的表验收没法复现,截图也没法比对。 */
constexpr int kTipIntervalMs = 4500;

// ───────────────── 加载动画(用户点名:"下面变成一个加载动画")────────────────

/** 一圈底环 + 一段弧在转。自绘、纯 Qt,不引任何图标资源。
 *  objectName 是 #sxclLaunchSpinner,验收 dump 按它认这个动画(并且在两次 dump 之间看它转没转)。 */
class LoadingSpinner : public QWidget {
public:
    explicit LoadingSpinner(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("sxclLaunchSpinner"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedSize(kSide, kSide);
        m_timer = new QTimer(this);
        m_timer->setObjectName(QStringLiteral("sxclLaunchSpinnerTimer"));
        m_timer->setInterval(40); // 25 帧/秒:足够顺,又不烧 CPU
        connect(m_timer, &QTimer::timeout, this, [this] {
            m_angle = (m_angle + 12) % 360;
            update();
        });
        m_timer->start();
    }

    /** 转 / 停(游戏退出了还转圈,就像在骗用户"还在忙")。 */
    void setSpinning(bool spinning) {
        if (spinning)
            m_timer->start();
        else
            m_timer->stop();
    }

    int angle() const { return m_angle; }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal side = qMin(width(), height()) - 6.0;
        const QRectF box((width() - side) / 2.0, (height() - side) / 2.0, side, side);
        const QColor accent = FluentTheme::instance().tokens().accent;
        QPen track(accent);
        track.setWidthF(3.0);
        QColor faint = accent;
        faint.setAlpha(48);
        track.setColor(faint);
        painter.setPen(track);
        painter.drawArc(box, 0, 360 * 16);
        QPen arc(accent);
        arc.setWidthF(3.0);
        arc.setCapStyle(Qt::RoundCap);
        painter.setPen(arc);
        // 负角度 = 顺时针;画一段 100 度的弧,角度每帧 +12 度
        painter.drawArc(box, int(-m_angle * 16), int(-100 * 16));
    }

private:
    static constexpr int kSide = 44; // 逻辑像素
    QTimer *m_timer = nullptr;
    int m_angle = 0;
};

// ───────────────── 页面本体 ────────────────

class LaunchProgressPage : public ScrollArea {
public:
    LaunchProgressPage(const VersionRef &version, QWidget *parent)
        : ScrollArea(parent), m_versionId(version.id) {
        // ---- BasePage ----
        setObjectName(QStringLiteral("LaunchProgressPage"));
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setStyleSheet(QStringLiteral("QScrollArea { background: %1; }")
                          .arg(FluentTheme::instance().tokens().bg.name()));
        setFrameShape(QFrame::StyledPanel);
        setLineWidth(1);

        m_view = new QWidget(this);
        m_view->setStyleSheet(QStringLiteral("background: transparent;"));
        setWidget(m_view);

        m_vBox = new QVBoxLayout(m_view);
        m_vBox->setContentsMargins(28, 24, 28, 24);
        m_vBox->setSpacing(16);
        m_vBox->setAlignment(Qt::AlignTop);

        // ① 启动什么(用户原话:"上面是'启动什么'")
        auto *title = new TitleLabel(QStringLiteral("启动 %1").arg(m_versionId), m_view);
        title->setObjectName(QStringLiteral("sxclLaunchTitle"));
        m_vBox->addWidget(title);

        buildContent();

        // 任务页的键:与 main_window.cpp 登记时用的完全一致
        m_taskKey = QStringLiteral("launch_") + m_versionId;

        // 真的启动(工作线程,主线程不阻塞)。用 0ms 单发而不是直接调:等事件循环转起来、
        // 页面已经能画之后再起,这样第一个 phase_changed 回来时界面已经就位。
        QTimer::singleShot(0, this, [this] { startLaunch(); });
    }

private:
    void applyThemeStyles() {
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        const QString color = m_barState == QLatin1String("failed") ? tokens.danger.name()
                              : m_barState == QLatin1String("done") ? tokens.success.name()
                                                                    : tokens.accent.name();
        m_progressBar->setStyleSheet(
            QStringLiteral("QProgressBar { border: none; background: %1; border-radius: 2px; }"
                           "QProgressBar::chunk { background: %2; border-radius: 2px; }")
                .arg(FluentTheme::instance().tokenText(QStringLiteral("track")), color));
        const QColor secondary = tokens.textSecondary;
        m_tipLabel->setTextColor(secondary, secondary);
        m_noteLabel->setTextColor(m_barState == QLatin1String("failed") ? tokens.danger
                                                                        : secondary,
                                  m_barState == QLatin1String("failed") ? tokens.danger
                                                                        : secondary);
    }

    void buildContent() {
        auto *card = new CardWidget(m_view);
        card->setObjectName(QStringLiteral("sxclLaunchCard"));
        card->setStyleSheet(QStringLiteral("CardWidget { border-radius: 8px; }"));
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(28, 24, 28, 24);
        layout->setSpacing(14);

        // ② 加载动画(居中)
        m_spinner = new LoadingSpinner(card);
        layout->addWidget(m_spinner, 0, Qt::AlignHCenter);

        // ③ 一句状态 + 进度条
        m_phaseLabel = new StrongBodyLabel(QStringLiteral("正在准备游戏…"), card);
        m_phaseLabel->setObjectName(QStringLiteral("sxclLaunchPhase"));
        m_phaseLabel->setAlignment(Qt::AlignHCenter);
        layout->addWidget(m_phaseLabel);

        m_progressBar = new QProgressBar(card);
        m_progressBar->setObjectName(QStringLiteral("sxclLaunchProgress"));
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
        m_progressBar->setFixedHeight(4);
        m_progressBar->setTextVisible(false);
        layout->addWidget(m_progressBar);

        /* 出问题时多这一句人话(核心库给的完整原因)。平时**一个字都不显示** ——
         * 它不是技术面板,是"为什么没起来"。 */
        m_noteLabel = new BodyLabel(QString(), card);
        m_noteLabel->setObjectName(QStringLiteral("sxclLaunchNote"));
        m_noteLabel->setWordWrap(true);
        m_noteLabel->setAlignment(Qt::AlignHCenter);
        m_noteLabel->setVisible(false);
        layout->addWidget(m_noteLabel);

        // ④ 小贴士(轮播;从我们自己的表里取)
        m_tipLabel = new BodyLabel(QString(), card);
        m_tipLabel->setObjectName(QStringLiteral("sxclLaunchTip"));
        m_tipLabel->setWordWrap(true);
        m_tipLabel->setAlignment(Qt::AlignHCenter);
        layout->addWidget(m_tipLabel);

        m_vBox->addWidget(card);

        // ── 按钮 ──
        auto *btnLayout = new QHBoxLayout();
        m_cancelButton = new PushButton(QStringLiteral("取消"), m_view);
        applyButtonFont(m_cancelButton);
        connect(m_cancelButton, &QAbstractButton::clicked, this, [this] { onCancel(); });
        btnLayout->addWidget(m_cancelButton);
        // 「导出日志」:把启动器日志 + 游戏 latest.log + crash-reports 打成一个 zip。
        // 为什么放在启动页:用户遇到问题时人就在这一页(刚崩完),不该再去翻设置找入口。
        // (包里的 token/uuid/玩家名由核心库在写盘前打码)
        m_exportButton = new PushButton(QStringLiteral("导出日志"), m_view);
        applyButtonFont(m_exportButton);
        connect(m_exportButton, &QAbstractButton::clicked, this, [this] { onExportLogs(); });
        btnLayout->addWidget(m_exportButton);
        btnLayout->addStretch(1);
        m_backButton = new PrimaryPushButton(QStringLiteral("返回"), m_view);
        applyButtonFont(m_backButton);
        m_backButton->setEnabled(false);
        connect(m_backButton, &QAbstractButton::clicked, this, [this] { onBack(); });
        btnLayout->addWidget(m_backButton);
        m_vBox->addLayout(btnLayout);
        m_vBox->addStretch(1);

        // 小贴士轮播:第一条立刻显示,之后每 kTipIntervalMs 换下一条
        m_tips = launchTips();
        m_tipTimer = new QTimer(this);
        m_tipTimer->setObjectName(QStringLiteral("sxclLaunchTipTimer"));
        m_tipTimer->setInterval(kTipIntervalMs);
        connect(m_tipTimer, &QTimer::timeout, this, [this] { advanceTip(); });
        showTip(0);
        m_tipTimer->start();

        applyThemeStyles();
    }

    // ── 小贴士 ──
    void showTip(int index) {
        if (m_tips.isEmpty())
            return;
        m_tipIndex = ((index % m_tips.size()) + m_tips.size()) % m_tips.size();
        m_tipLabel->setText(m_tips.at(m_tipIndex));
    }

    /** 换下一条(顺次循环)。验收要"两次 dump 不一样",所以顺序必须确定。 */
    void advanceTip() { showTip(m_tipIndex + 1); }

    // ═══════════════ 启动接线(工作线程 + 信号回填)═══════════════

    QObject *tasksPage() const {
        QWidget *host = window();
        return host != nullptr ? host->findChild<QObject *>(QStringLiteral("TasksPage"))
                               : nullptr;
    }

    void updateTaskCard(int percent, const QString &status, const QString &detail) {
        QObject *page = tasksPage();
        if (page == nullptr) {
            uiTrace(QStringLiteral("task | 任务页(TasksPage)不在,任务卡 %1 没能更新").arg(m_taskKey));
            return;
        }
        QMetaObject::invokeMethod(page, "updateTask", Qt::DirectConnection,
                                  Q_ARG(QString, m_taskKey), Q_ARG(int, percent),
                                  Q_ARG(QString, status), Q_ARG(QString, detail));
        int count = -1;
        QMetaObject::invokeMethod(page, "taskCount", Qt::DirectConnection,
                                  Q_RETURN_ARG(int, count));
        uiTrace(QStringLiteral("task | %1 -> %2% | %3 | %4 | 任务页卡片数=%5")
                    .arg(m_taskKey)
                    .arg(percent)
                    .arg(status, detail)
                    .arg(count));
    }

    /** 游戏**正常退出**之后:把启动页收掉、回到主页(用户 2026-09-23 点名)。
     *
     *  为什么要留 1.6 秒:得让人看见"游戏退了"这件事,然后才收页 ——
     *  立刻跳走会让人以为"点了一下什么都没发生"。
     *  只有**成功**这一支才自动收:失败/取消要停在页上,原因得让人看清楚(那是要动手的)。
     *  还会再确认一次"这页眼下真的在前台" —— 用户要是自己先切走了,就别把页面从他手上抢走。 */
    void scheduleReturnHome() {
        QPointer<LaunchProgressPage> self(this);
        QTimer::singleShot(1600, this, [self]() {
            if (self == nullptr || !self->isVisible()) {
                return;
            }
            QWidget *w = self->window();
            if (w == nullptr) {
                return;
            }
            /* goBackFromLaunch = hideTempPage(true):回到进来之前那一页(从主页点启动就是主页);
             * 万一下层没这个方法,退回"版本列表"那条老路,绝不把用户丢在空白的临时页上。 */
            if (!QMetaObject::invokeMethod(w, "goBackFromLaunch", Qt::DirectConnection)) {
                (void)QMetaObject::invokeMethod(w, "goBackToVersions", Qt::DirectConnection);
            }
        });
    }

    void callTaskState(const char *method) {
        QObject *page = tasksPage();
        if (page == nullptr) {
            uiTrace(QStringLiteral("task | 任务页(TasksPage)不在,状态 %1 没能写进任务卡")
                        .arg(QString::fromLatin1(method)));
            return;
        }
        QMetaObject::invokeMethod(page, method, Qt::DirectConnection, Q_ARG(QString, m_taskKey));
        uiTrace(QStringLiteral("task | %1 -> %2()").arg(m_taskKey, QString::fromLatin1(method)));
    }

    /** 状态 + 进度(页面里**唯一**改这两样的地方,免得各处各写一句)。 */
    void setStatus(const QString &text, const QString &state, int percent) {
        m_barState = state;
        m_phaseLabel->setText(text);
        m_progressBar->setValue(percent);
        applyThemeStyles();
    }

    void showNote(const QString &text) {
        m_noteLabel->setText(text);
        m_noteLabel->setVisible(!text.isEmpty());
    }

    // 真的启动。**立刻返回**,主线程不阻塞。
    void startLaunch() {
        if (m_worker != nullptr || m_finished)
            return;

        // ── 0) 先确认这个版本**真的装好了**:versions/<id>/<id>.json 在不在 ──
        //
        // 为什么要有这一道(用户实测踩到):他点了"启动",页面一路跑下去,最后弹出
        // "读不到版本 JSON … 这个版本可能没装好"。**那不是一个错误,是一个前置条件** ——
        // 用户真正的诉求是"我想玩这个版本",那就该告诉他"先装它",并给一个能直接点的入口,
        // 而不是把一个内部错误报告(还带剪贴板复制)甩给他。
        // 判据与核心库 sxcl_instance_scan 一致:versions/<id>/ 下有 <id>.json 才算装好
        // (Forge 1.13+/Fabric 的实例没有自己的 jar,按 jar+json 判会全漏)。
        if (!instanceInstalled()) {
            showNeedInstall();
            return;
        }

        LaunchRequest request;
        request.gameDir = uiGameDirectory();
        request.versionName = m_versionId;
        request.javaPath = uiJavaPath();     // 空 -> 让核心库自己探测并排序(正常路径)
        request.memoryMb = uiMemoryMb();     // 0 -> 核心库按位数取默认
        request.settingsFile = uiSettingsFilePath();

        // ── 身份分两条路(判据与 home_page 的原逻辑一致)──
        //
        // A) 已登录账户可用 -> **调用现成入口** startAccountLaunch(dialogs/account.cpp)。
        //    账户快照 AccountSnapshot **故意不带 access_token**,令牌由账户模块自己从加密存储
        //    读出来直接交给核心库启动层,不经过界面 —— 所以本页根本拿不到令牌,也就无从泄漏。
        //    本页在交给它之前先跑一遍**只准备**(dry_run)的 LaunchWorker,把该准备的都准备好。
        //
        // B) 没有可用账户 -> 本页的 LaunchWorker 走完整流程(dry_run=0)。
        //    离线身份 = --offline <名字>(SXCL_UI_OFFLINE_NAME,默认 "Player")。
        const AccountSnapshot account = loadAccountSnapshot();
        // 主页那个"离线启动"键会把它置起来:这一次**强制离线**,哪怕账户可用也不用
        // (用户原话:「我们支持已经登陆正版的玩家以离线登录」)。读一次就清掉,不粘住。
        auto *mainWindow = qobject_cast<MainWindow *>(window());
        const bool forceOffline = (mainWindow != nullptr) && mainWindow->nextLaunchOffline();
        if (mainWindow != nullptr)
            mainWindow->setNextLaunchOffline(false);
        m_accountLaunch = !forceOffline && accountCanLaunch(account);
        const QString envOffline = qEnvironmentVariable("SXCL_UI_OFFLINE_NAME");
        const QString offlineId = !envOffline.isEmpty() ? envOffline : offlinePlayerName();
        m_identityText = m_accountLaunch
                             ? QStringLiteral("已登录账户 %1(玩家名 %2)")
                                   .arg(account.accountName, account.playerName)
                             : QStringLiteral("离线身份 %1%2")
                                   .arg(offlineId, forceOffline ? QStringLiteral("（你选的是离线启动）")
                                                                : QString());

        // 取证/验收通路:让启动页只准备不起进程(= CLI 的 sxcl-dl launch --dry-run)
        const bool forceDryRun = qEnvironmentVariableIntValue("SXCL_UI_LAUNCH_DRY_RUN") == 1;
        m_forceDryRun = forceDryRun;
        m_dryRunOnly = forceDryRun || m_accountLaunch;
        request.dryRun = m_dryRunOnly ? 1 : 0;
        if (!m_accountLaunch) {
            request.offlineName = offlineId;
            if (account.loggedIn) {
                m_identityText += QStringLiteral("(账户本次用不上:%1)")
                                      .arg(!account.hasMcToken || account.mcExpired
                                               ? QStringLiteral("凭据过期")
                                               : QStringLiteral("没有 Java 版档案"));
            }
        }

        m_worker = new LaunchWorker(std::move(request), this);
        connect(m_worker, &LaunchWorker::phaseChanged, this,
                [this](int index, int total, const QString &name) {
                    onPhaseChanged(index, total, name);
                });
        connect(m_worker, &LaunchWorker::javaInfo, this,
                [this](const QString &path, int major, const QString &version, int is64) {
                    // Java 探测结果**不再上界面**(用户点名去掉);它照样进运行日志与任务卡明细
                    onJavaInfo(path, major, version, is64);
                });
        connect(m_worker, &LaunchWorker::commandLine, this,
                [this](const QStringList &lines) { onCommandLine(lines); });
        // 启动前「补全文件」的实时读数(docs/24 §8):以前这一段是黑盒 ——
        // 几千个文件只在跑完之后报一行,用户看到的就是"卡住了"。
        connect(m_worker, &LaunchWorker::completeProgress, this,
                [this](int finished, int failed, const QString &label, qint64 done, qint64 total) {
                    onCompleteProgress(finished, failed, label, done, total);
                });
        connect(m_worker, &LaunchWorker::logLine, this,
                [this](const QString &text, const QString &kind, int severity, int isStderr) {
                    onLogLine(text, kind, severity, isStderr);
                });
        // 进程真的起来了:PID 立刻记下来(不等进程结束),并且能**单独结束它**
        connect(m_worker, &LaunchWorker::processStarted, this,
                [this](qint64 pid) { onProcessStarted(pid); });
        connect(m_worker, &LaunchWorker::finished, this,
                [this](bool ok, bool cancelled, bool dryRun, int exitCode, int timedOut, int killed,
                       const QString &conclusion, const QString &message, const QString &detail) {
                    onFinished(ok, cancelled, dryRun, exitCode, timedOut, killed, conclusion,
                               message, detail);
                });
        callTaskState("setTaskRunning");
        setStatus(QStringLiteral("正在准备游戏…"), QStringLiteral("running"), 0);
        showNote(QString());
        updateTaskCard(0, QStringLiteral("启动中"), m_identityText);
        m_worker->start(); // 起线程后立刻返回
    }

    void onPhaseChanged(int index, int total, const QString &name) {
        const int percent = total > 0 ? int((double(index) / double(total)) * 100.0) : 0;
        setStatus(launchPhaseText(index), QStringLiteral("running"), percent);
        updateTaskCard(percent, QStringLiteral("%1%").arg(percent), name);
    }

    /** 补全文件的实时读数:推进度条 + 写任务卡(阶段行已经不在这一页上了)。
     *  取消中就不再刷了 —— 用户已经点过取消,继续报"正在补 xxx"只会让人以为没理他。 */
    void onCompleteProgress(int finished, int failed, const QString &label, qint64 done,
                            qint64 total) {
        if (m_worker == nullptr || m_worker->cancelRequested()) {
            return;
        }
        if (!m_finished && total > 0) {
            // 准备阶段里进度条跟**真实字节**走(比"第几个阶段"实在得多)
            m_progressBar->setValue(int(double(done) * 100.0 / double(total)));
        }
        QString detail = QStringLiteral("正在补全文件:%1 件已落定").arg(finished);
        if (failed > 0) {
            detail += QStringLiteral("（失败 %1）").arg(failed);
        }
        if (!label.isEmpty()) {
            detail += QStringLiteral(" · %1").arg(QFileInfo(label).fileName());
        }
        if (total > 0) {
            detail += QStringLiteral("（%1%）").arg(int(double(done) * 100.0 / double(total)));
        }
        updateTaskCard(m_progressBar->value(), QStringLiteral("补全文件中"), detail);
    }

    /** Java 探测结果:**只进日志与任务卡明细**,界面上一处都不显示(用户点名去掉"Java 版本")。 */
    void onJavaInfo(const QString &path, int major, const QString &version, int is64) {
        if (path.isEmpty()) {
            SXCL_LOG_I("ui", "启动页:核心库没探测到可用的 Java");
            return;
        }
        SXCL_LOG_I("ui", "启动页:选中 Java %s(major=%d version=%s%s)", path.toUtf8().constData(),
                   major, version.toUtf8().constData(),
                   is64 == 1 ? " 64 位" : (is64 == 0 ? " 32 位" : " 位数未知"));
    }

    /** 最终命令行(**核心库自己打的、accessToken 已打码**):只落日志,不上界面。 */
    void onCommandLine(const QStringList &lines) {
        m_commandLineCount = lines.size();
        for (const QString &line : lines)
            SXCL_LOG_I("ui", "启动页 argv: %s", line.toUtf8().constData());
        if (lines.isEmpty())
            SXCL_LOG_I("ui", "启动页:核心库没有回传命令行(准备阶段没通过)");
    }

    // 一行游戏输出:不带界面负担,只为失败态记住"第一条问题行"。
    void onLogLine(const QString &text, const QString &kind, int severity, int isStderr) {
        Q_UNUSED(isStderr)
        m_logLineCount += 1;
        if (severity >= 2 || kind == QLatin1String("crash"))
            m_lastProblemLine = text;
    }

    void onFinished(bool ok, bool cancelled, bool dryRun, int exitCode, int timedOut, int killed,
                    const QString &conclusion, const QString &message, const QString &detail) {
        Q_UNUSED(timedOut)
        Q_UNUSED(killed)
        Q_UNUSED(exitCode) // 退出码不再往界面上摆(用户点名去掉技术信息);它在日志与错误详情里
        // 账户路径的第一段(只准备)成功了 -> 接着交给现成入口 startAccountLaunch。
        // forceDryRun 时**不**交接:用户明确只想要"准备",不该真起进程。
        if (ok && dryRun && m_accountLaunch && !m_forceDryRun && !m_accountHandedOff) {
            m_accountHandedOff = true;
            handOffToAccountLaunch();
            return;
        }
        m_finished = true;
        m_cancelButton->setEnabled(false);
        m_cancelButton->setText(QStringLiteral("取消"));
        m_backButton->setEnabled(true);

        if (ok && dryRun) {
            // 只准备不起进程(= CLI 的 --dry-run):没有退出码可言,准备成功就是成功
            m_spinner->setSpinning(false);
            setStatus(QStringLiteral("准备完成（未起进程）"), QStringLiteral("done"), 100);
            showNote(QString());
            callTaskState("setTaskDone");
            InfoBar::push(InfoBar::Type::Success, QStringLiteral("启动准备完成"), message, this,
                          4000);
            return;
        }

        if (ok) {
            m_spinner->setSpinning(false);
            setStatus(QStringLiteral("游戏已退出"), QStringLiteral("done"), 100);
            showNote(QString());
            callTaskState("setTaskDone");
            InfoBar::push(InfoBar::Type::Success, QStringLiteral("游戏已退出"), message, this,
                          4000);
            scheduleReturnHome(); // 游戏退了就回主页(不再卡在启动页)
            return;
        }

        if (cancelled) {
            m_spinner->setSpinning(false);
            setStatus(QStringLiteral("已取消"), QStringLiteral("failed"), m_progressBar->value());
            showNote(QString());
            callTaskState("setTaskFailed");
            updateTaskCard(100, QStringLiteral("已取消"), detail);
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("已取消"), message, this, 4000);
            return;
        }

        // 失败:摆出**核心库的真实原因**(不是"失败"两个字)+ 一条统一错误出口(完整上下文进剪贴板)
        m_spinner->setSpinning(false);
        setStatus(QStringLiteral("启动没成功"), QStringLiteral("failed"), m_progressBar->value());
        showNote(message);
        callTaskState("setTaskFailed");
        updateTaskCard(m_progressBar->value(), QStringLiteral("失败"), message);
        UiErrorContext ctx;
        ctx.page = QStringLiteral("启动页 / launch_%1").arg(m_versionId);
        ctx.action = QStringLiteral("启动 %1(%2)").arg(m_versionId, m_identityText);
        ctx.reason = message; // 核心库人话(含退出码/日志结论),不改写
        ctx.detail = QStringLiteral("结论=%1 · %2").arg(conclusion, detail);
        ctx.title = QStringLiteral("启动失败");
        pushUiError(this, ctx, 10000);
    }

    // 交给现成的"已登录账户启动"入口(dialogs/account.cpp 的 startAccountLaunch)。
    // **本页不碰令牌**:账户模块自己从加密存储读出来、直接交给核心库启动层。
    void handOffToAccountLaunch() {
        setStatus(QStringLiteral("正在启动游戏…"), QStringLiteral("running"), 40);
        m_spinner->setSpinning(true);
        updateTaskCard(40, QStringLiteral("正版启动中"), m_identityText);

        if (AccountLaunchTask *task =
                startAccountLaunch(uiGameDirectory(), m_versionId, uiJavaPath(), uiMemoryMb())) {
            connect(task, &AccountLaunchTask::finished, this,
                    [this](bool ok, const QString &title, const QString &detail) {
                        onAccountFinished(ok, title, detail);
                    });
            return;
        }
        // 现成入口拒绝启动(参数不合法)—— 如实报,不假装
        m_finished = true;
        m_cancelButton->setEnabled(false);
        m_backButton->setEnabled(true);
        m_spinner->setSpinning(false);
        setStatus(QStringLiteral("启动没成功"), QStringLiteral("failed"), m_progressBar->value());
        showNote(QStringLiteral("这次没能开始启动，请返回后重新试一次。"));
        callTaskState("setTaskFailed");
        UiErrorContext ctx;
        ctx.page = QStringLiteral("启动页 / launch_%1").arg(m_versionId);
        ctx.action = QStringLiteral("用已登录账户启动 %1").arg(m_versionId);
        ctx.reason = QStringLiteral("账户启动入口拒绝了这次参数");
        ctx.title = QStringLiteral("启动失败");
        pushUiError(this, ctx, 10000);
    }

    // 现成账户入口的结束回调(AccountLaunchTask::finished)
    void onAccountFinished(bool ok, const QString &title, const QString &detail) {
        m_finished = true;
        m_cancelButton->setEnabled(false);
        m_backButton->setEnabled(true);
        m_spinner->setSpinning(false);
        if (ok) {
            setStatus(QStringLiteral("游戏已退出"), QStringLiteral("done"), 100);
            showNote(QString());
            callTaskState("setTaskDone");
            InfoBar::push(InfoBar::Type::Success, title, detail, this, 5000);
            scheduleReturnHome(); // 正版那一路同理:退了就回主页
            return;
        }
        setStatus(QStringLiteral("启动没成功"), QStringLiteral("failed"), m_progressBar->value());
        showNote(title);
        callTaskState("setTaskFailed");
        updateTaskCard(m_progressBar->value(), QStringLiteral("失败"), title);
        UiErrorContext ctx;
        ctx.page = QStringLiteral("启动页 / launch_%1").arg(m_versionId);
        ctx.action = QStringLiteral("用已登录账户启动 %1").arg(m_versionId);
        ctx.reason = detail.isEmpty() ? title : detail;
        ctx.detail = QStringLiteral("账户入口结论:%1").arg(title);
        ctx.title = QStringLiteral("启动失败");
        pushUiError(this, ctx, 10000);
    }

    /** 进程起来了(on_started):界面只把状态改成"等待游戏…" —— PID 这类东西留给日志与任务卡。
     *  游戏真的在跑的时候,转圈动画**停下**(状态行已经说明在等什么了)。 */
    void onProcessStarted(qint64 pid) {
        m_gamePid = pid;
        setStatus(QStringLiteral("等待游戏…"), QStringLiteral("running"), m_progressBar->value());
        SXCL_LOG_I("ui", "启动页:游戏进程已启动 PID=%lld", (long long)pid);
        uiTrace(QStringLiteral("launch | 界面收到 pid=%1").arg(pid));
        updateTaskCard(m_progressBar->value(), QStringLiteral("运行中"),
                       QStringLiteral("PID %1").arg(pid));
    }

    // 这个版本还没装好:页面停在"未安装"态,取消键变成"去下载并安装"。
    // 不报错、不进剪贴板 —— 这是引导,不是故障。
    bool instanceInstalled() const {
        const QString path = QDir(uiGameDirectory())
                                 .filePath(QStringLiteral("versions/%1/%1.json").arg(m_versionId));
        return QFileInfo::exists(path);
    }

    void showNeedInstall() {
        m_finished = true;
        m_needInstall = true;
        m_spinner->setSpinning(false);
        setStatus(QStringLiteral("这个版本还没安装"), QStringLiteral("failed"), 0);
        showNote(QStringLiteral("版本列表只是可下载清单，装好之后才能启动。"));
        callTaskState("setTaskFailed");
        updateTaskCard(0, QStringLiteral("未安装"), QStringLiteral("先下载安装这个版本"));
        if (m_cancelButton != nullptr) {
            m_cancelButton->setText(QStringLiteral("去下载并安装"));
            m_cancelButton->setEnabled(true);
        }
        if (m_backButton != nullptr)
            m_backButton->setEnabled(true);
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("这个版本还没安装"),
                      QStringLiteral("版本列表是可下载清单。点「去下载并安装」装好它，再回来启动。"),
                      this, 8000);
    }

    void onCancel() {
        if (m_needInstall) {
            // "去下载并安装":把版本 id 交给下载配置页(它再让用户挑加载器)
            if (QMetaObject::invokeMethod(window(), "switchToDownloadConfig", Qt::DirectConnection,
                                          Q_ARG(QString, m_versionId)))
                return;
        }
        bool killed = false;
        if (m_worker != nullptr && !m_finished) {
            // cancel() 内部:置取消位 + 若已拿到 PID 就**直接按 PID 结束**(立刻生效,
            // 不必等下一次输出)。killedPid 用来如实告诉用户走的是哪条路。
            m_worker->cancel();
            killed = m_worker->runningPid() > 0;
        }
        m_cancelButton->setEnabled(false);
        m_cancelButton->setText(QStringLiteral("正在取消…"));
        updateTaskCard(m_progressBar->value(), QStringLiteral("正在取消…"),
                       killed ? QStringLiteral("已向 PID %1 发出终止请求").arg(m_gamePid)
                              : QStringLiteral("等进程下一次输出就停"));
    }

    // 「导出日志」:核心库负责打包与打码,界面只负责问路径、报结果。
    // **本页不碰令牌**:包里的 token/uuid/玩家名由 sxcl_logs_export 在写盘前换成 ***。
    void onExportLogs() {
        const QString gameDir = uiGameDirectory();
        const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        const QString baseDir = gameDir.isEmpty() ? QDir::homePath() : gameDir;
        const QString suggested =
            QDir(baseDir).filePath(QStringLiteral("sxcl-logs-%1.zip").arg(stamp));
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出日志包"), suggested, QStringLiteral("ZIP 压缩包 (*.zip)"));
        if (path.isEmpty())
            return; // 用户取消:什么都不做,也不提示

        const QByteArray gameUtf8 = QDir::fromNativeSeparators(gameDir).toUtf8();
        const QByteArray outUtf8 = QDir::fromNativeSeparators(path).toUtf8();
        const QByteArray logDirUtf8 = QDir::fromNativeSeparators(uiLauncherDataRoot() +
                                                                 QStringLiteral("/logs"))
                                          .toUtf8();
        sxcl_logs_export_request req;
        sxcl_logs_export_result res;
        char err[SXCL_LOGS_EXPORT_ERROR_MAX];
        std::memset(&req, 0, sizeof(req));
        std::memset(&res, 0, sizeof(res));
        err[0] = '\0';
        req.game_dir = gameUtf8.isEmpty() ? nullptr : gameUtf8.constData();
        req.log_dir = logDirUtf8.constData();
        req.out_zip = outUtf8.constData();
        const int rc = sxcl_logs_export(&req, &res, err, sizeof(err));
        if (rc == 0) {
            const QString detail = QStringLiteral("%1 · %2 个文件 · %3 字节")
                                       .arg(QString::fromUtf8(res.out_path))
                                       .arg(res.files)
                                       .arg(res.zip_bytes);
            SXCL_LOG_I("ui", "日志已导出:%s(文件 %d 个,zip %lld 字节,跳过 %d)", res.out_path,
                       res.files, res.zip_bytes, res.skipped);
            InfoBar::push(InfoBar::Type::Success, QStringLiteral("日志已导出"), detail, this, 6000);
        } else {
            const QString why =
                QString::fromUtf8(err[0] != '\0' ? err : "导出失败(核心库没给原因)");
            SXCL_LOG_E("ui", "日志导出失败:%s", why.toUtf8().constData());
            InfoBar::push(InfoBar::Type::Error, QStringLiteral("导出失败"), why, this, 8000);
        }
    }

    void onBack() { autoClose(); }

    void autoClose() {
        // Python: window.go_back_to_versions()(launch_page.py 里调的就是这个,
        // 不是 go_back_from_launch —— 后者是主窗口对外留的入口)
        if (QMetaObject::invokeMethod(window(), "goBackToVersions", Qt::DirectConnection))
            return;
        QMetaObject::invokeMethod(window(), "switchToRoute", Qt::DirectConnection,
                                  Q_ARG(QString, QStringLiteral("versions")));
    }

    QString m_versionId;
    QString m_barState = QStringLiteral("running");
    QWidget *m_view = nullptr;
    QVBoxLayout *m_vBox = nullptr;
    LoadingSpinner *m_spinner = nullptr;
    StrongBodyLabel *m_phaseLabel = nullptr;
    BodyLabel *m_noteLabel = nullptr;   // 出错时那一句人话(平时藏着)
    BodyLabel *m_tipLabel = nullptr;    // 轮播的小贴士
    QProgressBar *m_progressBar = nullptr;
    PushButton *m_cancelButton = nullptr;
    PushButton *m_exportButton = nullptr; // 导出日志包(打码由核心库做)
    PrimaryPushButton *m_backButton = nullptr;
    QTimer *m_tipTimer = nullptr;
    QStringList m_tips;
    int m_tipIndex = 0;

    LaunchWorker *m_worker = nullptr;     // 工作线程外壳(this 的子对象,析构时会 join)
    QString m_taskKey;                    // 任务页的键("launch_<版本名>")
    QString m_identityText;               // 本次用什么身份(只进任务卡明细)
    QString m_lastProblemLine;            // 日志里第一条错误行(失败态兜底)
    int m_commandLineCount = 0;           // 核心库回传的命令行行数(只记数,不上界面)
    bool m_finished = false;              // 终态已定
    bool m_needInstall = false;           // 停在"未安装"态(取消键改语义为"去下载并安装")
    bool m_accountLaunch = false;         // 走的是"已登录账户"那条路
    bool m_forceDryRun = false;           // SXCL_UI_LAUNCH_DRY_RUN=1(验收:只准备)
    bool m_dryRunOnly = false;            // 本次只准备不起进程(账户路径第一段也属于它)
    bool m_accountHandedOff = false;      // 已交给 startAccountLaunch
    int m_logLineCount = 0;               // 累计归类过的日志行数
    qint64 m_gamePid = -1;                // 游戏进程 PID(没起来 = -1;结束游戏时只动它)
};

} // namespace

QWidget *createLaunchPage(const VersionRef &version, QWidget *parent) {
    return new LaunchProgressPage(version, parent);
}

} // namespace sxcl::ui
