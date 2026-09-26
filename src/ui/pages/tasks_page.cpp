/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "page_factory.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
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

// QSS 用的令牌字符串。实测(6.11):Qt 的 QSS 解析器**吃不下带空格的 rgba**
// ("rgba(255, 255, 255, 0.05)" 会被整条声明丢掉 → 卡片底色不画),而令牌字符串
// (theme.py) 里就是带空格的 —— 所以进 QSS 前先把 rgba(...) 里的空格去掉,
// 颜色值不变,渲染结果与 Python 的设计意图(半透明白)一致。
QString tokenQss(const QString &name) {
    QString s = tokenCss(name);
    if (s.startsWith(QLatin1String("rgba"), Qt::CaseInsensitive)) {
        const int l = s.indexOf(QLatin1Char('('));
        const int r = s.lastIndexOf(QLatin1Char(')'));
        if (l > 0 && r > l)
            s = s.left(l + 1) + s.mid(l + 1, r - l - 1).remove(QLatin1Char(' ')) + s.mid(r);
    }
    return s;
}

} // namespace

// ──────────────────────────────────────────────────────────────── _StatusIcon

// tasks_page.py:40-96 —— 任务状态图标:进行中 = 转圈,完成 = 对勾,失败 = 叉。
// **三个状态全部自绘**(用户 2026-09-22 与 09-26 两次点名:「emoji 去掉,改成 svg」)——
// 以前这里用 "⠋⠙⠹…" 盲文转轮和 "✓/✕" 两个字符:它们落到哪个字体由系统决定,
// 大小 / 颜色 / 基线都不受控(在别的机器上就是 emoji 的观感)。颜色仍取主题令牌。
class TaskStatusIcon : public QWidget {
    Q_OBJECT
public:
    enum State { Running = 0, Done = 1, Failed = 2 };

    explicit TaskStatusIcon(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(24, 24);
        m_timer = new QTimer(this);
        m_timer->setInterval(120);
        connect(m_timer, &QTimer::timeout, this, &TaskStatusIcon::tick);
        connect(&FluentTheme::instance(), &FluentTheme::changed, this, [this] { update(); });
        m_timer->start();
    }

    void setState(int state) {
        m_state = state;
        if (state == Running) {
            m_timer->start();
            setCursor(Qt::ArrowCursor);
        } else {
            m_timer->stop();
            setCursor(state == Failed ? Qt::PointingHandCursor : Qt::ArrowCursor);
        }
        update(); // 状态变了立刻重画,不等下一帧
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(3.5, 3.5, -3.5, -3.5);
        if (m_state == Running) { // 转圈:每 tick 转 30°,画 3/4 圈
            QPen pen(FluentTheme::instance().accent(), 2.0);
            pen.setCapStyle(Qt::RoundCap);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawArc(box, m_spin * 30 * 16, 270 * 16);
            return;
        }
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        QPen pen(m_state == Failed ? tokens.danger : tokens.success, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        QPainterPath path; // 完成 = 对勾;失败 = 叉
        if (m_state == Failed) {
            path.moveTo(box.left(), box.top());
            path.lineTo(box.right(), box.bottom());
            path.moveTo(box.right(), box.top());
            path.lineTo(box.left(), box.bottom());
        } else {
            path.moveTo(box.left(), box.center().y() + 1.0);
            path.lineTo(box.center().x() - 1.0, box.bottom());
            path.lineTo(box.right(), box.top());
        }
        painter.drawPath(path);
    }

private:
    void tick() { // tasks_page.py:94-96(帧推进;这里推的是自绘弧的角度)
        m_spin = (m_spin + 1) % 12;
        update();
    }

    QTimer *m_timer = nullptr;
    int m_state = Running;
    int m_spin = 0;
};

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
        // Python 用类名选择器 TaskCard { … };Qt 的 QSS 对**命名空间里的类**要求写成
        // sxcl--ui--TaskCard(冒号换成 --),既啰嗦又容易踩坑 —— 这里改成等价的
        // objectName 选择器 #taskCard,渲染结果与 Python 一致。
        setObjectName(QStringLiteral("taskCard"));

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
                          "#taskCard { background: %1; border-radius: 8px;"
                          " margin: 2px 0; }\n"
                          "#taskCard:hover { background: %2; }")
                          .arg(tokenQss(QStringLiteral("hoverBg")),
                               tokenQss(QStringLiteral("hoverBgStrong"))));
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

        // 页面底色钉令牌 bg(#202020):抓参考图时 Python 也是这么钉的(grab_reference_ui.py:141
        //   page.setStyleSheet("QWidget { background: %s }" % token("bg")))——参考图的内容区
        // 就是 #202020,不是内容栈那层半透明白。栈自己的 rgba(255,255,255,0.0314) 只该在
        // 窗口左上圆角那半像素露出来(见 main_window.cpp 的圆角取证),页面不钉底色整片会变 #272727。
        setStyleSheet(QStringLiteral("QScrollArea { background: %1; }")
                          .arg(FluentTheme::instance().tokens().bg.name()));

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
