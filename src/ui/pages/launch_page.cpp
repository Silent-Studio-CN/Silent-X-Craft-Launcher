// launch_page.cpp —— 启动进度页(临时页)
//
// 逐条移植 Python 版 src/app/pages/launch_page.py(行号见每段注释),外壳照抄
// src/app/common/base_page.py。外观的唯一依据:docs/05-UI-1to1规格.md + Python 源码。
//
// 控件树(实测 build/ref/TREE_py_launch.txt;页面可用区 1051x701):
//   LaunchProgressPage(ScrollArea, objectName=LaunchProgressPage, 1px StyledPanel 边框)
//     └ view(QWidget, transparent)
//        └ QVBoxLayout(margins 28,24,28,24; spacing 16; AlignTop)
//           ├ TitleLabel     "启动 <id>"  993x38 (28px/600)
//           ├ SubtitleLabel  "" -> hide()
//           ├ CardWidget     993x282  VBox(28,20,28,20) sp12
//           │   ├ HBox: StrongBodyLabel "准备启动" + 弹性 + BodyLabel "● 准备中"(accent/500)
//           │   ├ QProgressBar 937x4(h[4..4];range 0..100;textVisible=false;圆角 2)
//           │   ├ 5 行阶段: LaunchIndicator 12x12 + BodyLabel(行内 sp8)+ 弹性
//           │   └ BodyLabel 日志行(空;定高 40;color #9a9a9a)
//           ├ HBox: PushButton "取消" 54x32 + 弹性 + PrimaryPushButton "返回" 54x32(禁用)
//           └ 弹簧
//
// 数据来源说明(见交付报告):启动流程(核心库 sxcl 的 launch API)还没接到 UI 层,
// 页面停在 Python 构造后、LaunchWorker 第一个 phase_changed 之前的初始态
// —— 这也正是参考图的状态(见 tools/grab_reference_ui.py 的临时页分支)。
#include "page_factory.h"

#include <QAbstractButton>
#include <QFont>
#include <QHBoxLayout>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

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
        // :511 self._start_launch():启动流程属于核心库(见文件头说明),UI 层保持初始态。
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

        m_vBox->addWidget(card);                             // :585 add_content(card)

        // ── 按钮(:587-598)──
        auto *btnLayout = new QHBoxLayout();
        m_cancelButton = new PushButton(QStringLiteral("取消"), m_view);   // :589
        applyButtonFont(m_cancelButton);
        connect(m_cancelButton, &QAbstractButton::clicked, this, [this] { onCancel(); });
        btnLayout->addWidget(m_cancelButton);
        btnLayout->addStretch(1);
        m_backButton = new PrimaryPushButton(QStringLiteral("返回"), m_view); // :593
        applyButtonFont(m_backButton);
        m_backButton->setEnabled(false);                                      // :594
        connect(m_backButton, &QAbstractButton::clicked, this, [this] { onBack(); });
        btnLayout->addWidget(m_backButton);
        m_vBox->addLayout(btnLayout);                        // :597
        m_vBox->addStretch(1);                               // :598
    }

    void onCancel() { // :667-671
        // Python: self._worker.cancel() + 按钮禁用改字;没有 worker 时只做按钮那半边。
        m_cancelButton->setEnabled(false);
        m_cancelButton->setText(QStringLiteral("正在取消…"));
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
    PrimaryPushButton *m_backButton = nullptr;
    QVector<PhaseRow> m_phaseWidgets = {};
};

} // namespace

QWidget *createLaunchPage(const VersionRef &version, QWidget *parent) {
    return new LaunchProgressPage(version, parent);
}

} // namespace sxcl::ui
