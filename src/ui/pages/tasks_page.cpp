// tasks_page.cpp —— 任务页(Python 版 src/app/pages/tasks_page.py 的 1:1 C++ 移植)
//
// 逐条对照(尺寸/文字/颜色都能指回下面某一条,见 docs/05-UI-1to1规格.md):
//   * 版面骨架 = Python src/app/common/base_page.py:32-66(BasePage:ScrollArea + view 的
//     QVBoxLayout margins(28,24,28,24)/spacing 16/AlignTop + TitleLabel + SubtitleLabel);
//     这里**内联**写了一遍而不是抽公共头 —— 与主代理约定过"不新增共享头文件",见最终报告;
//   * 空态标签 / 任务容器 / 弹簧 = tasks_page.py:218-231;
//   * TaskCard 三行结构 = tasks_page.py:118-207(固定高 80、内边距 16/12/16/12、
//     ProgressBar 固定高 4、明细行 12px);
//   * _StatusIcon = tasks_page.py:40-96(24×24,盲文转轮 10 帧 120ms/帧,✓/✕);
//   * _DeleteBtn  = tasks_page.py:99-115(20×20,叉号,danger,默认隐藏);
//   * 对外 API 与自动移除 = tasks_page.py:235-288。
//
// 与 Python 逐字相同的硬编码色:#888(空态标签/状态文本)、#999(明细行)、#606060/#AAAAAA
// (副标题)。其余一律走 FluentTheme 令牌(hover_bg / hover_bg_strong / accent / success / danger)。
//
// 两处等价替换(C 版机制差异,外观不变):
//   1) Python 用类名选择器 TaskCard { … };C 版的 TaskCard 带 Q_OBJECT,moc 的 className
//      就是 "TaskCard",选择器与 Python 完全一致;
//   2) Python 布局间距用 Qt 样式默认值(实测 6),这里显式写成 6,免得换样式后漂移。
#include "page_factory.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#if defined(_MSC_VER)
#pragma warning(push, 0) // libqf 的头在 /W4 下不是零警告,整体静音(见 libqf.h 的说明)
#endif
#include "fluent/fluent_controls.h" // PushButton / InfoBar
#include "fluent/fluent_labels.h"   // BodyLabel / StrongBodyLabel / SubtitleLabel / TitleLabel
#include "fluent/fluent_progress.h" // ProgressBar
#include "fluent/fluent_scroll.h"   // ScrollArea
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "fluent_theme.h"

namespace sxcl::ui {

namespace {

// 令牌小工具
QString tokenCss(const QString &name) { return FluentTheme::instance().tokenText(name); }

} // namespace

// ──────────────────────────────────────────────────────────────── _StatusIcon

// tasks_page.py:40-96 —— 任务状态图标:进行中 = 转轮,完成 = ✓,失败 = ✕
class TaskStatusIcon : public QWidget {
    Q_OBJECT
public:
    enum State { Running = 0, Done = 1, Failed = 2 };

    explicit TaskStatusIcon(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(24, 24);
        m_label = new BodyLabel(QStringLiteral("⠋"), this);
        m_label->setAlignment(Qt::AlignCenter);
        m_label->setStyleSheet(QStringLiteral("font-size: 16px;"));
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_label);
        m_timer = new QTimer(this);
        m_timer->setInterval(120);
        connect(m_timer, &QTimer::timeout, this, &TaskStatusIcon::tick);
        connect(&FluentTheme::instance(), &FluentTheme::changed, this,
                [this] { refreshTheme(); });
        refreshTheme();
    }

    void setState(int state) {
        m_state = state;
        if (state == Running) {
            m_timer->start();
            setCursor(Qt::ArrowCursor);
        } else if (state == Done) {
            m_timer->stop();
            m_label->setText(QStringLiteral("✓"));
            setCursor(Qt::ArrowCursor);
        } else if (state == Failed) {
            m_timer->stop();
            m_label->setText(QStringLiteral("✕"));
            setCursor(Qt::PointingHandCursor);
        }
        refreshTheme();
    }

private:
    void tick() { // tasks_page.py:94-96
        m_spin = (m_spin + 1) % kSpinChars.size();
        m_label->setText(kSpinChars.at(m_spin));
    }

    void refreshTheme() { // tasks_page.py:66-77
        QString color;
        QString weight;
        QString text;
        switch (m_state) {
        case Done:
            text = QStringLiteral("✓");
            color = tokenCss(QStringLiteral("success"));
            weight = QStringLiteral("bold");
            break;
        case Failed:
            text = QStringLiteral("✕");
            color = tokenCss(QStringLiteral("danger"));
            weight = QStringLiteral("bold");
            break;
        default:
            text = QStringLiteral("⠋");
            color = tokenCss(QStringLiteral("accent"));
            weight = QStringLiteral("normal");
            break;
        }
        // 进行中时不覆盖转轮当前帧(与 Python 的 "if state != RUNNING and text not in (✓,✕)" 同义)
        if (m_state != Running && m_label->text() != QStringLiteral("✓") &&
            m_label->text() != QStringLiteral("✕"))
            m_label->setText(text);
        else if (m_state == Running && m_label->text().isEmpty())
            m_label->setText(text);
        m_label->setStyleSheet(QStringLiteral("color: %1; font-size: 16px; font-weight: %2;")
                                   .arg(color, weight));
    }

    static const QStringList kSpinChars; // tasks_page.py:55
    BodyLabel *m_label = nullptr;
    QTimer *m_timer = nullptr;
    int m_state = Running;
    int m_spin = 0;
};

const QStringList TaskStatusIcon::kSpinChars = {
    QStringLiteral("⠋"), QStringLiteral("⠙"), QStringLiteral("⠹"), QStringLiteral("⠸"),
    QStringLiteral("⠼"), QStringLiteral("⠴"), QStringLiteral("⠦"), QStringLiteral("⠧"),
    QStringLiteral("⠇"), QStringLiteral("⠏")};

// ──────────────────────────────────────────────────────────────── _DeleteBtn

// tasks_page.py:99-115 —— 鼠标悬停时出现的红色叉号删除按钮
class TaskDeleteButton : public QPushButton {
    Q_OBJECT
public:
    explicit TaskDeleteButton(QWidget *parent = nullptr)
        : QPushButton(QStringLiteral("×"), parent) {
        setFixedSize(20, 20);
        setVisible(false);
        connect(&FluentTheme::instance(), &FluentTheme::changed, this,
                [this] { refreshTheme(); });
        refreshTheme();
    }

private:
    void refreshTheme() {
        const QString danger = tokenCss(QStringLiteral("danger"));
        setStyleSheet(QStringLiteral(
                          "QPushButton { border: none; color: %1; font-size: 18px;"
                          " font-weight: bold; background: transparent; }\n"
                          "QPushButton:hover { color: %1; }")
                          .arg(danger));
    }
};

// ──────────────────────────────────────────────────────────────── TaskCard

// tasks_page.py:118-207 —— 一张任务卡(下载/启动进度)
class TaskCard : public QWidget {
    Q_OBJECT
public:
    TaskCard(const QString &taskId, const QString &title, QWidget *parent = nullptr)
        : QWidget(parent), m_taskId(taskId) {
        setFixedHeight(80);
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 12, 16, 12);
        layout->setSpacing(6); // Python 用样式默认值(实测 6)

        // Row 1: 图标 + 标题 + 状态 + 删除
        auto *row1 = new QHBoxLayout();
        row1->setSpacing(6);
        m_icon = new TaskStatusIcon(this);
        row1->addWidget(m_icon);
        row1->addSpacing(8);
        m_title = new StrongBodyLabel(title, this);
        row1->addWidget(m_title);
        row1->addStretch();
        m_status = new BodyLabel(QStringLiteral("准备中"), this);
        m_status->setStyleSheet(QStringLiteral("color: #888;"));
        row1->addWidget(m_status);
        row1->addSpacing(4);
        m_delete = new TaskDeleteButton(this);
        connect(m_delete, &QPushButton::clicked, this,
                [this] { emit deleteRequested(m_taskId); });
        row1->addWidget(m_delete);
        layout->addLayout(row1);

        // Row 2: 进度条
        m_progress = new ProgressBar(this);
        m_progress->setFixedHeight(4);
        m_progress->setRange(0, 100);
        m_progress->setValue(0);
        layout->addWidget(m_progress);

        // Row 3: 明细
        m_detail = new BodyLabel(QString(), this);
        m_detail->setStyleSheet(QStringLiteral("color: #999; font-size: 12px;"));
        layout->addWidget(m_detail);

        connect(&FluentTheme::instance(), &FluentTheme::changed, this,
                [this] { refreshTheme(); });
        refreshTheme();
    }

    QString taskId() const { return m_taskId; }

    void updateProgress(int value, const QString &status, const QString &detail = QString()) {
        m_progress->setValue(value);
        m_status->setText(status);
        if (!detail.isEmpty())
            m_detail->setText(detail);
    }

    void setRunning() { // tasks_page.py:182-185
        m_icon->setState(TaskStatusIcon::Running);
        m_hoverShowsDelete = false;
        m_delete->setVisible(false);
    }
    void setDone() { // tasks_page.py:187-191
        m_icon->setState(TaskStatusIcon::Done);
        m_hoverShowsDelete = false;
        m_progress->setValue(100);
        m_delete->setVisible(false);
    }
    void setFailed() { // tasks_page.py:193-195
        m_icon->setState(TaskStatusIcon::Failed);
        m_hoverShowsDelete = true;
    }

signals:
    void clicked(TaskCard *card);
    void deleteRequested(const QString &taskId);

protected:
    void mousePressEvent(QMouseEvent *event) override { // tasks_page.py:197-198
        emit clicked(this);
        QWidget::mousePressEvent(event);
    }
    void enterEvent(QEnterEvent *event) override { // tasks_page.py:200-203
        if (m_hoverShowsDelete)
            m_delete->setVisible(true);
        QWidget::enterEvent(event);
    }
    void leaveEvent(QEvent *event) override { // tasks_page.py:205-207
        m_delete->setVisible(false);
        QWidget::leaveEvent(event);
    }

private:
    void refreshTheme() { // tasks_page.py:169-174
        setStyleSheet(QStringLiteral(
                          "TaskCard { background: %1; border-radius: 8px; margin: 2px 0; }\n"
                          "TaskCard:hover { background: %2; }")
                          .arg(tokenCss(QStringLiteral("hover_bg")),
                               tokenCss(QStringLiteral("hover_bg_strong"))));
    }

    QString m_taskId;
    TaskStatusIcon *m_icon = nullptr;
    StrongBodyLabel *m_title = nullptr;
    BodyLabel *m_status = nullptr;
    TaskDeleteButton *m_delete = nullptr;
    ProgressBar *m_progress = nullptr;
    BodyLabel *m_detail = nullptr;
    bool m_hoverShowsDelete = false;
};

// ──────────────────────────────────────────────────────────────── TasksPage

// tasks_page.py:210-293
class TasksPage : public ScrollArea {
    Q_OBJECT
public:
    explicit TasksPage(QWidget *parent = nullptr) : ScrollArea(parent) {
        setObjectName(QStringLiteral("TasksPage"));
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        // BasePage 骨架(base_page.py:46-60)
        m_view = new QWidget(this);
        m_view->setStyleSheet(QStringLiteral("background: transparent;"));
        setWidget(m_view);
        m_box = new QVBoxLayout(m_view);
        m_box->setContentsMargins(28, 24, 28, 24);
        m_box->setSpacing(16);
        m_box->setAlignment(Qt::AlignTop);
        m_title = new TitleLabel(QStringLiteral("任务"), m_view);
        m_subtitle = new SubtitleLabel(QStringLiteral("查看和管理正在进行的操作"), m_view);
        m_subtitle->setTextColor(QColor(QStringLiteral("#606060")),
                                 QColor(QStringLiteral("#AAAAAA")));
        m_box->addWidget(m_title);
        m_box->addWidget(m_subtitle);

        buildContent();
    }

    // ---- 对外 API(tasks_page.py:235-283;主窗口登记任务时按 id 驱动)----

    Q_INVOKABLE TaskCard *addOrUpdateTask(const QString &taskId, const QString &title,
                                          int progress = 0,
                                          const QString &status = QStringLiteral("准备中"),
                                          const QString &detail = QString()) {
        if (TaskCard *existing = m_tasks.value(taskId, nullptr)) {
            existing->updateProgress(progress, status, detail);
            return existing;
        }
        auto *card = new TaskCard(taskId, title, m_container);
        card->updateProgress(progress, status, detail);
        connect(card, &TaskCard::clicked, this, [this](TaskCard *c) { onTaskClicked(c); });
        connect(card, &TaskCard::deleteRequested, this, [this](const QString &id) {
            removeTask(id);
        });
        m_tasks.insert(taskId, card);
        m_layout->addWidget(card);
        refreshVisibility();
        return card;
    }

    Q_INVOKABLE void removeTask(const QString &taskId) {
        TaskCard *card = m_tasks.value(taskId, nullptr);
        if (!card)
            return;
        m_tasks.remove(taskId);
        m_layout->removeWidget(card);
        card->deleteLater();
        refreshVisibility();
    }

    Q_INVOKABLE void updateTask(const QString &taskId, int progress = 0,
                                const QString &status = QString(),
                                const QString &detail = QString()) {
        if (TaskCard *card = m_tasks.value(taskId, nullptr))
            card->updateProgress(progress, status, detail);
    }

    Q_INVOKABLE void setTaskRunning(const QString &taskId) {
        if (TaskCard *card = m_tasks.value(taskId, nullptr))
            card->setRunning();
    }

    Q_INVOKABLE void setTaskDone(const QString &taskId) {
        if (TaskCard *card = m_tasks.value(taskId, nullptr))
            card->setDone();
        // 完成后 3 秒自动移除(tasks_page.py:277-278)
        QTimer::singleShot(3000, this, [this, taskId] { removeTask(taskId); });
    }

    Q_INVOKABLE void setTaskFailed(const QString &taskId) {
        if (TaskCard *card = m_tasks.value(taskId, nullptr))
            card->setFailed();
    }

    // 自检用(报告/测试):当前任务卡数量
    Q_INVOKABLE int taskCount() const { return int(m_tasks.size()); }

private:
    void buildContent() { // tasks_page.py:218-231
        m_emptyLabel = new BodyLabel(QStringLiteral("暂无进行中的任务"), m_view);
        m_emptyLabel->setAlignment(Qt::AlignCenter);
        m_emptyLabel->setStyleSheet(
            QStringLiteral("color: #888; font-size: 14px; margin: 60px 0;"));
        m_box->addWidget(m_emptyLabel);

        m_container = new QWidget(m_view);
        m_layout = new QVBoxLayout(m_container);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(6); // Python 用样式默认值(实测 6)
        m_layout->setAlignment(Qt::AlignTop);
        m_container->setVisible(false);
        m_box->addWidget(m_container);

        m_box->addStretch(1);
    }

    void refreshVisibility() { // tasks_page.py:285-288
        const bool has = !m_tasks.isEmpty();
        m_emptyLabel->setVisible(!has);
        m_container->setVisible(has);
    }

    void onTaskClicked(TaskCard *card) { // tasks_page.py:290-293
        if (!card)
            return;
        // Python: mw.navigate_to_task(card.task_id) —— 主窗口没这个方法就什么也不做
        QMetaObject::invokeMethod(window(), "navigateToTask", Qt::DirectConnection,
                                  Q_ARG(QString, card->taskId()));
    }

    QWidget *m_view = nullptr;
    QVBoxLayout *m_box = nullptr;
    TitleLabel *m_title = nullptr;
    SubtitleLabel *m_subtitle = nullptr;
    BodyLabel *m_emptyLabel = nullptr;
    QWidget *m_container = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QMap<QString, TaskCard *> m_tasks;
};

QWidget *createTasksPage(QWidget *parent) { return new TasksPage(parent); }

} // namespace sxcl::ui

#include "tasks_page.moc"
