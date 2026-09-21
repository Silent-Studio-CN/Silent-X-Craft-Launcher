/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "page_factory.h"

#include <QAbstractButton>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <optional>

#if defined(_MSC_VER)
#pragma warning(push, 0) // libqf 是外部依赖,头文件在 /W4 下不干净(见 libqf.h 的说明)
#endif
#include "fluent/fluent_cards.h"
#include "fluent/fluent_controls.h"
#include "fluent/fluent_labels.h"
#include "fluent/fluent_input.h"
#include "fluent/fluent_scroll.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "fluent_theme.h"
#include "sxcl_icons.h"

#include "workers/ui_error.h"  // 统一错误出口(完整上下文 + 自动复制剪贴板)
#include "workers/ui_paths.h" // 游戏目录的唯一一份解析(与安装 worker 同口径)

namespace sxcl::ui {
namespace {

// Python 未给这些布局设间距,取的是布局默认值。参考图实测 = 6(home_page.cpp 同款取证)。
constexpr int kDefaultLayoutSpacing = 6;

// animations.py:45 NORMAL —— 展开/收起 190ms(进入 OutCubic / 退出 InCubic)
constexpr int kExpandDurationMs = 190;

// qf PushButton 构造里有一句 setFont(self)(button.py:36 -> 14px),libqf 的 PushButton::init()
// 只套 QSS 没设字号,这里按 docs/05-UI-1to1规格.md §4「PushButton 高 32,字体 14」钉回去。
void applyButtonFont(QPushButton *button) {
    QFont font = button->font();
    font.setPixelSize(14);
    font.setWeight(QFont::Normal);
    button->setFont(font);
    button->setFixedHeight(32);
}

// 游戏目录:走界面层共用的那一个解析(workers/ui_paths.h)。
//
// 为什么改成共用:本页要判断"版本名是否已存在",而安装 worker 要拿同一个目录去**真装**。
// 两边各算一遍必然出现"这里说不存在、那边装到别处"这种产品级缺陷。
// ui_paths 的口径与主页/CLI 一致:SXCL_UI_GAME_DIR > sxcl_settings 的 game.default_dir
// (含一次 Python 旧配置迁移)> 核心库平台默认。
QString gameDirectory() { return uiGameDirectory(); }

// ───────────────────────── 可点标题行(section_card.py:46-54 / loader_row.py:132-140)──────

class ClickableHeader : public QWidget {
public:
    explicit ClickableHeader(QWidget *parent = nullptr) : QWidget(parent) {}
    std::function<void()> onClick;

protected:
    // 两张卡片都是:左键 + 落点在自身矩形内才算"点了这一行"
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
            if (onClick)
                onClick();
        }
        QWidget::mouseReleaseEvent(event);
    }
};

// ───────────────────────── SectionCard(section_card.py:57-165)──────────────────────────

class SectionCard : public QWidget {
public:
    SectionCard(const QString &title, const QString &icon, QWidget *parent)
        : QWidget(parent), m_iconText(icon) {
        setObjectName(QStringLiteral("SectionCard"));   // :71
        setAttribute(Qt::WA_StyledBackground, true);    // :72

        auto *outer = new QVBoxLayout(this);            // :76-78 contentsMargins(4,4,4,8) spacing 0
        outer->setContentsMargins(4, 4, 4, 8);
        outer->setSpacing(0);

        m_header = new ClickableHeader(this);           // :80-83 setFixedHeight(46) + PointingHand
        m_header->setFixedHeight(46);
        m_header->setCursor(Qt::PointingHandCursor);
        m_header->onClick = [this] { toggle(); };
        auto *head = new QHBoxLayout(m_header);         // :84-85 margins(14,0,14,0) spacing 10
        head->setContentsMargins(14, 0, 14, 0);
        head->setSpacing(10);

        // :88-95 —— icon 不在 ICON_KINDS 里就退化成 StrongBodyLabel(📝/🔧 走的就是这条)
        head->addWidget(new StrongBodyLabel(m_iconText, m_header));
        head->addWidget(new StrongBodyLabel(title, m_header)); // :97-98
        head->addStretch(1);                                   // :99

        m_summaryLabel = new BodyLabel(QString(), m_header);   // :101-103
        head->addWidget(m_summaryLabel);

        m_arrowLabel = new BodyLabel(QStringLiteral("▾"), m_header); // :105-107
        head->addWidget(m_arrowLabel);
        outer->addWidget(m_header);                            // :108

        m_body = new QWidget(this);                            // :110-114 margins(14,0,14,8) sp6
        m_bodyLayout = new QVBoxLayout(m_body);
        m_bodyLayout->setContentsMargins(14, 0, 14, 8);
        m_bodyLayout->setSpacing(6);
        outer->addWidget(m_body);

        applyStyle();                                          // :116 + on_theme_changed(:117)
    }

    void addContent(QWidget *widget) { m_bodyLayout->addWidget(widget); }  // :121-122
    void addLayout(QLayout *layout) { m_bodyLayout->addLayout(layout); }   // :124-125
    void setSummary(const QString &text) { m_summaryLabel->setText(text); } // :127-128

    bool isExpanded() const { return m_expanded; }             // :132-133
    void toggle() { setExpanded(!m_expanded); }                // :135-136

    // :138-158。animate=false 用于构造期:Python 构造时也走 190ms 动画,而抓图前的稳定
    // 等待远大于 190ms,最终几何与"直接展开"完全相同(参考图就是这么来的)。
    void setExpanded(bool expanded, bool animate = true) {
        m_expanded = expanded;
        m_arrowLabel->setText(m_expanded ? QStringLiteral("▴") : QStringLiteral("▾")); // :156
        if (m_expanded) {
            m_body->setVisible(true);
            if (animate) {
                animateHeight(m_body->maximumHeight(), m_body->sizeHint().height(),
                              QEasingCurve::OutCubic);
            } else {
                // 构造期**必须**对齐动画的终态:animations.py:67-73 expand_height 的终点是
                // "动画开始那一刻的 sizeHint().height()",而且这个 maximumHeight 会一直留着
                // (Python 不恢复)。下载配置页就靠这一点:卡片在 name_input **还没套样式表**
                // 时展开(脚本 _build_content:179 在 _style_name_input:144 之前),body 被钉在
                // 31px;随后样式表的 8px 内边距让输入框 sizeHint 变 37px,却被 31 的
                // maximumHeight 压回 23px —— 参考图里输入框就是 957x23。
                // 这里直接写 sizeHint 而不是放开上限,否则整张卡会高出 14px。
                m_body->setMaximumHeight(m_body->sizeHint().height());
            }
        } else if (animate) {
            animateHeight(m_body->height(), 0, QEasingCurve::InCubic);
        } else {
            m_body->setVisible(false);
            m_body->setMaximumHeight(16777215);
        }
        if (m_expanded && onExpanded)                          // :157-158
            onExpanded(this);
    }

    std::function<void(SectionCard *)> onExpanded;

private:
    // animations.py:48-64 animate(widget, b"maximumHeight", ..., OutCubic/InCubic)
    void animateHeight(int from, int to, QEasingCurve::Type curve) {
        if (m_anim)
            m_anim->stop();
        m_anim = new QPropertyAnimation(m_body, "maximumHeight", this);
        m_anim->setDuration(kExpandDurationMs);
        m_anim->setStartValue(from);
        m_anim->setEndValue(to);
        m_anim->setEasingCurve(curve);
        if (to == 0) {
            // animations.py:79-86 collapse_height 的 _done:收起后隐藏并恢复 maximumHeight
            connect(m_anim, &QPropertyAnimation::finished, this, [this] {
                m_body->setVisible(false);
                m_body->setMaximumHeight(16777215);
            });
        }
        m_anim->start(QPropertyAnimation::KeepWhenStopped);
    }

    void applyStyle() { // :160-165 styles.section_card_qss()
        setStyleSheet(FluentTheme::instance().sectionCardQss());
        const QColor tertiary = FluentTheme::instance().tokens().textTertiary;
        m_summaryLabel->setTextColor(tertiary);
        m_arrowLabel->setTextColor(tertiary);
    }

    QString m_iconText;
    ClickableHeader *m_header = nullptr;
    QWidget *m_body = nullptr;
    QVBoxLayout *m_bodyLayout = nullptr;
    BodyLabel *m_summaryLabel = nullptr;
    BodyLabel *m_arrowLabel = nullptr;
    QPropertyAnimation *m_anim = nullptr;
    bool m_expanded = true; // section_card.py:73 self._expanded = True
};

// ─────────────────── 加载器版本行(loader_row.py,行号见注释)───────────────────

// loader_row.py:300-371 的标签/次要信息:UI 层只认"版本号 + 次要信息 + 三个标签",
// 数据由核心服务给(见报告);这里用结构体代替 Python 的 dict。
struct LoaderVersionItem {
    QString version;
    QString detail;
    bool recommended = false;
    bool beta = false;
};

// loader_row.py:51-129 LoaderVersionDelegate:一行 = 主信息(加粗)+ 次要信息(右对齐)
// + 右起的小胶囊标签;选中用 accent 描边 + 半透明底,悬停用 hover 令牌色。
class LoaderVersionDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static constexpr int kRowHeight = 40; // loader_row.py:60 ROW_HEIGHT

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return QSize(0, kRowHeight);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect rect = option.rect.adjusted(6, 3, -6, -3); // :70
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const bool selected = index.data(Qt::UserRole + 2).toBool();

        if (selected) {                                        // :74-79
            QColor fill = tokens.accent;
            fill.setAlpha(38);
            painter->setPen(QPen(tokens.accent, 1));
            painter->setBrush(QBrush(fill));
        } else if (hovered) {                                  // :80-82
            painter->setPen(Qt::NoPen);
            painter->setBrush(FluentTheme::parseCssColor(tokens.hover));
        } else {                                               // :83-85
            painter->setPen(Qt::NoPen);
            painter->setBrush(Qt::NoBrush);
        }
        painter->drawRoundedRect(rect, 6, 6);                  // :85

        const QString label = index.data(Qt::DisplayRole).toString(); // :87
        const QString detail = index.data(Qt::UserRole + 3).toString(); // :88
        const QVariantList tags = index.data(Qt::UserRole + 4).toList(); // :89

        QFont font = painter->font();                          // :93-96
        font.setBold(true);
        font.setPointSizeF(font.pointSizeF() + 0.5);
        painter->setFont(font);
        painter->setPen(QPen(tokens.text));
        painter->drawText(QRect(rect.left() + 10, rect.top(), rect.width() - 120, rect.height()),
                          Qt::AlignVCenter | Qt::AlignLeft, label);   // :98-99

        if (!detail.isEmpty()) {                               // :101-107
            font.setBold(false);
            font.setPointSizeF(qMax(7.5, font.pointSizeF() - 2.0));
            painter->setFont(font);
            painter->setPen(QPen(tokens.textTertiary));
            painter->drawText(
                QRect(rect.left() + 10, rect.top(), rect.width() - 120, rect.height()),
                Qt::AlignVCenter | Qt::AlignRight, detail);
        }

        int x = rect.right() - 12;                             // :109-123 标签从右往左排
        const QFontMetrics metrics = painter->fontMetrics();
        for (int i = tags.size() - 1; i >= 0; --i) {
            const QVariantList chip = tags.at(i).toList();
            if (chip.size() < 2)
                continue;
            const QString text = chip.at(0).toString();
            const QColor color = FluentTheme::parseCssColor(
                FluentTheme::instance().tokenText(chip.at(1).toString()));
            const int width = metrics.horizontalAdvance(text) + 14;
            const QRect box(x - width, rect.center().y() - 9, width, 18);
            QColor fill = color;
            fill.setAlpha(46);
            painter->setPen(QPen(color, 1));
            painter->setBrush(QBrush(fill));
            painter->drawRoundedRect(box, 5, 5);
            painter->setPen(QPen(color));
            painter->drawText(box, Qt::AlignCenter, text);
            x -= width + 6;
        }
        painter->restore();
    }
};

// loader_row.py:143-425
class LoaderRow : public QWidget {
public:
    static constexpr int kListHeight = 190;   // :150
    static constexpr int kHeaderHeight = 42;  // :151

    LoaderRow(const QString &loaderType, const QString &displayName, QWidget *parent)
        : QWidget(parent), m_loaderType(loaderType), m_displayName(displayName) {
        auto *outer = new QVBoxLayout(this);   // :164-166
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        m_header = new ClickableHeader(this);  // :169-175
        m_header->setFixedHeight(kHeaderHeight);
        m_header->setCursor(Qt::PointingHandCursor);
        m_header->onClick = [this] { toggle(); };
        auto *head = new QHBoxLayout(m_header); // :173-174
        head->setContentsMargins(10, 0, 10, 0);
        head->setSpacing(10);

        m_logo = new QLabel(m_header);          // :177-181
        m_logo->setFixedSize(22, 22);
        m_logo->setPixmap(SxclIcons::instance().blockPixmap(loaderType, 22));
        m_logo->setScaledContents(true);
        head->addWidget(m_logo);

        head->addWidget(new StrongBodyLabel(displayName, m_header)); // :183-184
        head->addStretch(1);                                          // :186

        m_summary = new BodyLabel(QStringLiteral("加载中…"), m_header); // :188-190
        head->addWidget(m_summary);

        m_arrow = new BodyLabel(QStringLiteral("▾"), m_header);       // :192-194
        head->addWidget(m_arrow);
        outer->addWidget(m_header);                                   // :196

        m_body = new QWidget(this);                                   // :199-202
        auto *bodyLayout = new QVBoxLayout(m_body);
        bodyLayout->setContentsMargins(12, 4, 12, 10);
        bodyLayout->setSpacing(8);

        m_search = new SearchLineEdit(m_body);                        // :204-207
        m_search->setPlaceholderText(QStringLiteral("筛选版本…"));
        bodyLayout->addWidget(m_search);

        m_list = new QListWidget(m_body);                             // :209-215
        m_list->setFixedHeight(kListHeight);
        m_list->setUniformItemSizes(true);
        m_list->setMouseTracking(true);
        m_list->setItemDelegate(new LoaderVersionDelegate(m_list));
        m_list->setStyleSheet(QStringLiteral(
            "QListWidget { background: transparent; border: none; }")); // theme.py:207-210
        bodyLayout->addWidget(m_list);

        auto *bottom = new QHBoxLayout();                             // :217-226
        bottom->setContentsMargins(0, 0, 0, 0);
        m_hint = new BodyLabel(QString(), m_body);
        bottom->addWidget(m_hint);
        bottom->addStretch(1);
        m_clearBtn = new PushButton(QStringLiteral("清除选择"), m_body);
        applyButtonFont(m_clearBtn);
        bottom->addWidget(m_clearBtn);
        bodyLayout->addLayout(bottom);

        m_body->setVisible(false);                                    // :228
        outer->addWidget(m_body);

        applyColors();                                                // :231-232
        connect(&FluentTheme::instance(), &FluentTheme::changed, this,
                [this] { applyColors(); });
    }

    QString loaderType() const { return m_loaderType; }               // :155
    bool isExpanded() const { return m_expanded; }                    // :142-143
    bool isSelected() const { return m_selectedVersion.has_value(); } // :415-416
    QString selectedVersion() const {                             // :418-419
        return m_selectedVersion.value_or(QString());
    }

    // loader_row.py:395-400 _clear():同组别的行被选中时把它清掉(选中回调也会跟着发)
    void clearSelection() { clear(); }

    void setGroup(const QVector<LoaderRow *> &rows) {                 // :236-238
        m_group.clear();
        for (LoaderRow *row : rows) {
            if (row != this)
                m_group.append(row);
        }
    }

    void toggle() { setExpanded(!m_expanded); }                       // :240-241

    // :243-262 —— 展开/收起;同一组里只留一个展开(手风琴)
    void setExpanded(bool expanded) {
        if (expanded == m_expanded)
            return;
        m_expanded = expanded;
        if (m_expanded) {
            m_body->setMaximumHeight(0);
            m_body->setVisible(true);
            animateHeight(m_body->sizeHint().height());
        } else {
            animateHeight(0);
        }
        m_arrow->setText(m_expanded ? QStringLiteral("▴") : QStringLiteral("▾"));
        if (m_expanded) {
            for (LoaderRow *row : m_group) {
                if (row->m_expanded)
                    row->setExpanded(false);
            }
            m_search->setFocus();                                     // :261
            if (onExpanded)
                onExpanded(this);                                     // :262
        }
    }

    void setLoading(bool loading) {                                   // :266-271
        m_loading = loading;
        if (loading) {
            m_summary->setText(QStringLiteral("加载中…"));
            m_list->clear();
            m_hint->setText(QString());
        }
    }

    void setError(const QString &message) {                           // :273-278
        m_loading = false;
        m_versions.clear();
        m_summary->setText(message);
        m_list->clear();
        m_hint->setText(message);
    }

    // :280-313 —— 把版本列表塞进来(默认选中第一个,和以前的下拉行为一致)
    void setVersions(const QVector<LoaderVersionItem> &versions) {
        m_loading = false;
        m_versions = versions;
        m_list->clear();
        if (m_versions.isEmpty()) {
            m_summary->setText(QStringLiteral("无可用版本"));
            m_hint->setText(QStringLiteral("这个游戏版本没有对应的加载器版本"));
            m_clearBtn->setVisible(false);
            return;
        }
        QVector<QPair<QString, QString>> labels;
        for (int index = 0; index < m_versions.size(); ++index) {
            const LoaderVersionItem &item = m_versions.at(index);
            if (item.version.isEmpty())
                continue;
            QStringList tags;
            if (index == 0)
                tags.append(QStringLiteral("最新"));                   // :299-300
            if (item.recommended)
                tags.append(QStringLiteral("推荐"));                   // :301-302
            if (item.version.contains(QStringLiteral("beta"), Qt::CaseInsensitive) ||
                item.beta)
                tags.append(QStringLiteral("Beta"));                   // :303-304
            const QString suffix = tags.isEmpty()
                                       ? QString()
                                       : QStringLiteral("    （%1）")
                                             .arg(tags.join(QStringLiteral(" / "))); // :305
            labels.append({item.version, item.version + suffix});
        }
        m_hint->setText(QStringLiteral("共 %1 个版本，点一行选中").arg(labels.size())); // :309
        fillList(labels);
        if (!labels.isEmpty())                                        // :312-313
            select(labels.first().first);
    }

    // 页面接线(替代 Python 的 loader_selected / loader_cleared / expanded 信号)
    std::function<void(const QString &, const QString &)> onLoaderSelected; // :146
    std::function<void(const QString &)> onLoaderCleared;                  // :147
    std::function<void(LoaderRow *)> onExpanded;                           // :148

private:
    // animations.py:67-73 expand_height / :76-86 collapse_height
    void animateHeight(int target) {
        if (m_anim)
            m_anim->stop();
        m_anim = new QPropertyAnimation(m_body, "maximumHeight", this);
        m_anim->setDuration(kExpandDurationMs);
        m_anim->setStartValue(m_body->maximumHeight() < 16777215 ? m_body->maximumHeight() : 0);
        m_anim->setEndValue(target);
        m_anim->setEasingCurve(target == 0 ? QEasingCurve::InCubic : QEasingCurve::OutCubic);
        if (target == 0) {
            connect(m_anim, &QPropertyAnimation::finished, this, [this] {
                m_body->setVisible(false);
                m_body->setMaximumHeight(16777215);
            });
        }
        m_anim->start(QPropertyAnimation::KeepWhenStopped);
    }

    void fillList(const QVector<QPair<QString, QString>> &labels) {  // :330-339
        m_list->clear();
        for (const auto &pair : labels) {
            auto *item = new QListWidgetItem(pair.second);
            item->setData(Qt::UserRole, pair.first);
            item->setData(Qt::UserRole + 2, pair.first == m_selectedVersion.value_or(QString()));
            item->setData(Qt::UserRole + 3, detailOf(pair.first));
            item->setData(Qt::UserRole + 4, tagsOf(pair.first));
            m_list->addItem(item);
        }
    }

    QString detailOf(const QString &version) const {                 // :341-354
        for (const LoaderVersionItem &item : m_versions) {
            if (item.version != version)
                continue;
            return item.detail;
        }
        return QString();
    }

    QVariantList tagsOf(const QString &version) const {              // :356-371
        QVariantList tags;
        const bool newest = !m_versions.isEmpty() && m_versions.first().version == version;
        for (const LoaderVersionItem &item : m_versions) {
            if (item.version != version)
                continue;
            if (newest)
                tags.append(QVariantList{QStringLiteral("最新"), QStringLiteral("accent")});
            if (item.recommended)
                tags.append(QVariantList{QStringLiteral("推荐"), QStringLiteral("success")});
            if (version.contains(QStringLiteral("beta"), Qt::CaseInsensitive) || item.beta)
                tags.append(QVariantList{QStringLiteral("Beta"), QStringLiteral("warning")});
        }
        return tags;
    }

    void select(const QString &version) {                            // :387-393
        m_selectedVersion = version;
        refreshSelection();
        m_summary->setText(QStringLiteral("已选 %1").arg(version));
        refreshSummaryColor();
        m_clearBtn->setVisible(true);
        if (onLoaderSelected)
            onLoaderSelected(m_loaderType, version);
    }

    void clear() {                                                   // :395-400
        m_selectedVersion.reset();
        m_summary->setText(QStringLiteral("未选择"));
        refreshSummaryColor();
        m_clearBtn->setVisible(false);
        if (onLoaderCleared)
            onLoaderCleared(m_loaderType);
    }

    void refreshSelection() {                                        // :402-406
        for (int row = 0; row < m_list->count(); ++row) {
            QListWidgetItem *item = m_list->item(row);
            item->setData(Qt::UserRole + 2,
                          item->data(Qt::UserRole).toString() ==
                              m_selectedVersion.value_or(QString()));
        }
    }

    void applyColors() {                                             // :408-411
        refreshSummaryColor();
        m_arrow->setTextColor(FluentTheme::instance().tokens().textTertiary);
        m_hint->setTextColor(FluentTheme::instance().tokens().textTertiary);
    }

    void refreshSummaryColor() {                                     // :408-411
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        const QColor color = m_selectedVersion.has_value() ? tokens.success : tokens.textTertiary;
        m_summary->setTextColor(color);
    }

    QString m_loaderType;
    QString m_displayName;
    ClickableHeader *m_header = nullptr;
    QLabel *m_logo = nullptr;
    BodyLabel *m_summary = nullptr;
    BodyLabel *m_arrow = nullptr;
    BodyLabel *m_hint = nullptr;
    QWidget *m_body = nullptr;
    SearchLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    PushButton *m_clearBtn = nullptr;
    QPropertyAnimation *m_anim = nullptr;
    QVector<LoaderRow *> m_group;
    QVector<LoaderVersionItem> m_versions;
    std::optional<QString> m_selectedVersion;
    bool m_loading = true;  // :162 self.is_loading = True
    bool m_expanded = false; // :163 self._expanded = False
};

// ───────────────────────── 页面本体(download_config_page.py:121-550)─────────────────────

class DownloadConfigPage : public ScrollArea {
public:
    DownloadConfigPage(const VersionRef &version, QWidget *parent)
        : ScrollArea(parent), m_versionId(version.id) {
        // ---- BasePage(src/app/common/base_page.py:41-60)----
        setObjectName(QStringLiteral("DownloadConfigPage"));  // :42
        setWidgetResizable(true);                             // :43
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // :44
        // 页面底色 = 令牌 bg(#202020)。依据 docs/05-UI-1to1规格.md §10.2(内容区设计值 #202020)
        // 与 §11.2(抓参考图时把页面底色钉成 token(bg))。外壳的内容栈是透明的,这一行是"保险"。
        setStyleSheet(QStringLiteral("QScrollArea { background: %1; }")
                          .arg(FluentTheme::instance().tokens().bg.name()));
        // Python 的 BasePage 继承 qf ScrollArea(默认 StyledPanel 的 1px 边框),
        // libqf 的 ScrollArea::initArea() 设成 NoFrame,这里按 Python 恢复 1px。
        setFrameShape(QFrame::StyledPanel);
        setLineWidth(1);

        m_view = new QWidget(this);                            // :46
        m_view->setStyleSheet(QStringLiteral("background: transparent;")); // :47
        setWidget(m_view);                                     // :48

        m_vBox = new QVBoxLayout(m_view);                      // :50
        m_vBox->setContentsMargins(28, 24, 28, 24);            // :51
        m_vBox->setSpacing(16);                                // :52
        m_vBox->setAlignment(Qt::AlignTop);                    // :53

        m_vBox->addWidget(new TitleLabel(QStringLiteral("安装 %1").arg(m_versionId), m_view));
        // :125 subtitle="" 且**没有** hide()(与另外两个临时页不同)
        m_vBox->addWidget(new SubtitleLabel(QString(), m_view));

        buildContent();                                        // :143
        styleNameInput(QStringLiteral("normal"));              // :144

        connect(&FluentTheme::instance(), &FluentTheme::changed, this, [this] {
            // :149-151 主题切换时把"按状态重算"的样式再套一遍
            styleNameInput(m_warning->isVisible() ? QStringLiteral("error")
                                                  : QStringLiteral("normal"));
        });
    }

private:
    void buildContent() { // :153-246
        // ── 返回按钮(:155-164)──
        auto *back = new QPushButton(QStringLiteral("←  返回版本列表"), m_view);
        back->setCursor(Qt::PointingHandCursor);
        back->setStyleSheet(QStringLiteral(
                                "QPushButton { border: none; color: %1; font-size: 13px;\n"
                                "              padding: 8px 0; text-align: left; }\n"
                                "QPushButton:hover { color: %1; }")
                                .arg(FluentTheme::instance().tokenText(QStringLiteral("accent"))));
        connect(back, &QAbstractButton::clicked, this, [this] { goBack(); });
        m_vBox->insertWidget(0, back);                          // :164

        // ── Section 1: 版本名称(:166-180)──
        m_nameSection = new SectionCard(QStringLiteral("版本名称"), QStringLiteral("📝"), m_view);
        m_nameInput = new QLineEdit(m_view);                    // :168
        m_nameInput->setPlaceholderText(QStringLiteral("输入自定义版本名称…")); // :169
        m_nameInput->setText(m_versionId);                      // :170
        connect(m_nameInput, &QLineEdit::textChanged, this,
                [this](const QString &text) { onNameManualEdit(text); }); // :171

        m_warning = new BodyLabel(QStringLiteral("⚠ 不能与现有版本名相同"), m_view); // :173
        m_warning->setTextColor(FluentTheme::instance().tokens().danger);            // :175
        m_warning->setVisible(false);                                                // :176
        m_nameSection->addContent(m_nameInput);                                      // :177
        m_nameSection->addContent(m_warning);                                        // :178
        m_nameSection->setExpanded(true, false);                                     // :179
        m_vBox->addWidget(m_nameSection);                                            // :180

        // ── Section 2: 模组加载器(:182-227)──
        m_loaderSection = new SectionCard(QStringLiteral("模组加载器"), QStringLiteral("🔧"), m_view);

        const struct {
            const char *type;
            const char *name;
        } loaders[] = {{"forge", "Forge"}, {"neoforge", "NeoForge"},
                       {"fabric", "Fabric"}, {"optifine", "OptiFine"}};
        for (const auto &spec : loaders) {
            auto *row = new LoaderRow(QString::fromLatin1(spec.type),
                                      QString::fromLatin1(spec.name), m_view);
            row->onLoaderSelected = [this](const QString &type, const QString &ver) {
                onLoaderSelected(type, ver);
            };
            row->onLoaderCleared = [this](const QString &type) { onLoaderCleared(type); };
            row->onExpanded = [this](LoaderRow *r) { onLoaderExpanded(r); };
            m_loaderRows.append(row);
            m_loaderSection->addContent(row);
        }

        // OptiFine 状态行(:210-219):HBox margins(8,4,8,4),标签定宽 64
        auto *ofLayout = new QHBoxLayout();
        ofLayout->setContentsMargins(8, 4, 8, 4);
        auto *ofLabel = new BodyLabel(QStringLiteral("OptiFine"), m_view);
        ofLabel->setFixedWidth(64);
        ofLayout->addWidget(ofLabel);
        m_optifineStatus = new BodyLabel(QStringLiteral("检测中…"), m_view);
        ofLayout->addWidget(m_optifineStatus);
        ofLayout->addStretch();
        m_loaderSection->addLayout(ofLayout);

        m_loaderSection->setExpanded(false, false);             // :221
        for (LoaderRow *row : m_loaderRows)
            row->setGroup(m_loaderRows);                        // :223-225

        m_vBox->addWidget(m_loaderSection);                     // :227

        // ── Section 3: 下载按钮(:229-244)──
        auto *downloadCard = new CardWidget(m_view);            // :230
        auto *dlLayout = new QHBoxLayout(downloadCard);         // :231
        dlLayout->setContentsMargins(0, 0, 0, 0);               // :232
        dlLayout->setSpacing(kDefaultLayoutSpacing);            // :233-238 未设 -> 默认 6
        // 兼容性提示区:不兼容时这里写清原因,按钮同时禁用(**初始为空且可见**:
        // Python 只在 _refresh_compat 里 setVisible,而它要等加载器列表回来才跑)
        m_compatBox = new QWidget(downloadCard);                // :234
        auto *compatLayout = new QVBoxLayout(m_compatBox);      // :235
        compatLayout->setContentsMargins(0, 0, 0, 6);           // :236
        compatLayout->setSpacing(4);                            // :237
        dlLayout->addWidget(m_compatBox);                       // :238

        m_downloadBtn = new PrimaryPushButton(QStringLiteral("开始下载"), downloadCard); // :240
        applyButtonFont(m_downloadBtn);
        m_downloadBtn->setFixedHeight(44);                      // :241
        connect(m_downloadBtn, &QAbstractButton::clicked, this, [this] { onDownload(); }); // :242
        dlLayout->addWidget(m_downloadBtn);                     // :243
        m_vBox->addWidget(downloadCard);                        // :244

        m_vBox->addStretch(1);                                  // :246
    }

    // :248-252 展开一行就把别的收起来(手风琴)
    void onLoaderExpanded(LoaderRow *row) {
        for (LoaderRow *other : m_loaderRows) {
            if (other != row && other->isExpanded())
                other->setExpanded(false);
        }
    }

    // :304-310 选中一个加载器后,别的行里已选的清掉
    void onLoaderSelected(const QString &type, const QString &version) {
        for (LoaderRow *row : m_loaderRows) {
            if (row->loaderType() != type && row->isSelected())
                row->clearSelection(); // :306-307 row._clear()
        }
        m_selectedLoader = type;
        m_selectedLoaderVersion = version;
        updateVersionName();
    }

    void onLoaderCleared(const QString &) { // :312-316
        m_selectedLoader.clear();
        m_selectedLoaderVersion.clear();
        updateVersionName();
    }

    // :318-349 默认版本名按 PCL 的规则拼(GetSelectName)。
    // 核心库的 loaders.default_version_name 还没挂到 UI 层(见报告),这里保留
    // "无加载器时 = 原版 id" 这条路径 —— 也正是初始态会走到的分支。
    void updateVersionName() {
        const QString vn = m_versionId;
        if (!m_userEditedName) {
            const QSignalBlocker blocker(m_nameInput); // :344-346 blockSignals
            m_nameInput->setText(vn);
        }
        m_nameInput->setPlaceholderText(QStringLiteral("自动生成: %1").arg(vn)); // :347
        checkVersionExists(vn);                                                  // :348
    }

    // :351-359 版本名输入框的三种状态(普通 / 重名错误),颜色全走令牌
    void styleNameInput(const QString &state) {
        const QColor border = state == QLatin1String("error")
                                  ? FluentTheme::instance().tokens().danger
                                  : FluentTheme::instance().tokens().inputBorder;
        const QString width = state == QLatin1String("error") ? QStringLiteral("2px")
                                                              : QStringLiteral("1px");
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        m_nameInput->setStyleSheet(
            QStringLiteral("QLineEdit { border: %1 solid %2; border-radius: 6px;"
                           " padding: 8px 12px; font-size: 13px; background: %3;"
                           " color: %4; }"
                           "QLineEdit:focus { border-color: %5; }")
                .arg(width, border.name(), tokens.inputBg.name(), tokens.text.name(),
                     FluentTheme::instance().tokenText(QStringLiteral("accent"))));
    }

    // :420-433 目录里有 JSON 就算已存在(Forge 1.13+/Fabric 装的版本没有自己的 jar)
    void checkVersionExists(const QString &versionName) {
        const QString dir = gameDirectory() + QStringLiteral("/versions/") + versionName;
        bool hasJson = false;
        const QDir d(dir);
        if (d.exists()) {
            const QStringList names = d.entryList(QStringList() << QStringLiteral("*.json"),
                                                  QDir::Files);
            hasJson = !names.isEmpty();
        }
        m_nameTaken = hasJson ||
                      QFileInfo::exists(dir + QLatin1Char('/') + versionName +
                                        QStringLiteral(".jar"));
        if (m_nameTaken) {
            styleNameInput(QStringLiteral("error"));
            m_warning->setVisible(true);
        } else {
            styleNameInput(QStringLiteral("normal"));
            m_warning->setVisible(false);
        }
    }

    void onNameManualEdit(const QString &text) { // :435-439
        m_userEditedName = !text.isEmpty() &&
                           text != m_nameInput->placeholderText() && text != m_versionId;
        checkVersionExists(text.trimmed());
    }

    void goBack() { // :441-446
        // Python: mw = self.window(); hasattr(mw, 'go_back_to_versions') -> 调它,
        // 否则回退到 parent().switchTo(parent().versions_page)
        if (QMetaObject::invokeMethod(window(), "goBackToVersions", Qt::DirectConnection))
            return;
        if (QMetaObject::invokeMethod(window(), "switchToRoute", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("versions"))))
            return;
    }

    // :499-549 开始下载。核心库的安装引擎还没接到 UI(见报告),这里是"算好参数后交给
    // 主窗口的下载进度页"这一段 —— 与 Python 的 switch_to_download_progress 调用一致。
    void onDownload() {
        const QString vn = m_nameInput->text().trimmed();
        if (vn.isEmpty()) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("请输入版本名称"),
                          QStringLiteral("版本名称不能为空"), this, 3000);
            return;
        }
        const QString dir = gameDirectory() + QStringLiteral("/versions/") + vn;
        const QDir d(dir);
        const bool hasJson =
            d.exists() && !d.entryList(QStringList() << QStringLiteral("*.json"), QDir::Files).isEmpty();
        if (hasJson ||
            QFileInfo::exists(dir + QLatin1Char('/') + vn + QStringLiteral(".jar"))) {
            // 统一错误出口(带 warning 级别):一样复制完整上下文到剪贴板
            UiErrorContext ctx;
            ctx.page = QStringLiteral("下载配置页 / download_config_%1").arg(m_versionId);
            ctx.action = QStringLiteral("开始下载(实例名 %1)").arg(vn);
            ctx.reason = QStringLiteral("版本 '%1' 已经安装，请使用不同的版本名称").arg(vn);
            ctx.detail = QStringLiteral("目标目录已有版本 JSON 或 jar: %1")
                             .arg(QDir::toNativeSeparators(dir));
            ctx.title = QStringLiteral("版本已存在");
            ctx.warning = true;
            pushUiError(this, ctx, 6000);
            styleNameInput(QStringLiteral("error"));
            return;
        }

        QString loaderType = QStringLiteral("none");
        QString loaderVersion;
        if (m_selectedLoader == QLatin1String("forge") ||
            m_selectedLoader == QLatin1String("neoforge") ||
            m_selectedLoader == QLatin1String("fabric") ||
            m_selectedLoader == QLatin1String("optifine")) {
            for (LoaderRow *row : m_loaderRows) {
                if (row->loaderType() != m_selectedLoader || !row->isSelected())
                    continue;
                loaderVersion = row->selectedVersion();
                if (!loaderVersion.isEmpty())
                    loaderType = m_selectedLoader;
            }
        }

        if (QMetaObject::invokeMethod(window(), "switchToDownloadProgress", Qt::DirectConnection,
                                      Q_ARG(QString, m_versionId), Q_ARG(QString, vn),
                                      Q_ARG(QString, loaderType),
                                      Q_ARG(QString, loaderVersion)))
            return;
        UiErrorContext ctx;
        ctx.page = QStringLiteral("下载配置页 / download_config_%1").arg(m_versionId);
        ctx.action = QStringLiteral("开始下载(实例名 %1,加载器 %2)").arg(vn, loaderType);
        ctx.reason = QStringLiteral("主窗口未提供下载进度页接口(switchToDownloadProgress 调用失败)");
        ctx.detail = QStringLiteral("游戏目录: %1").arg(QDir::toNativeSeparators(dir));
        ctx.title = QStringLiteral("无法开始下载");
        pushUiError(this, ctx, 8000);
    }

    QString m_versionId;
    QWidget *m_view = nullptr;
    QVBoxLayout *m_vBox = nullptr;
    SectionCard *m_nameSection = nullptr;
    SectionCard *m_loaderSection = nullptr;
    QLineEdit *m_nameInput = nullptr;
    BodyLabel *m_warning = nullptr;
    QVector<LoaderRow *> m_loaderRows;
    BodyLabel *m_optifineStatus = nullptr;
    QWidget *m_compatBox = nullptr;
    PrimaryPushButton *m_downloadBtn = nullptr;
    QString m_selectedLoader;
    QString m_selectedLoaderVersion;
    bool m_nameTaken = false;      // :138
    bool m_userEditedName = false; // :139
};

} // namespace

QWidget *createDownloadConfigPage(const VersionRef &version, QWidget *parent) {
    return new DownloadConfigPage(version, parent);
}

} // namespace sxcl::ui
