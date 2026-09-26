/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "page_factory.h"

#include <QAbstractButton>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "main_window.h" // 验收通路:点侧栏「任务」按钮要用 navPanel()/currentRouteKey()

#include "workers/install_worker.h"
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

// ───────────────── 阶段指示器(download_progress_page.py:67-131 StageIndicator)─────────────

// 20x20:idle 空心圆 / spinning 旋转弧 / done 对号 / failed 叉。
// 四个颜色是 Python 里的字面量(QColor(200,200,200) / (0,120,212) / (82,196,26) / (255,77,79)),
// 不是主题令牌 —— 移植时逐字照抄,不自作主张换令牌。
class StageIndicator : public QWidget {
public:
    enum State { Idle, Spinning, Done, Failed };

    explicit StageIndicator(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(20, 20);                       // :73
        m_timer = new QTimer(this);                 // :74-77
        m_timer->setInterval(50);
        connect(m_timer, &QTimer::timeout, this, [this] {
            m_angle = (m_angle + 12) % 360;         // :88-90
            update();
        });
    }

    void setState(State state) {                    // :79-86
        m_state = state;
        if (state == Spinning)
            m_timer->start();
        else
            m_timer->stop();
        update();
    }

    State state() const { return m_state; }

protected:
    void paintEvent(QPaintEvent *) override {       // :92-131
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRect rect = this->rect();
        const QPoint center = rect.center();
        const int radius = qMin(rect.width(), rect.height()) / 2 - 2; // :98
        const int lineWidth = 2;                                     // :99

        switch (m_state) {
            case Idle: // :101-105 空心圆
                painter.setPen(QPen(QColor(200, 200, 200), lineWidth));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(center, radius, radius);
                break;
            case Spinning: { // :107-112 旋转环(270° 弧,每 50ms 转 12°)
                painter.setPen(QPen(QColor(0, 120, 212), lineWidth, Qt::SolidLine, Qt::RoundCap));
                const int spanAngle = 270 * 16;
                const int startAngle = m_angle * 16;
                painter.drawArc(rect.adjusted(lineWidth, lineWidth, -lineWidth, -lineWidth),
                                startAngle, spanAngle);
                break;
            }
            case Done: { // :114-123 对号
                painter.setPen(QPen(QColor(82, 196, 26), 2.5, Qt::SolidLine, Qt::RoundCap));
                painter.setBrush(Qt::NoBrush);
                QPainterPath path;
                path.moveTo(center.x() - 5, center.y());
                path.lineTo(center.x() - 1, center.y() + 5);
                path.lineTo(center.x() + 6, center.y() - 5);
                painter.drawPath(path);
                break;
            }
            case Failed: { // :125-131 叉
                painter.setPen(QPen(QColor(255, 77, 79), 2.5, Qt::SolidLine, Qt::RoundCap));
                painter.setBrush(Qt::NoBrush);
                const int offset = 5;
                painter.drawLine(center.x() - offset, center.y() - offset, center.x() + offset,
                                 center.y() + offset);
                painter.drawLine(center.x() + offset, center.y() - offset, center.x() - offset,
                                 center.y() + offset);
                break;
            }
        }
    }

private:
    State m_state = Idle;    // :72 self._state = "idle"
    int m_angle = 0;         // :71
    QTimer *m_timer = nullptr;
};

// ───────────────── 页面本体(download_progress_page.py:142-429)────────────────

class DownloadProgressPage : public ScrollArea {
public:
    DownloadProgressPage(const VersionRef &version, const QString &versionName,
                         const QString &loaderType, const QString &loaderVersion,
                         QWidget *parent)
        : ScrollArea(parent),
          m_versionId(version.id),
          m_versionName(versionName),
          m_loaderType(loaderType),
          m_loaderVersion(loaderVersion) {
        // ---- BasePage(src/app/common/base_page.py:41-60)----
        setObjectName(QStringLiteral("DownloadProgressPage")); // :43 self.__class__.__name__
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

        m_vBox->addWidget(new TitleLabel(QStringLiteral("正在安装 %1").arg(m_versionName), m_view));
        auto *subtitle = new SubtitleLabel(QString(), m_view);
        subtitle->hide();                                       // :158 self.subtitleLabel.hide()
        m_vBox->addWidget(subtitle);

        buildContent();                                          // :170

        // :171-172 on_theme_changed(self._apply_theme_styles) —— 立刻跑一次
        applyThemeStyles();

        // 任务页的键:与 main_window.cpp:437-446 登记时用的完全一致
        m_taskKey = QStringLiteral("download_progress_") + m_versionName;

        // :173 self._start_installation() —— 真的起安装(工作线程,主线程不阻塞)。
        // 构造期不直接调:等事件循环转起来再起,这样"页面已经能画"之后才会有信号回来,
        // 也保证 Worker 的队列投递有接收者。
        // (队列投递的目标是 this,this 已经构造完 -> 用 0ms 单发是安全的)
        QTimer::singleShot(0, this, [this] { startInstallation(); });
    }

private:
    // :175-182 进度条配色(主题切换时会重新调用;失败态用 danger 色)
    void styleProgress(const QString &state) {
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        const QString color = state == QLatin1String("failed") ? tokens.danger.name()
                              : state == QLatin1String("done") ? tokens.success.name()
                                                               : tokens.accent.name();
        m_progressBar->setStyleSheet(
            QStringLiteral("QProgressBar { border: none; background: %1; border-radius: 3px; }"
                           "QProgressBar::chunk { background: %2; border-radius: 3px; }")
                .arg(FluentTheme::instance().tokenText(QStringLiteral("track")), color));
    }

    QString badgeColor() const { // :184-187
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        if (m_barState == QLatin1String("done"))
            return tokens.success.name();
        if (m_barState == QLatin1String("failed"))
            return tokens.danger.name();
        if (m_barState == QLatin1String("running"))
            return tokens.accent.name();
        return tokens.textSecondary.name();
    }

    // :189-195。**注意**:Python 这一段的第二行是 self._divider.setStyleSheet(...),而
    // self._divider 在 _build_content 里从来没建过(_build_content:238-239 的"分隔线"
    // 那段是空注释),on_theme_changed 又把异常吞掉(theme.py:236-240)——
    // 于是后面两行(status_badge / detail_label)在 Python 里**永远不会被重刷**。
    // 移植时如实保留这个结果:只刷进度条。参考图里 detail_label 的 QSS 就停在
    // _build_content 设的 "font-size: 12px;"(没有 color),与此吻合。
    void applyThemeStyles() {
        styleProgress(m_barState);
    }

    void buildContent() { // :197-301
        auto *card = new CardWidget(m_view);                     // :199
        m_card = card;
        card->setStyleSheet(QStringLiteral("CardWidget { border-radius: 8px; }")); // :200
        auto *layout = new QVBoxLayout(card);                    // :201
        layout->setContentsMargins(32, 24, 32, 24);              // :202
        layout->setSpacing(12);                                  // :203

        // ---- 标题行(:205-215)----
        auto *titleLayout = new QHBoxLayout();
        m_stageLabel = new StrongBodyLabel(QStringLiteral("准备安装"), card); // :207
        m_stageLabel->setStyleSheet(QStringLiteral("font-size: 15px;"));      // :208
        {
            QFont font = m_stageLabel->font();
            font.setPixelSize(15);
            font.setWeight(QFont::DemiBold);
            m_stageLabel->setFont(font);
        }
        titleLayout->addWidget(m_stageLabel);
        titleLayout->addStretch(1);

        m_statusBadge = new BodyLabel(QStringLiteral("进行中"), card);      // :212
        m_statusBadge->setStyleSheet(QStringLiteral("color: %1; font-weight: 500;")
                                         .arg(FluentTheme::instance().tokenText(
                                             QStringLiteral("accent"))));      // :213
        titleLayout->addWidget(m_statusBadge);
        layout->addLayout(titleLayout);

        // ---- 进度条(:217-223)----
        m_progressBar = new QProgressBar(card);                  // :218
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
        m_progressBar->setFixedHeight(6);
        m_progressBar->setTextVisible(false);
        layout->addWidget(m_progressBar);

        // ---- 进度百分比(:225-229)----
        m_percentLabel = new BodyLabel(QStringLiteral("0%"), card); // :226
        m_percentLabel->setTextColor(FluentTheme::instance().tokens().textTertiary);
        m_percentLabel->setAlignment(Qt::AlignRight);
        layout->addWidget(m_percentLabel);

        // ---- 详情行(:231-236)----
        m_detailLabel = new BodyLabel(QString(), card);          // :232
        m_detailLabel->setTextColor(FluentTheme::instance().tokens().textTertiary);
        m_detailLabel->setWordWrap(true);
        m_detailLabel->setStyleSheet(QStringLiteral("font-size: 12px;")); // :235 顶掉上面的颜色
        layout->addWidget(m_detailLabel);

        // ---- 阶段列表(:240-282)----
        m_stageListLayout = new QVBoxLayout();
        m_stageListLayout->setSpacing(6);
        m_stageListLayout->setContentsMargins(0, 8, 0, 0);

        QStringList stages;                                      // :246-262
        stages << QStringLiteral("下载原版 json 文件")
               << QStringLiteral("下载原版 client.jar")
               << QStringLiteral("下载原版支持库文件")
               << QStringLiteral("下载原版资源文件");
        if (m_loaderType != QLatin1String("none")) {
            stages << QStringLiteral("下载加载器") << QStringLiteral("分析加载器依赖")
                   << QStringLiteral("下载加载器依赖库") << QStringLiteral("执行加载器安装");
        }
        stages << QStringLiteral("整理文件") << QStringLiteral("安装完成");

        // :266-280 —— 构造期先摆 Python 那份行表(参考图就是这个状态);
        // 计划一回来(planReady)就换成**核心库的阶段表**,见 applyPlan()。
        for (const QString &name : stages)
            appendStageRow(name);
        layout->addLayout(m_stageListLayout);
        m_vBox->addWidget(card);                                 // :283 add_content(card)

        // ---- 按钮(:285-301)----
        auto *btnLayout = new QHBoxLayout();
        btnLayout->setSpacing(12);                               // :287
        m_cancelButton = new PushButton(QStringLiteral("取消"), m_view);  // :289
        applyButtonFont(m_cancelButton);
        connect(m_cancelButton, &QAbstractButton::clicked, this, [this] { onCancel(); });
        btnLayout->addWidget(m_cancelButton);

        btnLayout->addStretch(1);

        m_backButton = new PrimaryPushButton(QStringLiteral("返回"), m_view); // :295
        applyButtonFont(m_backButton);
        m_backButton->setEnabled(false);                                      // :296
        connect(m_backButton, &QAbstractButton::clicked, this, [this] { onBack(); });
        btnLayout->addWidget(m_backButton);

        m_vBox->addLayout(btnLayout);                            // :300
        m_vBox->addStretch(1);                                   // :301
    }

    // ═══════════════ 阶段 7:安装接线(工作线程 + 信号回填)═══════════════

    // 阶段行:一行 = 指示器 + 文案(几何与 buildContent 里原来那份完全一致)
    void appendStageRow(const QString &name) {
        QWidget *host = m_card != nullptr ? static_cast<QWidget *>(m_card) : m_view;
        auto *row = new QHBoxLayout();
        row->setSpacing(10);                                     // :268
        auto *indicator = new StageIndicator(host);
        row->addWidget(indicator);
        auto *label = new BodyLabel(name, host);
        label->setTextColor(FluentTheme::instance().tokens().textTertiary,
                            FluentTheme::instance().tokens().textDisabled); // :274
        label->setStyleSheet(QStringLiteral("font-size: 13px;"));           // :275
        {
            QFont font = label->font();
            font.setPixelSize(13);
            font.setWeight(QFont::Normal);
            label->setFont(font);
        }
        row->addWidget(label);
        row->addStretch(1);
        m_stageListLayout->addLayout(row);
        m_stageWidgets.append({label, indicator});
    }

    QObject *tasksPage() const {
        QWidget *host = window();
        return host != nullptr ? host->findChild<QObject *>(QStringLiteral("TasksPage"))
                               : nullptr;
    }

    // 任务页联动。任务卡片由 main_window.cpp:445 用同一个 id 登记("download_progress_<版本名>"),
    // 这里只按 id 更新它 —— 对应 Python download_progress_page.py:371-378。
    void updateTaskCard(int percent, const QString &status, const QString &detail) {
        QObject *page = tasksPage();
        if (page == nullptr) {
            uiTrace(QStringLiteral("task | 任务页(TasksPage)不在,任务卡 %1 没能更新").arg(m_taskKey));
            return;
        }
        QMetaObject::invokeMethod(page, "updateTask", Qt::DirectConnection,
                                  Q_ARG(QString, m_taskKey), Q_ARG(int, percent),
                                  Q_ARG(QString, status), Q_ARG(QString, detail));
        // 证据(SXCL_UI_TRACE=1):任务卡的**真实**状态。taskCount 从任务页自己读回来,
        // 所以"调了但没人接"这种情况不会冒充成"联动成功"。
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

    // :173 self._start_installation() —— 真的起安装。**立刻返回**,主线程不阻塞。
    void startInstallation() {
        if (m_worker != nullptr || m_finished)
            return;
        InstallRequest request;
        request.gameDir = uiGameDirectory();       // SXCL_UI_GAME_DIR > 设置 > 平台默认
        request.versionId = m_versionId;           // 去版本清单里找的那个 MC 版本号
        request.instanceName = m_versionName;      // 装到 versions/<版本名>/
        request.loaderType = m_loaderType;         // none/forge/neoforge/fabric/optifine
        request.loaderVersion = m_loaderVersion;
        request.javaPath = uiJavaPath();           // 含加载器时安装器要用它
        request.settingsFile = uiSettingsFilePath();
        request.assetsLevel = QStringLiteral("default"); // 全量(与 Python 一致)

        m_worker = new InstallWorker(std::move(request), this);
        connect(m_worker, &InstallWorker::planReady, this,
                [this](const QStringList &ids, const QStringList &names) { applyPlan(ids, names); });
        connect(m_worker, &InstallWorker::stageChanged, this,
                [this](int index, int total, const QString &id, const QString &name) {
                    onStageChanged(index, total, id, name);
                });
        connect(m_worker, &InstallWorker::progress, this,
                [this](const InstallProgress &update) { onProgress(update); });
        connect(m_worker, &InstallWorker::logLine, this,
                [this](const QString &line) { m_lastLogLine = line; });
        connect(m_worker, &InstallWorker::finished, this,
                [this](bool ok, bool cancelled, int code, bool retryable, const QString &message,
                       const QString &detail) {
                    onFinished(ok, cancelled, code, retryable, message, detail);
                });
        callTaskState("setTaskRunning");
        updateTaskCard(0, QStringLiteral("准备中"), QStringLiteral("正在获取版本清单…"));
        m_worker->start(); // 起线程后立刻返回

        // 验收通路(SXCL_UI_TASKS_AFTER_MS=<毫秒>):安装开始 N 毫秒后切到任务页 ——
        // 走的是**真正的用户路径**:点侧栏「任务」那个导航按钮(nav.cpp:238-241 的
        // NavigationWidget::clicked -> emit routeChanged),不是绕过界面直接设栈。
        // 只在设置了环境变量时生效,不影响正常使用。
        const int tasksAfter = qEnvironmentVariableIntValue("SXCL_UI_TASKS_AFTER_MS");
        if (tasksAfter > 0) {
            QTimer::singleShot(tasksAfter, this, [this] {
                auto *host = qobject_cast<MainWindow *>(window());
                if (host == nullptr || host->navPanel() == nullptr) {
                    uiTrace(QStringLiteral("task | 切任务页**失败**:拿不到 NavPanel"));
                    return;
                }
                NavigationPushButton *btn = host->navPanel()->button(QStringLiteral("tasks"));
                if (btn == nullptr) {
                    uiTrace(QStringLiteral("task | 切任务页**失败**:侧栏没有「任务」按钮"));
                    return;
                }
                btn->click();
                // **必须回读验证**:上一次这里用 invokeMethod("switchToRoute") 而那个方法不是
                // Q_INVOKABLE,调用静默失败、追踪行却照样写"已切到任务页" —— 假证据。
                // 现在按当前路由回读,切没切上如实报。
                const QString now = host->currentRouteKey();
                uiTrace(QStringLiteral("task | 点侧栏「任务」-> 当前路由=%1 %2")
                            .arg(now, now == QLatin1String("tasks") ? QStringLiteral("已切到任务页")
                                                                    : QStringLiteral("没切上")));
            });
        }
    }

    // 照核心库的阶段表重画阶段行。
    // 依据:install.h:20-21 —— "计划里实际会跑的阶段用 sxcl_install_plan_stage_count/at 取,
    // UI 直接照它画行,不要自己另写一份表"。构造期那份 Python 行表只用于"计划还没回来"的
    // 一瞬间(也就是参考图抓到的那个状态)。
    void applyPlan(const QStringList &ids, const QStringList &names) {
        Q_UNUSED(ids)
        for (const StageRow &row : m_stageWidgets) {
            delete row.label;      // Qt 会在控件析构时把对应的布局项一起摘掉
            delete row.indicator;
        }
        m_stageWidgets.clear();
        while (QLayoutItem *item = m_stageListLayout->takeAt(0))
            delete item;
        for (int i = 0; i < names.size(); ++i)
            appendStageRow(names.at(i));
        // 末尾再补一行"安装完成":它是**终态标记**,不是核心库的阶段(核心最后一个阶段是
        // "整理文件"),所以不进 plan 的行数,也不参与 stageIndex 的映射。
        appendStageRow(QStringLiteral("安装完成"));
        m_planStages = names.size();
    }

    void onStageChanged(int index, int total, const QString &id, const QString &name) {
        Q_UNUSED(id)
        if (index > 0 && index - 1 < m_stageWidgets.size()) {          // :329-332
            StageRow &prev = m_stageWidgets[index - 1];
            prev.indicator->setState(StageIndicator::Done);
            prev.label->setTextColor(FluentTheme::instance().tokens().success,
                                     FluentTheme::instance().tokens().success);
        }
        if (index >= 0 && index < m_stageWidgets.size()) {              // :334-340
            StageRow &cur = m_stageWidgets[index];
            cur.indicator->setState(StageIndicator::Spinning);
            cur.label->setTextColor(FluentTheme::instance().tokens().accent,
                                    FluentTheme::instance().tokens().accent);
            cur.label->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 500;"));
            m_stageLabel->setText(name);
        }
        m_planStageTotal = total;
    }

    void onProgress(const InstallProgress &p) {
        m_barState = QStringLiteral("running");
        m_progressBar->setValue(p.percent);                             // :343-345
        m_percentLabel->setText(QStringLiteral("%1%").arg(p.percent));
        m_statusBadge->setText(QStringLiteral("进行中"));
        styleProgress(m_barState);

        // 顶部大字:核心库给的中文阶段名(**永远非空**) + 在本计划里的位置
        m_stageLabel->setText(QStringLiteral("%1(%2/%3)")
                                  .arg(p.stageName)
                                  .arg(p.stageIndex + 1)
                                  .arg(p.stageTotal));

        // 详情行:阶段 · 速度 · 已下载/总量 · 文件 · 当前文件
        // (Python 的格式见 download_progress_page.py:517-521:
        //  "{speed:.1f} MB/s | 剩余 {eta:.0f}s | {current}" —— 这里多了字节与文件数,
        //  因为核心库的进度里本来就带着它们,没有理由丢掉)
        QStringList parts;
        parts << p.stageName;
        if (p.speedBps > 0.0)
            parts << QStringLiteral("%1/s").arg(humanBytes(qint64(p.speedBps)));
        if (p.etaSeconds >= 0)
            parts << QStringLiteral("剩余 %1s").arg(p.etaSeconds);
        if (p.bytesTotal > 0)
            parts << QStringLiteral("%1/%2").arg(humanBytes(p.bytesDone), humanBytes(p.bytesTotal));
        if (p.filesTotal > 0)
            parts << QStringLiteral("文件 %1/%2").arg(p.filesDone).arg(p.filesTotal);
        if (p.filesSkipped > 0)
            parts << QStringLiteral("跳过 %1").arg(p.filesSkipped);
        if (p.filesFailed > 0)
            parts << QStringLiteral("失败 %1").arg(p.filesFailed);
        if (!p.current.isEmpty())
            parts << p.current;
        m_detailLabel->setText(parts.join(QStringLiteral(" | ")).left(200)); // :360 截 200

        updateTaskCard(p.percent, QStringLiteral("%1%").arg(p.percent),
                       QStringLiteral("%1 | %2").arg(p.stageName, p.status));
    }

    void onFinished(bool ok, bool cancelled, int code, bool retryable, const QString &message,
                    const QString &detail) { // :363-418
        m_finished = true;
        m_cancelButton->setEnabled(false);
        m_cancelButton->setText(QStringLiteral("取消"));
        m_backButton->setEnabled(true);

        if (ok) {
            m_barState = QStringLiteral("done");
            styleProgress(m_barState);
            for (const StageRow &row : m_stageWidgets) {                 // :381-384
                row.indicator->setState(StageIndicator::Done);
                row.label->setTextColor(FluentTheme::instance().tokens().success,
                                        FluentTheme::instance().tokens().success);
            }
            m_progressBar->setValue(100);
            m_percentLabel->setText(QStringLiteral("100%"));
            m_statusBadge->setText(QStringLiteral("完成"));
            m_stageLabel->setText(QStringLiteral("安装完成"));
            m_detailLabel->setText(detail.isEmpty() ? message : detail);
            callTaskState("setTaskDone");
            InfoBar::push(InfoBar::Type::Success, QStringLiteral("安装成功"), message, this, 4000);
            // 「结束后关闭」(电脑端)的**唯一**判据来源:三条终态里都通知窗口一次,
            // 关不关、什么时候关由 MainWindow 决定(页面不自己退出进程)。
            QMetaObject::invokeMethod(window(), "notifyInstallFinished", Qt::DirectConnection,
                                      Q_ARG(bool, true), Q_ARG(bool, false));
            return;
        }

        m_barState = QStringLiteral("failed");
        styleProgress(m_barState);
        if (cancelled) {
            // **取消是独立终态**:界面必须说"已取消",不能显示成成功,也不该说成"失败"。
            m_statusBadge->setText(QStringLiteral("已取消"));
            m_statusBadge->setStyleSheet(
                QStringLiteral("color: %1; font-weight: 500;")
                    .arg(FluentTheme::instance().tokenText(QStringLiteral("warning"))));
            m_stageLabel->setText(QStringLiteral("已取消"));
            m_detailLabel->setText(QStringLiteral("%1 | %2").arg(message, detail));
            callTaskState("setTaskFailed");
            updateTaskCard(m_progressBar->value(), QStringLiteral("已取消"), detail);
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("已取消"), message, this, 4000);
            // 「结束后关闭」(电脑端)的**唯一**判据来源:三条终态里都通知窗口一次,
            // 关不关、什么时候关由 MainWindow 决定(页面不自己退出进程)。
            QMetaObject::invokeMethod(window(), "notifyInstallFinished", Qt::DirectConnection,
                                      Q_ARG(bool, false), Q_ARG(bool, true));
            return;
        }

        // 失败:把**核心库的真实原因**显示出来(不是"失败"两个字),并标出失败阶段
        for (const StageRow &row : m_stageWidgets) {                     // :401-405
            if (row.indicator->state() == StageIndicator::Spinning) {
                row.indicator->setState(StageIndicator::Failed);
                row.label->setTextColor(FluentTheme::instance().tokens().danger,
                                        FluentTheme::instance().tokens().danger);
                break;
            }
        }
        m_statusBadge->setText(QStringLiteral("失败"));
        m_statusBadge->setStyleSheet(QStringLiteral("color: %1; font-weight: 500;")
                                         .arg(FluentTheme::instance().tokenText(
                                             QStringLiteral("danger"))));
        m_stageLabel->setText(QStringLiteral("安装失败"));
        m_detailLabel->setText(
            QStringLiteral("%1 | %2%3")
                .arg(message, detail,
                     retryable ? QStringLiteral(" | 可以重试") : QString()));
        callTaskState("setTaskFailed");
        updateTaskCard(m_progressBar->value(), QStringLiteral("失败"), message);
            // 「结束后关闭」(电脑端)的**唯一**判据来源:三条终态里都通知窗口一次,
            // 关不关、什么时候关由 MainWindow 决定(页面不自己退出进程)。
            QMetaObject::invokeMethod(window(), "notifyInstallFinished", Qt::DirectConnection,
                                      Q_ARG(bool, false), Q_ARG(bool, false));

        // 统一错误出口:完整上下文(页面/操作/核心库原始原因/错误码/阶段/构建)进剪贴板。
        // 这里**不再**自己拼 InfoBar —— 一处实现,别处不再各写一遍(用户明确要求)。
        UiErrorContext ctx;
        ctx.page = QStringLiteral("下载进度页 / download_progress_%1").arg(m_versionName);
        ctx.action = QStringLiteral("安装 Minecraft %1(实例 %2,加载器 %3)")
                         .arg(m_versionId, m_versionName,
                              m_loaderType.isEmpty() ? QStringLiteral("none") : m_loaderType);
        ctx.reason = message; // **核心库的人话原因原文**,不改写
        ctx.detail = QStringLiteral("错误码 %1(%2)· %3")
                         .arg(code)
                         .arg(QString::fromUtf8(sxcl_install_code_name(code)), detail);
        ctx.title = QStringLiteral("安装失败");
        pushUiError(this, ctx, 10000);
    }

    void onCancel() { // :420-424
        if (m_worker != nullptr && !m_finished) {
            m_worker->cancel(); // 核心库在文件边界上停下,清理 .part,返回 ERR_CANCELLED
        }
        m_cancelButton->setEnabled(false);
        m_cancelButton->setText(QStringLiteral("正在取消..."));
        updateTaskCard(m_progressBar->value(), QStringLiteral("正在取消…"),
                       QStringLiteral("等当前文件下完就停"));
    }

    // 详情行里"人话字节"(核心库只给整数,这里只做显示换算)
    static QString humanBytes(qint64 bytes) {
        const double mb = double(bytes) / (1024.0 * 1024.0);
        if (mb >= 1.0)
            return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
        const double kb = double(bytes) / 1024.0;
        if (kb >= 1.0)
            return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
        return QStringLiteral("%1 B").arg(bytes);
    }

    void onBack() { // :426-429
        if (QMetaObject::invokeMethod(window(), "goBackToVersions", Qt::DirectConnection))
            return;
        QMetaObject::invokeMethod(window(), "switchToRoute", Qt::DirectConnection,
                                  Q_ARG(QString, QStringLiteral("versions")));
    }

    struct StageRow {
        BodyLabel *label = nullptr;
        StageIndicator *indicator = nullptr;
    };

    QString m_versionId;
    QString m_versionName;
    QString m_loaderType;
    QString m_loaderVersion;
    QString m_barState = QStringLiteral("running"); // :168
    QWidget *m_view = nullptr;
    QVBoxLayout *m_vBox = nullptr;
    StrongBodyLabel *m_stageLabel = nullptr;
    BodyLabel *m_statusBadge = nullptr;
    QProgressBar *m_progressBar = nullptr;
    BodyLabel *m_percentLabel = nullptr;
    BodyLabel *m_detailLabel = nullptr;
    QVBoxLayout *m_stageListLayout = nullptr;
    PushButton *m_cancelButton = nullptr;
    PrimaryPushButton *m_backButton = nullptr;
    QVector<StageRow> m_stageWidgets = {};

    // ── 阶段 7 新增:执行侧 ──
    CardWidget *m_card = nullptr;          // 阶段行重建时的父控件(几何与构造期一致)
    InstallWorker *m_worker = nullptr;     // 工作线程外壳(this 的子对象,析构时会 join)
    QString m_taskKey;                     // 任务页的键("download_progress_<版本名>")
    QString m_lastLogLine;                 // 最近一条核心库日志(失败时给详情行兜底)
    bool m_finished = false;               // 三种终态已定(按钮/取消都要看它)
    int m_planStages = 0;                  // 计划里的阶段数(核心库那份)
    int m_planStageTotal = 0;              // 同上(信号里带的 total,显示用)
};

} // namespace

QWidget *createDownloadProgressPage(const VersionRef &version, const QString &versionName,
                                    const QString &loaderType, const QString &loaderVersion,
                                    QWidget *parent) {
    return new DownloadProgressPage(version, versionName, loaderType, loaderVersion, parent);
}

} // namespace sxcl::ui
