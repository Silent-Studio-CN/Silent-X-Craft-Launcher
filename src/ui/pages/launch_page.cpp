/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "page_factory.h"

#include <QAbstractButton>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstring>
#include <utility>

#include "sxcl/log.h"       // SXCL_LOG_I/E:导出结果进运行日志
#include "sxcl/logexport.h" // 一键导出日志包(token/uuid/用户名在写盘前就打码)

#include "game_folders.h"   // offlinePlayerName():主页那个离线 ID 输入框存下来的
#include "main_window.h"     // nextLaunchOffline():主页"离线启动"键的那一次
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
    button->setFont(font);
    button->setFixedHeight(32);
}

// ───────────────── 阶段指示点(launch_page.py:452-488 LaunchIndicator)────────────────

// 12x12 实心圆点,四态四色。颜色是 Python 里的字面量(不是主题令牌),逐字照抄:
//   DONE QColor(82,196,26) / ACTIVE QColor(0,120,212) / FAILED QColor(255,77,79)
//   PENDING QColor(200,200,200)
class LaunchIndicator : public QWidget {
public:
    enum State { Done = 0, Active = 1, Pending = 2, Failed = 3 };

    explicit LaunchIndicator(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(12, 12); // :462
    }

    void setState(State state) { // :465-467
        m_state = state;
        update();
    }

    State state() const { return m_state; }

protected:
    void paintEvent(QPaintEvent *) override { // :469-488
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRect r = rect().adjusted(1, 1, -1, -1); // :472
        switch (m_state) {
            case Done:
                painter.setBrush(QColor(82, 196, 26)); // :474 green
                break;
            case Active:
                painter.setBrush(QColor(0, 120, 212)); // :478 blue
                break;
            case Failed:
                painter.setBrush(QColor(255, 77, 79)); // :482 red
                break;
            case Pending:
            default:
                painter.setBrush(QColor(200, 200, 200)); // :486 gray
                break;
        }
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(r);
    }

private:
    State m_state = Pending; // :463 self._state = self.PENDING
};

// ───────────────── 页面本体(launch_page.py:494-679)────────────────

class LaunchProgressPage : public ScrollArea {
public:
    LaunchProgressPage(const VersionRef &version, QWidget *parent)
        : ScrollArea(parent), m_versionId(version.id) {
        // ---- BasePage(src/app/common/base_page.py:41-60)----
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

        m_vBox->addWidget(new TitleLabel(QStringLiteral("启动 %1").arg(m_versionId), m_view));
        auto *subtitle = new SubtitleLabel(QString(), m_view);
        subtitle->hide();                                    // :503 self.subtitleLabel.hide()
        m_vBox->addWidget(subtitle);

        buildContent();                                      // :508

        applyThemeStyles();                                  // :509-510 on_theme_changed(...)

        // 任务页的键:与 main_window.cpp:450-459 登记时用的完全一致
        m_taskKey = QStringLiteral("launch_") + m_versionId;

        // :511 self._start_launch() —— 真的启动(工作线程,主线程不阻塞)。
        // 用 0ms 单发而不是直接调:等事件循环转起来、页面已经能画之后再起,
        // 这样第一个 phase_changed 回来时界面已经就位。
        QTimer::singleShot(0, this, [this] { startLaunch(); });
    }

private:
    void styleProgress(const QString &state) { // :513-519
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        const QString color = state == QLatin1String("failed") ? tokens.danger.name()
                              : state == QLatin1String("done") ? tokens.success.name()
                                                               : tokens.accent.name();
        m_progressBar->setStyleSheet(
            QStringLiteral("QProgressBar { border: none; background: %1; border-radius: 2px; }"
                           "QProgressBar::chunk { background: %2; border-radius: 2px; }")
                .arg(FluentTheme::instance().tokenText(QStringLiteral("track")), color));
    }

    QString badgeColor() const { // :521-524
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        if (m_barState == QLatin1String("done"))
            return tokens.success.name();
        if (m_barState == QLatin1String("failed"))
            return tokens.danger.name();
        if (m_barState == QLatin1String("running"))
            return tokens.accent.name();
        return tokens.textSecondary.name();
    }

    void applyThemeStyles() { // :526-530
        styleProgress(m_barState);
        styleMonoView(); // 阶段 7 新增的两个等宽框(命令行/日志)也走同一套令牌
        m_statusBadge->setStyleSheet(QStringLiteral("color: %1; font-weight: 500;")
                                         .arg(badgeColor()));
        m_logOutput->setStyleSheet(
            QStringLiteral("color: %1;").arg(FluentTheme::instance().tokenText(
                QStringLiteral("textTertiary"))));
    }

    void buildContent() { // :532-598
        auto *card = new CardWidget(m_view);                 // :533
        card->setStyleSheet(QStringLiteral("CardWidget { border-radius: 8px; }")); // :534
        auto *layout = new QVBoxLayout(card);                // :535
        layout->setContentsMargins(28, 20, 28, 20);          // :536
        layout->setSpacing(12);                              // :537

        // ── 标题行(:539-546)──
        auto *titleRow = new QHBoxLayout();
        m_phaseLabel = new StrongBodyLabel(QStringLiteral("准备启动"), card); // :541
        titleRow->addWidget(m_phaseLabel);
        titleRow->addStretch(1);
        m_statusBadge = new BodyLabel(QStringLiteral("● 准备中"), card);      // :544
        titleRow->addWidget(m_statusBadge);
        layout->addLayout(titleRow);

        // ── 进度条(:548-554)──
        m_progressBar = new QProgressBar(card);              // :549
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
        m_progressBar->setFixedHeight(4);
        m_progressBar->setTextVisible(false);
        layout->addWidget(m_progressBar);

        // ── 阶段列表(:556-576)──
        QStringList phases;                                  // :557-563
        phases << QStringLiteral("检测 Java 运行时") << QStringLiteral("构建启动命令")
               << QStringLiteral("启动游戏进程")
               // :561 wait_window_phase:supports_window_detection() 在 Windows 上为真,
               // 否则退化成"等待游戏启动"(src/core/platform.py)
#ifdef Q_OS_WIN
               << QStringLiteral("等待游戏窗口")
#else
               << QStringLiteral("等待游戏启动")
#endif
               << QStringLiteral("运行完成");

        for (const QString &name : phases) {                 // :566-576
            auto *row = new QHBoxLayout();
            row->setSpacing(8);                              // :568
            auto *dot = new LaunchIndicator(card);
            row->addWidget(dot);
            auto *label = new BodyLabel(name, card);
            // :572 setTextColor(text_tertiary, text_disabled):深色主题下用 text_disabled
            label->setTextColor(FluentTheme::instance().tokens().textTertiary,
                                FluentTheme::instance().tokens().textDisabled);
            row->addWidget(label);
            row->addStretch(1);
            layout->addLayout(row);
            m_phaseWidgets.append({label, dot});
        }

        // ── 日志输出(:578-583)──
        m_logOutput = new BodyLabel(QString(), card);        // :579
        m_logOutput->setWordWrap(true);
        m_logOutput->setTextColor(FluentTheme::instance().tokens().textTertiary,
                                  FluentTheme::instance().tokens().textTertiary);
        m_logOutput->setFixedHeight(40);
        layout->addWidget(m_logOutput);

        // ── 阶段 7 新增:执行证据(Java 探测 / 最终命令行 / 游戏输出)──
        //
        // 这三块是**用户点名要看**的事实。它们不在 Python 参考图里 —— 参考图抓的是
        // "还没接线、worker 未起"的初始态;这一页现在真的会启动,所以必须交代:
        // 用哪个 Java、将要执行的完整命令行(accessToken 打码)、游戏每一行输出归到哪一类。
        // 几何/配色仍走同一套令牌,不引入新的视觉语言。
        m_javaLabel = new BodyLabel(QStringLiteral("Java:检测中…"), card);
        m_javaLabel->setWordWrap(true);
        m_javaLabel->setStyleSheet(QStringLiteral("font-size: 12px;"));
        layout->addWidget(m_javaLabel);

        layout->addWidget(makeSectionTitle(
            QStringLiteral("最终命令行（accessToken / 用户名 / UUID 已打码）"), card));
        m_commandView = makeMonoView(card, 132);
        layout->addWidget(m_commandView);

        layout->addWidget(makeSectionTitle(QStringLiteral("游戏输出（按核心库日志归类）"), card));
        m_logView = makeMonoView(card, 168);
        layout->addWidget(m_logView);

        m_vBox->addWidget(card);                             // :585 add_content(card)

        // ── 按钮(:587-598)──
        auto *btnLayout = new QHBoxLayout();
        m_cancelButton = new PushButton(QStringLiteral("取消"), m_view);   // :589
        applyButtonFont(m_cancelButton);
        connect(m_cancelButton, &QAbstractButton::clicked, this, [this] { onCancel(); });
        btnLayout->addWidget(m_cancelButton);
        // 「导出日志」:把启动器日志 + 游戏 latest.log + crash-reports 打成一个 zip。
        // 为什么放在启动页:用户遇到问题时人就在这一页(刚崩完),不该再去翻设置找入口。
        m_exportButton = new PushButton(QStringLiteral("导出日志"), m_view);
        applyButtonFont(m_exportButton);
        connect(m_exportButton, &QAbstractButton::clicked, this, [this] { onExportLogs(); });
        btnLayout->addWidget(m_exportButton);
        btnLayout->addStretch(1);
        m_backButton = new PrimaryPushButton(QStringLiteral("返回"), m_view); // :593
        applyButtonFont(m_backButton);
        m_backButton->setEnabled(false);                                      // :594
        connect(m_backButton, &QAbstractButton::clicked, this, [this] { onBack(); });
        btnLayout->addWidget(m_backButton);
        m_vBox->addLayout(btnLayout);                        // :597
        m_vBox->addStretch(1);                               // :598
    }

    // ═══════════════ 阶段 7:启动接线(工作线程 + 信号回填)═══════════════

    static StrongBodyLabel *makeSectionTitle(const QString &text, QWidget *host) {
        auto *label = new StrongBodyLabel(text, host);
        label->setStyleSheet(QStringLiteral("font-size: 13px;"));
        QFont font = label->font();
        font.setPixelSize(13);
        font.setWeight(QFont::DemiBold);
        label->setFont(font);
        return label;
    }

    // 只读的等宽文本框:命令行与日志都要"原样、可选中、可滚动",用富文本标签会吃掉空格。
    QPlainTextEdit *makeMonoView(QWidget *host, int height) {
        auto *view = new QPlainTextEdit(host);
        view->setReadOnly(true);
        view->setFixedHeight(height);
        view->setLineWrapMode(QPlainTextEdit::NoWrap);
        QFont mono = view->font();
        mono.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
                          QStringLiteral("DejaVu Sans Mono"), mono.family()});
        mono.setPixelSize(12);
        view->setFont(mono);
        return view;
    }

    void styleMonoView() { // 主题切换时重刷(颜色全部走令牌,不写字面量)
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        const QString qss =
            QStringLiteral("QPlainTextEdit { background: %1; color: %2;"
                           " border: 1px solid %3; border-radius: 6px; padding: 6px; }")
                .arg(tokens.inputBg.name(), tokens.text.name(), tokens.inputBorder.name());
        if (m_commandView != nullptr)
            m_commandView->setStyleSheet(qss);
        if (m_logView != nullptr)
            m_logView->setStyleSheet(qss);
    }

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

    // :511 self._start_launch() —— 真的启动。**立刻返回**,主线程不阻塞。
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
        //    为什么必须走它:账户快照 AccountSnapshot **故意不带 access_token**
        //    (account.h:38-55 只有 hasMcToken/mcExpired,uuid/玩家名,**没有令牌**)——
        //    令牌由账户模块自己从加密存储读出来直接交给核心库启动层,不经过界面。
        //    所以正版身份不是"本页要不要传令牌",而是"本页根本拿不到令牌",
        //    这也正是"别改账户代码、只调现成入口"的由来。
        //    本页在交给它之前先跑一遍**只准备**(dry_run)的 LaunchWorker:
        //    Java 探测结果、最终命令行、natives/目录检查因此照样能看到(命令行是离线形态,
        //    页面会写明"正版启动时令牌由账户模块注入")。
        //
        // B) 没有可用账户 -> 本页的 LaunchWorker 走完整流程(dry_run=0):
        //    Java 探测 -> 最终命令行 -> 真起进程 -> stdout/stderr 归类 -> 退出码。
        //    离线身份 = --offline <名字>(SXCL_UI_OFFLINE_NAME,默认 "Player")。
        const AccountSnapshot account = loadAccountSnapshot();
        // 主页那个"离线启动"键会把它置起来:这一次**强制离线**,哪怕账户可用也不用
        // (用户原话:「我们支持已经登陆正版的玩家以离线登录」)。读一次就清掉,不粘住。
        auto *mainWindow = qobject_cast<MainWindow *>(window());
        const bool forceOffline = (mainWindow != nullptr) && mainWindow->nextLaunchOffline();
        if (mainWindow != nullptr)
            mainWindow->setNextLaunchOffline(false);
        m_accountLaunch = !forceOffline && accountCanLaunch(account);
        // 离线 ID:主页那个输入框存下来的(launch.offline_name,**状态保留**);
        // 环境变量仍是取证/验收通路上的覆盖项(老行为不变)。
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
                    onJavaInfo(path, major, version, is64);
                });
        connect(m_worker, &LaunchWorker::commandLine, this,
                [this](const QStringList &lines) { onCommandLine(lines); });
        connect(m_worker, &LaunchWorker::logLine, this,
                [this](const QString &text, const QString &kind, int severity, int isStderr) {
                    onLogLine(text, kind, severity, isStderr);
                });
        // 进程真的起来了:PID 立刻显示出来(不等进程结束),并且能**单独结束它**
        connect(m_worker, &LaunchWorker::processStarted, this,
                [this](qint64 pid) { onProcessStarted(pid); });
        connect(m_worker, &LaunchWorker::finished, this,
                [this](bool ok, bool cancelled, bool dryRun, int exitCode, int timedOut,
                       int killed, const QString &conclusion, const QString &message,
                       const QString &detail) {
                    onFinished(ok, cancelled, dryRun, exitCode, timedOut, killed, conclusion,
                               message, detail);
                });
        callTaskState("setTaskRunning");
        updateTaskCard(0, QStringLiteral("启动中"), m_identityText);
        m_worker->start(); // 起线程后立刻返回
    }

    void onPhaseChanged(int index, int total, const QString &name) { // :608-626
        if (index > 0 && index - 1 < m_phaseWidgets.size()) {
            PhaseRow &prev = m_phaseWidgets[index - 1];
            prev.dot->setState(LaunchIndicator::Done);
            prev.label->setTextColor(FluentTheme::instance().tokens().success,
                                     FluentTheme::instance().tokens().success);
        }
        if (index >= 0 && index < m_phaseWidgets.size()) {
            PhaseRow &cur = m_phaseWidgets[index];
            cur.dot->setState(LaunchIndicator::Active);
            cur.label->setTextColor(FluentTheme::instance().tokens().accent,
                                    FluentTheme::instance().tokens().accent);
            m_phaseLabel->setText(name);
        }
        const int percent = total > 0 ? int((double(index) / double(total)) * 100.0) : 0;
        m_progressBar->setValue(percent);
        m_barState = QStringLiteral("running");
        m_statusBadge->setText(QStringLiteral("● 进行中"));
        applyThemeStyles();
        updateTaskCard(percent, QStringLiteral("%1%").arg(percent), name);
    }

    void onJavaInfo(const QString &path, int major, const QString &version, int is64) {
        if (path.isEmpty()) {
            m_javaLabel->setText(QStringLiteral("Java:没探测到可用的 Java(核心库没给路径)"));
            return;
        }
        QString text = QStringLiteral("Java:%1").arg(QDir::toNativeSeparators(path));
        if (!version.isEmpty())
            text += QStringLiteral("（Java %1）").arg(version);
        else if (major > 0)
            text += QStringLiteral("（Java %1）").arg(major);
        else
            text += QStringLiteral("（版本未知）");
        if (is64 == 1)
            text += QStringLiteral(" · 64 位");
        else if (is64 == 0)
            text += QStringLiteral(" · 32 位");
        m_javaLabel->setText(text);
    }

    // 最终命令行:**核心库自己打的**(driver.c:360-378),accessToken 已换成 ***。
    // 本页只负责显示,不自己拼一份 —— 自己拼的迟早与核心库漂移。
    void onCommandLine(const QStringList &lines) {
        if (lines.isEmpty()) {
            m_commandView->setPlainText(
                QStringLiteral("(核心库没有回传命令行 —— 这一般意味着准备阶段就没通过)"));
            return;
        }
        m_commandView->setPlainText(lines.join(QLatin1Char('\n')));
        m_commandView->verticalScrollBar()->setValue(0);
    }

    // 一行游戏输出:带核心库的归类标签(logscan),错误行也记下来给失败态用。
    void onLogLine(const QString &text, const QString &kind, int severity, int isStderr) {
        const QString tag = logKindTag(kind);
        const QString stream = isStderr != 0 ? QStringLiteral("err") : QStringLiteral("out");
        QString line = QStringLiteral("[%1][%2]%3 %4")
                           .arg(stream, tag,
                                severity >= 2 ? QStringLiteral("[!]") : QString(),
                                text);
        m_logView->appendPlainText(line);
        m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());
        m_logLineCount += 1;
        if (severity >= 2 || kind == QLatin1String("crash"))
            m_lastProblemLine = text;
    }

    // 核心库的稳定英文键 -> 中文短标签(logscan.c:637-651 是键的唯一来源)
    static QString logKindTag(const QString &kind) {
        if (kind == QLatin1String("graphics")) return QStringLiteral("图形");
        if (kind == QLatin1String("vulkan_fallback")) return QStringLiteral("Vulkan回退");
        if (kind == QLatin1String("java_version")) return QStringLiteral("Java版本");
        if (kind == QLatin1String("mod_loader")) return QStringLiteral("模组/加载器");
        if (kind == QLatin1String("missing")) return QStringLiteral("缺东西");
        if (kind == QLatin1String("account_net")) return QStringLiteral("账户/网络");
        if (kind == QLatin1String("crash")) return QStringLiteral("崩溃");
        if (kind == QLatin1String("exit_ok")) return QStringLiteral("正常退出");
        return QStringLiteral("其它");
    }

    void onFinished(bool ok, bool cancelled, bool dryRun, int exitCode, int timedOut, int killed,
                    const QString &conclusion, const QString &message,
                    const QString &detail) { // :639-665
        Q_UNUSED(timedOut)
        Q_UNUSED(killed)
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
            for (const PhaseRow &row : m_phaseWidgets) {
                row.dot->setState(LaunchIndicator::Done);
                row.label->setTextColor(FluentTheme::instance().tokens().success,
                                        FluentTheme::instance().tokens().success);
            }
            m_barState = QStringLiteral("done");
            applyThemeStyles();
            m_progressBar->setValue(100);
            m_phaseLabel->setText(QStringLiteral("准备完成（未起进程）"));
            m_statusBadge->setText(QStringLiteral("✓ 已准备"));
            m_logOutput->setText(detail.left(80));
            m_logView->appendPlainText(QStringLiteral("[info] %1").arg(message));
            callTaskState("setTaskDone");
            InfoBar::push(InfoBar::Type::Success, QStringLiteral("启动准备完成"), message, this,
                          4000);
            return;
        }

        if (ok) {
            for (const PhaseRow &row : m_phaseWidgets) {
                row.dot->setState(LaunchIndicator::Done);
                row.label->setTextColor(FluentTheme::instance().tokens().success,
                                        FluentTheme::instance().tokens().success);
            }
            m_barState = QStringLiteral("done");
            applyThemeStyles();
            m_progressBar->setValue(100);
            m_phaseLabel->setText(QStringLiteral("游戏已退出"));
            m_statusBadge->setText(QStringLiteral("✓ 退出码 0"));
            m_logOutput->setText(QStringLiteral("退出码 0 | %1").arg(detail).left(80));
            m_logView->appendPlainText(QStringLiteral("[info] 游戏已退出(退出码 0)"));
            callTaskState("setTaskDone");
            InfoBar::push(InfoBar::Type::Success, QStringLiteral("游戏已退出"), message, this,
                          4000);
            return;
        }

        m_barState = QStringLiteral("failed");
        applyThemeStyles();
        if (cancelled) {
            m_statusBadge->setText(QStringLiteral("⊘ 已取消"));
            m_statusBadge->setStyleSheet(
                QStringLiteral("color: %1; font-weight: 500;")
                    .arg(FluentTheme::instance().tokenText(QStringLiteral("warning"))));
            m_phaseLabel->setText(QStringLiteral("已取消"));
            m_logView->appendPlainText(QStringLiteral("[warn] %1 | %2").arg(message, detail));
            m_logOutput->setText(message.left(80));
            callTaskState("setTaskFailed");
            updateTaskCard(100, QStringLiteral("已取消"), detail);
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("已取消"), message, this, 4000);
            return;
        }

        // 失败:当前阶段标红,并把**核心库的真实原因**摆出来(不是"失败"两个字)
        for (const PhaseRow &row : m_phaseWidgets) {
            if (row.dot->state() == LaunchIndicator::Active) {
                row.dot->setState(LaunchIndicator::Failed);
                row.label->setTextColor(FluentTheme::instance().tokens().danger,
                                        FluentTheme::instance().tokens().danger);
                break;
            }
        }
        m_statusBadge->setText(QStringLiteral("✗ 失败"));
        m_statusBadge->setStyleSheet(QStringLiteral("color: %1; font-weight: 500;")
                                         .arg(FluentTheme::instance().tokenText(
                                             QStringLiteral("danger"))));
        m_phaseLabel->setText(message.left(40));
        m_logOutput->setText(message.left(80));
        m_logView->appendPlainText(
            QStringLiteral("[error] 退出码 %1 · 结论 %2\n[error] %3\n[error] %4")
                .arg(exitCode)
                .arg(conclusion)
                .arg(message, detail));
        if (!m_lastProblemLine.isEmpty())
            m_logView->appendPlainText(QStringLiteral("[error] 日志里的第一条问题:%1")
                                           .arg(m_lastProblemLine.left(200)));
        callTaskState("setTaskFailed");
        updateTaskCard(m_progressBar->value(), QStringLiteral("失败"), message);
        // 统一错误出口(与下载进度页同一个):完整上下文进剪贴板
        UiErrorContext ctx;
        ctx.page = QStringLiteral("启动页 / launch_%1").arg(m_versionId);
        ctx.action = QStringLiteral("启动 %1(%2)").arg(m_versionId, m_identityText);
        ctx.reason = message; // 核心库人话(含退出码/日志结论),不改写
        ctx.detail = QStringLiteral("结论=%1 · %2").arg(conclusion, detail);
        ctx.title = QStringLiteral("启动失败");
        pushUiError(this, ctx, 10000);
    }

    // 交给现成的"已登录账户启动"入口(dialogs/account.cpp 的 startAccountLaunch)。
    // **本页不碰令牌**:账户模块自己从加密存储读出来、直接交给核心库启动层
    // (account.h:128-131 写明"sxcl 的启动层负责不打印")。
    void handOffToAccountLaunch() {
        for (int i = 0; i < m_phaseWidgets.size(); ++i) { // 0/1 已完成,2 交给账户入口
            PhaseRow &row = m_phaseWidgets[i];
            if (i < 2) {
                row.dot->setState(LaunchIndicator::Done);
                row.label->setTextColor(FluentTheme::instance().tokens().success,
                                        FluentTheme::instance().tokens().success);
            } else if (i == 2) {
                row.dot->setState(LaunchIndicator::Active);
                row.label->setTextColor(FluentTheme::instance().tokens().accent,
                                        FluentTheme::instance().tokens().accent);
            }
        }
        m_barState = QStringLiteral("running");
        applyThemeStyles();
        m_progressBar->setValue(40);
        m_phaseLabel->setText(QStringLiteral("启动游戏进程（正版身份）"));
        m_statusBadge->setText(QStringLiteral("● 正版启动中"));
        m_logView->appendPlainText(QStringLiteral(
            "[info] 准备完成。正版身份交给账户入口执行:access_token 由账户模块从加密存储读取后"
            "直接交给核心库启动层,不经过启动页(所以上面命令行是离线形态,已打码)。"));
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
        m_barState = QStringLiteral("failed");
        applyThemeStyles();
        m_phaseLabel->setText(QStringLiteral("正版启动入口拒绝启动"));
        m_statusBadge->setText(QStringLiteral("✗ 失败"));
        m_logView->appendPlainText(
            QStringLiteral("[error] startAccountLaunch 返回空(参数不合法:目录/版本/Java 路径)"));
        callTaskState("setTaskFailed");
        UiErrorContext ctx;
        ctx.page = QStringLiteral("启动页 / launch_%1").arg(m_versionId);
        ctx.action = QStringLiteral("用已登录账户启动 %1").arg(m_versionId);
        ctx.reason = QStringLiteral("startAccountLaunch 返回空(参数不合法:游戏目录/版本名/Java 路径)");
        ctx.title = QStringLiteral("启动失败");
        pushUiError(this, ctx, 10000);
    }

    // 现成账户入口的结束回调(AccountLaunchTask::finished)
    void onAccountFinished(bool ok, const QString &title, const QString &detail) {
        m_finished = true;
        m_cancelButton->setEnabled(false);
        m_backButton->setEnabled(true);
        m_barState = ok ? QStringLiteral("done") : QStringLiteral("failed");
        applyThemeStyles();
        if (ok) {
            for (const PhaseRow &row : m_phaseWidgets) {
                row.dot->setState(LaunchIndicator::Done);
                row.label->setTextColor(FluentTheme::instance().tokens().success,
                                        FluentTheme::instance().tokens().success);
            }
            m_progressBar->setValue(100);
            m_phaseLabel->setText(QStringLiteral("游戏已退出"));
            m_statusBadge->setText(QStringLiteral("✓ 退出码 0"));
            m_logView->appendPlainText(QStringLiteral("[info] %1 · %2").arg(title, detail));
            callTaskState("setTaskDone");
            InfoBar::push(InfoBar::Type::Success, title, detail, this, 5000);
            return;
        }
        for (const PhaseRow &row : m_phaseWidgets) {
            if (row.dot->state() == LaunchIndicator::Active) {
                row.dot->setState(LaunchIndicator::Failed);
                row.label->setTextColor(FluentTheme::instance().tokens().danger,
                                        FluentTheme::instance().tokens().danger);
                break;
            }
        }
        m_phaseLabel->setText(title.left(40));
        m_statusBadge->setText(QStringLiteral("✗ 失败"));
        m_logView->appendPlainText(QStringLiteral("[error] %1 · %2").arg(title, detail));
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

    // 进程起来了(on_started -> LaunchWorker::processStarted)。
    // 这一步的意义:在此之前界面只能"猜"进程到底起没起来 —— 现在有 PID 可显示、
    // 可核对、可**单独结束**(启动器自己不受影响)。
    void onProcessStarted(qint64 pid) {
        m_gamePid = pid;
        m_phaseLabel->setText(QStringLiteral("等待游戏窗口"));
        m_statusBadge->setText(QStringLiteral("● 运行中 · PID %1").arg(pid));
        m_logView->appendPlainText(
            QStringLiteral("[info] 游戏进程已启动：PID %1（独立进程；结束游戏不会退出启动器）").arg(pid));
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
        m_phaseLabel->setText(QStringLiteral("这个版本还没安装"));
        m_statusBadge->setText(QStringLiteral("⊘ 未安装"));
        m_barState = QStringLiteral("failed");
        applyThemeStyles();
        m_logView->appendPlainText(
            QStringLiteral("[info] 找不到 %1 。版本列表只是**可下载清单**,装好之后才能启动。")
                .arg(QDir::toNativeSeparators(
                    QDir(uiGameDirectory())
                        .filePath(QStringLiteral("versions/%1/%1.json").arg(m_versionId)))));
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

    void onCancel() { // :667-671
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
            SXCL_LOG_I("ui", "日志已导出:%s(文件 %d 个,zip %lld 字节,跳过 %d)",
                       res.out_path, res.files, res.zip_bytes, res.skipped);
            m_logView->appendPlainText(QStringLiteral("[info] 日志已导出:%1").arg(detail));
            InfoBar::push(InfoBar::Type::Success, QStringLiteral("日志已导出"), detail, this, 6000);
        } else {
            const QString why =
                QString::fromUtf8(err[0] != '\0' ? err : "导出失败(核心库没给原因)");
            SXCL_LOG_E("ui", "日志导出失败:%s", why.toUtf8().constData());
            m_logView->appendPlainText(QStringLiteral("[error] 日志导出失败:%1").arg(why));
            InfoBar::push(InfoBar::Type::Error, QStringLiteral("导出失败"), why, this, 8000);
        }
    }

    void onBack() { autoClose(); } // :673-674

    void autoClose() { // :676-679
        // Python: window.go_back_to_versions()(launch_page.py 里调的就是这个,
        // 不是 go_back_from_launch —— 后者是主窗口对外留的入口)
        if (QMetaObject::invokeMethod(window(), "goBackToVersions", Qt::DirectConnection))
            return;
        QMetaObject::invokeMethod(window(), "switchToRoute", Qt::DirectConnection,
                                  Q_ARG(QString, QStringLiteral("versions")));
    }

    struct PhaseRow {
        BodyLabel *label = nullptr;
        LaunchIndicator *dot = nullptr;
    };

    QString m_versionId;
    QString m_barState = QStringLiteral("running"); // :506
    QWidget *m_view = nullptr;
    QVBoxLayout *m_vBox = nullptr;
    StrongBodyLabel *m_phaseLabel = nullptr;
    BodyLabel *m_statusBadge = nullptr;
    QProgressBar *m_progressBar = nullptr;
    BodyLabel *m_logOutput = nullptr;
    PushButton *m_cancelButton = nullptr;
    PushButton *m_exportButton = nullptr; // 导出日志包(打码由核心库做)
    PrimaryPushButton *m_backButton = nullptr;
    QVector<PhaseRow> m_phaseWidgets = {};

    // ── 阶段 7 新增:执行证据的三个控件 ──
    BodyLabel *m_javaLabel = nullptr;          // Java 探测结果
    QPlainTextEdit *m_commandView = nullptr;   // 最终命令行(核心库打码后的)
    QPlainTextEdit *m_logView = nullptr;       // stdout/stderr(带核心库归类标签)

    // ── 阶段 7 新增:执行侧 ──
    LaunchWorker *m_worker = nullptr;     // 工作线程外壳(this 的子对象,析构时会 join)
    QString m_taskKey;                    // 任务页的键("launch_<版本名>")
    QString m_identityText;               // 本次用什么身份(显示 + 任务卡明细)
    QString m_lastProblemLine;            // 日志里第一条错误行(失败态兜底)
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
