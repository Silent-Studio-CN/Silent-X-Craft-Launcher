// download_progress_page.cpp —— 下载/安装进度页(临时页)
//
// 逐条移植 Python 版 src/app/pages/download_progress_page.py(行号见每段注释),外壳照抄
// src/app/common/base_page.py。外观的唯一依据:docs/05-UI-1to1规格.md + Python 源码。
//
// 控件树(实测 build/ref/TREE_py_download_progress.txt;页面可用区 1051x701):
//   DownloadProgressPage(ScrollArea, objectName=DownloadProgressPage, 1px StyledPanel 边框)
//     └ view(QWidget, transparent)
//        └ QVBoxLayout(margins 28,24,28,24; spacing 16; AlignTop)
//           ├ TitleLabel     "正在安装 <版本名>"  993x38 (28px/600)
//           ├ SubtitleLabel  ""  -> hide()              (与下载配置页不同,这里 Python 明确 hide)
//           ├ CardWidget     993x316  VBox(32,24,32,24) sp12
//           │   ├ HBox: StrongBodyLabel "准备安装"(15px) + 弹性 + BodyLabel "● 进行中"
//           │   ├ QProgressBar 929x6(h[6..6];range 0..100;textVisible=false;圆角 3)
//           │   ├ BodyLabel "0%" 右对齐(text_tertiary)
//           │   ├ BodyLabel 详情行(空;12px)
//           │   └ 阶段列表 VBox sp6 margins(0,8,0,0):6 行
//           │       StageIndicator 20x20 + BodyLabel 13px + 弹性(行内 sp10)
//           ├ HBox sp12: PushButton "取消" 54x32 + 弹性 + PrimaryPushButton "返回" 54x32(禁用)
//           └ 弹簧
//
// 数据来源说明(见交付报告):安装引擎(核心库 sxcl 的下载/安装 API)还没接到 UI 层,
// 页面停在 Python 构造后、InstallWorker 第一个 stage_changed 之前的初始态
// —— 这也正是参考图的状态(见 tools/grab_reference_ui.py 的临时页分支)。
#include "page_factory.h"

#include <QAbstractButton>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
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

        // :173 self._start_installation():安装引擎属于核心库(见文件头说明),
        // UI 层没有 worker 可起,页面保持初始态。
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

        m_statusBadge = new BodyLabel(QStringLiteral("● 进行中"), card);      // :212
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

        for (const QString &name : stages) {                     // :266-280
            auto *row = new QHBoxLayout();
            row->setSpacing(10);                                 // :268
            auto *indicator = new StageIndicator(card);
            row->addWidget(indicator);
            auto *label = new BodyLabel(name, card);
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

    void onCancel() { // :420-424
        // Python: self._worker.cancel() + 按钮禁用改字;没有 worker 时只做按钮那半边。
        m_cancelButton->setEnabled(false);
        m_cancelButton->setText(QStringLiteral("正在取消..."));
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
};

} // namespace

QWidget *createDownloadProgressPage(const VersionRef &version, const QString &versionName,
                                    const QString &loaderType, const QString &loaderVersion,
                                    QWidget *parent) {
    return new DownloadProgressPage(version, versionName, loaderType, loaderVersion, parent);
}

} // namespace sxcl::ui
