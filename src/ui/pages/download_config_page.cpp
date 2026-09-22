/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "page_factory.h"

#include <QAbstractButton>
#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

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
#include "workers/ui_paths.h" // 游戏目录/下载源/追踪的唯一一份口径(与安装 worker 同口径)

// ── 核心库(纯 C)—— 加载器版本目录的取数/解析全走这里,界面层不自己认格式 ──
#include "sxcl/fs.h"           // sxcl_fs_remove(清理残骸)
#include "sxcl/install.h"       // sxcl_install_target_probe/describe(与安装引擎同一份"已存在"判定)
#include "sxcl/loader_catalog.h" // sxcl_catalog_fetch/url/format_of + sxcl_loader_kind
#include "sxcl/net.h"            // sxcl_transport_qt_create(Qt 传输后端,工作线程里建/释放)

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

// ───────────── 加载器版本目录的取数层(include/sxcl/loader_catalog.h)─────────────
//
// 这一页以前只有"显示层":LoaderRow 有 setLoading/setVersions/setError,但**没有任何**
// 取数代码,四个加载器行于是永远停在"加载中…"(用户报过两次)。取数/解析一律交给核心库的
// 目录服务,界面层只做三件事:按设置排候选源、在工作线程里取、把结果原样回填。
//
// 线程口径(与 versions_page.cpp 的 loadVersions 同款):工作线程里
// sxcl_transport_qt_create() -> sxcl_catalog_fetch() -> QMetaObject::invokeMethod 回主线程;
// **主线程绝不联网、绝不阻塞**。传输后端有线程亲和性,所以谁建的谁释放(见下面 tr->destroy)。

// 与下载引擎(engine.c 的 SXCL_DEFAULT_UA)同一份 UA:OptiFine 官网对没有 UA 的请求不客气。
const char *const kCatalogUserAgent =
    "User-Agent: SilentXCraftLauncher/1.0 (+https://github.com/Silent-Studio-CN)";

// 每行最多收多少条。实测:Forge 1.20.1 有 130+ 条、Fabric 对 1.20.1 有 250+ 条、
// NeoForge 的 21.1.x 有 240+ 条;512 条 × 412 字节 ≈ 211 KB,在工作线程的堆上分配。
constexpr size_t kCatalogMaxEntries = 512;

// 一个来源的取数结论 —— **缓存的最小单位**,键 = (加载器, MC 版本, 来源)。
struct LoaderCatalogSourceResult {
    bool ok = false;                     // 取到了并且解析出至少一条
    QString error;                       // 失败原因(人话:网络/状态码/一条都没解析出来)
    QVector<LoaderVersionItem> versions; // 成功时的候选(顺序与"最新/推荐/Beta"标记都由核心库给)
};

// 工作线程带回来的一次尝试(纯数据;怎么用由主线程决定)。
struct LoaderCatalogAttempt {
    sxcl_catalog_source source = SXCL_CATALOG_SRC_OFFICIAL;
    bool ok = false;
    QString error;
    QVector<LoaderVersionItem> versions;
};

QString catalogSourceName(sxcl_catalog_source source) {
    return source == SXCL_CATALOG_SRC_MIRROR ? QStringLiteral("mirror")
                                             : QStringLiteral("official");
}

QString catalogFormatName(sxcl_catalog_format format) {
    switch (format) {
    case SXCL_CATALOG_FMT_MAVEN_XML:
        return QStringLiteral("maven-xml");
    case SXCL_CATALOG_FMT_META_JSON:
        return QStringLiteral("meta-json");
    case SXCL_CATALOG_FMT_OPTIFINE_JSON:
        return QStringLiteral("optifine-json");
    case SXCL_CATALOG_FMT_OPTIFINE_HTML:
        return QStringLiteral("optifine-html");
    case SXCL_CATALOG_FMT_LIST_JSON:
        return QStringLiteral("list-json");
    case SXCL_CATALOG_FMT_NONE:
    default:
        return QStringLiteral("none");
    }
}

// 源按设置走(workers/ui_paths.h 的 uiDownloadSource(),默认 "bmclapi"):
//   bmclapi / auto -> 先 MIRROR,不通再 OFFICIAL
//   mojang         -> 先 OFFICIAL,不通再 MIRROR
// 两条都试,只是顺序不同 —— 换源由这个顺序决定,不需要开关。
QVector<int> catalogSourceOrder(const QString &setting) {
    QVector<int> order;
    if (setting.compare(QLatin1String("mojang"), Qt::CaseInsensitive) == 0) {
        order << SXCL_CATALOG_SRC_OFFICIAL << SXCL_CATALOG_SRC_MIRROR;
    } else {
        order << SXCL_CATALOG_SRC_MIRROR << SXCL_CATALOG_SRC_OFFICIAL;
    }
    return order;
}

// 核心库的条目 -> 界面行的数据结构。只搬运:**不排序、不重排标记**
// (顺序、最新/推荐/Beta 都是 sxcl_catalog_prepare 给的,界面里再排一遍就会两边不一致)。
QVector<LoaderVersionItem> catalogToItems(const std::vector<sxcl_catalog_entry> &entries,
                                          size_t count) {
    QVector<LoaderVersionItem> items;
    items.reserve(static_cast<int>(count));
    for (size_t i = 0; i < count; ++i) {
        const sxcl_catalog_entry &entry = entries[i];
        if (entry.version[0] == '\0')
            continue;
        LoaderVersionItem item;
        item.version = QString::fromUtf8(entry.version);
        QStringList detail;
        if (entry.released[0] != '\0')
            detail << QString::fromUtf8(entry.released);
        if (entry.forge[0] != '\0') // OptiFine:配套的 Forge 版本
            detail << QStringLiteral("Forge %1").arg(QString::fromUtf8(entry.forge));
        item.detail = detail.join(QStringLiteral(" · "));
        item.recommended = entry.is_recommended != 0;
        item.beta = entry.is_beta != 0;
        items.append(item);
    }
    return items;
}

// 一条来源的取数:**在工作线程里跑**,不碰任何界面对象。
// 用 sxcl_catalog_format_of() 先问清"这个源给的是哪种格式"(两家格式不一样:
// Forge/NeoForge 是 maven-metadata.xml、Fabric 是 meta JSON、OptiFine 官方是网页/镜像是 JSON),
// FMT_NONE = 这个源压根没有这个加载器的接口 —— 如实报,不去猜格式。
LoaderCatalogAttempt fetchCatalogSource(sxcl_transport *tr, sxcl_loader_kind kind,
                                        const QString &kindId, const QString &mc,
                                        sxcl_catalog_source source) {
    LoaderCatalogAttempt attempt;
    attempt.source = source;
    const QString sourceName = catalogSourceName(source);
    const sxcl_catalog_format format = sxcl_catalog_format_of(kind, source);
    const QString formatName = catalogFormatName(format);
    QElapsedTimer timer;
    timer.start();
    size_t count = 0;
    QString failure;
    std::vector<sxcl_catalog_entry> entries(kCatalogMaxEntries);
    if (format == SXCL_CATALOG_FMT_NONE) {
        failure = QStringLiteral("这个来源没有该加载器的版本接口");
    } else {
        const char *const headers[2] = {kCatalogUserAgent, nullptr};
        char err[SXCL_HTTP_ERROR_MAX] = {0};
        sxcl_catalog_doc_info info{};
        const QByteArray mcUtf8 = mc.toUtf8();
        const int rc = sxcl_catalog_fetch(tr, kind, source, mcUtf8.constData(), headers, &info,
                                          entries.data(), kCatalogMaxEntries, &count, err,
                                          sizeof(err));
        if (rc == SXCL_CATALOG_OK && count > 0) {
            attempt.ok = true;
            attempt.versions = catalogToItems(entries, count);
        } else {
            failure = QString::fromUtf8(err[0] != '\0' ? err : "列表是空的");
        }
    }
    attempt.error = failure;
    // 验收通路(Android 上看 logcat 也是这一行):count=0 与失败都要打,失败带真实原因。
    uiTrace(QStringLiteral("【loader | kind=%1 mc=%2 source=%3 count=%4 ms=%5 format=%6%7】")
                .arg(kindId, mc, sourceName, QString::number(static_cast<qulonglong>(count)),
                     QString::number(timer.elapsed()), formatName,
                     failure.isEmpty() ? QString() : QStringLiteral(" error=") + failure));
    return attempt;
}

// 工作线程入口:把"这一轮要试的源"跑完,结果整包带回去(成功与否由主线程按缓存决定)。
QVector<LoaderCatalogAttempt> fetchCatalogAttempts(const QString &kindId, const QString &mc,
                                                   const QVector<int> &sources) {
    QVector<LoaderCatalogAttempt> attempts;
    attempts.reserve(sources.size());
#if defined(SXCL_UI_HAVE_QT_TRANSPORT)
    const sxcl_loader_kind kind = sxcl_loader_kind_from_id(kindId.toUtf8().constData());
    sxcl_transport *tr = sxcl_transport_qt_create();
    if (tr != nullptr) {
        for (int value : sources) {
            LoaderCatalogAttempt attempt =
                fetchCatalogSource(tr, kind, kindId, mc, static_cast<sxcl_catalog_source>(value));
            const bool ok = attempt.ok;
            attempts.append(attempt);
            if (ok)
                break; // 第一路拿到就停:这就是"先试 A,失败再试 B",不白跑第二条路
        }
        tr->destroy(tr->ctx); // 传输后端有线程亲和性:谁建的谁在这个线程里释放
        return attempts;
    }
    // 后端都建不出来:**如实说**,绝不用空列表假装成功
    for (int value : sources) {
        LoaderCatalogAttempt attempt;
        attempt.source = static_cast<sxcl_catalog_source>(value);
        attempt.error = QStringLiteral("没有可用的网络后端(sxcl_net_qt 未链接)");
        uiTrace(QStringLiteral("【loader | kind=%1 mc=%2 source=%3 count=0 ms=0 error=%4】")
                    .arg(kindId, mc, catalogSourceName(attempt.source), attempt.error));
        attempts.append(attempt);
    }
#else
    for (int value : sources) {
        LoaderCatalogAttempt attempt;
        attempt.source = static_cast<sxcl_catalog_source>(value);
        attempt.error = QStringLiteral("本产物没有编译进 Qt 传输后端(sxcl_net_qt)");
        uiTrace(QStringLiteral("【loader | kind=%1 mc=%2 source=%3 count=0 ms=0 error=%4】")
                    .arg(kindId, mc, catalogSourceName(attempt.source), attempt.error));
        attempts.append(attempt);
    }
#endif
    return attempts;
}

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

    // 取证用的只读访问(页面把"界面真的拿到了"写进追踪时要读这两个值)。不改任何状态。
    QString summaryText() const { return m_summary->text(); }
    int listCount() const { return m_list->count(); }

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

// 一行加载器的取数状态(只在主线程读写)。
struct LoaderRowFetchState {
    bool busy = false;    // 这一行正在取数(工作线程还没回来)
    bool hasData = false; // 拿到过非 0 条
    bool failed = false;  // 上一次取数失败 —— 用户再展开这一行就是"重试"
    QString mc;           // 界面上这份数据属于哪个 MC 版本(证据行里要打出来)
};

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

        // 下载源只读一次(与其它页同口径:改设置要重开页面才生效)—— 取数层按它排候选顺序。
        m_sourceSetting = uiDownloadSource();
        buildContent();                                        // :143
        styleNameInput(QStringLiteral("normal"));              // :144
        // 取数:(b) 当前选中的 MC 版本 = m_versionId,页面就是按它建出来的;
        // 四行各自取一次(工作线程)。缓存键里带 mc,所以换了版本(新页面/新实例)时
        // 旧版本的数据绝不会被顶上来。已拿到的不会重复联网(见 requestLoaderVersions)。
        preloadLoaderCatalogs();

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

        m_warning = new BodyLabel(QString(), m_view); // :173（提示文案在 checkVersionExists 里给）
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
                       {"fabric", "Fabric"}, {"quilt", "Quilt"},
                       {"optifine", "OptiFine"}};
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
        // (a) 用户展开这一行 -> 取数:已经拿到的**不重复取**;上一次失败的按"再点一次"重试
        // (Python 的 expanded 信号 -> _load_versions 就是这条)。
        if (row != nullptr)
            requestLoaderVersions(row->loaderType(),
                                  m_catalogState.value(row->loaderType()).failed);
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
        // 默认名要不要带加载器?**要**(用户点名,见 docs/22 的 B8/E3)。
        // 不带的话"同一个原版装第二个加载器"必然撞名 —— 引擎在拼路径之前就按
        // TARGET_JSON 拒装(sxcl_install_target_probe),用户只会看到"版本已存在"。
        // 命名习惯与核心库的候选名对齐(installer.c 的 adopt_generated):
        //   Fabric -> fabric-loader-<loader 版本>-<原版>(Fabric 安装器自己的命名,也是 PCL 的习惯)
        //   其它   -> <原版>-<加载器>-<加载器版本>(Forge/NeoForge/OptiFine 的常见叫法)
        QString vn = m_versionId;
        if (!m_selectedLoader.isEmpty() && m_selectedLoader != QLatin1String("none") &&
            !m_selectedLoaderVersion.isEmpty()) {
            if (m_selectedLoader == QLatin1String("fabric")) {
                vn = QStringLiteral("fabric-loader-%1-%2").arg(m_selectedLoaderVersion, m_versionId);
            } else {
                vn = QStringLiteral("%1-%2-%3")
                         .arg(m_versionId, m_selectedLoader, m_selectedLoaderVersion);
            }
        }
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

    // :420-433 "这个名字是不是已经被占了"。
    // **判定只有一份**:核心库的 sxcl_install_target_probe(install.h)——
    // 安装引擎在拼路径之前用的是同一个函数,所以"这里说不存在、那边装到别处/覆盖掉"不可能再发生。
    // 口径:versions/<名>/<名>.json 存在**且能解析** = 真装过;同名 jar 残留 = 没装完的残骸。
    bool versionNameTaken(const QString &versionName, QString *why = nullptr,
                          bool *remnant = nullptr) const {
        char text[SXCL_INSTALL_ERROR_MAX];
        text[0] = '\0';
        const int flags = sxcl_install_target_describe(text, sizeof(text),
                                                       gameDirectory().toUtf8().constData(),
                                                       versionName.toUtf8().constData());
        if (why != nullptr)
            *why = QString::fromUtf8(text);
        // **口径统一(2026-09-22,docs/22 的 B2/C3)**:拦不拦只看"有没有可解析的版本 JSON"——
        // 这正是安装引擎(sxcl_install_run 的预检)用的那一位。以前这里写的是 flags != NONE,
        // 于是"同名 jar 没有 JSON"的**残骸也拦**:引擎明明愿意重装(它只拒 TARGET_JSON),
        // 界面却只甩一句"版本已存在,请换个名字",用户既不知道能清理,也拿不到那句清理建议。
        // 残骸现在走另一条路:提示 + 让用户选"清理残骸并继续"(见 onDownload)。
        if (remnant != nullptr)
            *remnant = (flags & SXCL_INSTALL_TARGET_JAR) != 0 && (flags & SXCL_INSTALL_TARGET_JSON) == 0;
        return (flags & SXCL_INSTALL_TARGET_JSON) != 0;
    }

    /** 这个名字被占了的话，往后找一个空名字（base / base-2 / base-3 …）。
     *
     *  **为什么是"换名字"而不是"拦住用户"**（用户 2026-09-22 晚的原话）：
     *    「用户下无数个同版本你也管不着？游戏下载用得着你告诉用户那个下载过了？」
     *  版本 = versions/ 下的**文件夹名**：同一个原版装几份都正常，界面既不该拦、
     *  也不该教育用户"你下过了"。占名字这件事只有一种真实后果 —— 不能往同一个文件夹里
     *  再装一份（那是覆盖，会毁掉原来那份），所以这里**自动换个空文件夹名**，
     *  用户照样一键装第二份、第三份，全程不需要为名字操心。 */
    QString nextFreeVersionName(const QString &base) const {
        QString candidate = base.trimmed();
        if (candidate.isEmpty())
            candidate = m_versionId;
        if (!versionNameTaken(candidate))
            return candidate;
        for (int i = 2; i <= 99; ++i) {
            const QString alt = QStringLiteral("%1-%2").arg(candidate).arg(i);
            if (!versionNameTaken(alt))
                return alt;
        }
        return candidate + QStringLiteral("-") + QString::number(QDateTime::currentMSecsSinceEpoch());
    }

    void checkVersionExists(const QString &versionName) {
        QString why;
        bool remnant = false;
        if (versionNameTaken(versionName, &why, &remnant)) {
            // 只提示"会换个空名字装"，不画红框、不说"不能" —— 用户要装多少份都行
            styleNameInput(QStringLiteral("normal"));
            m_warning->setText(QStringLiteral("「%1」已经有一份了，这次会装成「%2」")
                                   .arg(versionName, nextFreeVersionName(versionName)));
            m_warning->setVisible(true);
        } else if (remnant) {
            // 残骸:引擎愿意装,所以**不画红框**,但要把引擎那句准确的话摆出来
            // (它自己就写了"或先清理那个目录"),用户不用猜。
            styleNameInput(QStringLiteral("normal"));
            m_warning->setText(QStringLiteral("%1").arg(why));
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
        QString vn = m_nameInput->text().trimmed();
        if (vn.isEmpty()) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("请输入版本名称"),
                          QStringLiteral("版本名称不能为空"), this, 3000);
            return;
        }
        const QString dir = gameDirectory() + QStringLiteral("/versions/") + vn;
        // 与安装引擎同一份判定(核心库 sxcl_install_target_probe):这里说"已存在"就是引擎会拒装
        // 的那一种;这里说"没有",引擎也不会另判一套。
        /* 用户点名（2026-09-22 晚）：「用户下无数个同版本你也管不着？游戏下载用得着你告诉
         * 用户那个下载过了？」—— 这里**不拦、不问、不说教**：这个名字已经有主了就自动往后找
         * 一个空文件夹名（1.20.1 -> 1.20.1-2 -> …），既不覆盖已经装好的那一份，也不打断用户。
         *
         * "版本 = versions/ 下的文件夹名"（PCL 概念）：同一个原版装多少份都由用户说了算，
         * 界面唯一要保证的是**别往同一个文件夹里再装一遍**（那是覆盖，会毁掉原来那份）。
         * 以前那两段（"版本已存在，请换名字"的错误弹窗 + "发现没装完的残骸"的确认框）都删了。 */
        const QString requested = vn;
        vn = nextFreeVersionName(vn);
        if (vn != requested) {
            const QSignalBlocker blocker(m_nameInput);
            m_nameInput->setText(vn);
        }

        QString loaderType = QStringLiteral("none");
        QString loaderVersion;
        if (m_selectedLoader == QLatin1String("forge") ||
            m_selectedLoader == QLatin1String("neoforge") ||
            m_selectedLoader == QLatin1String("fabric") ||
            m_selectedLoader == QLatin1String("quilt") ||
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

    // ───────────────────── 加载器版本目录:界面侧的取数/回填 ─────────────────────
    //
    // 三件事分开清楚(以前是整条缺失,所以四行永远"加载中…"):
    //   1) 什么时候取:(a) 用户展开那一行;(b) 页面为当前选中的 MC 版本建出来(构造后统一预取)。
    //   2) 取什么/怎么取:工作线程 + 核心库目录服务(取数层在文件上半部分)。
    //   3) 什么时候不取:同一个 (加载器, MC, 来源) 已经有结论就**不重复联网**;
    //      失败过的只有"用户再点开这一行"才重试(不是无限重试)。
    // 缓存与状态都**只在主线程**读写(工作线程只带回结果),不共享、不加锁。

    LoaderRow *rowForKind(const QString &kindId) const {
        for (LoaderRow *row : m_loaderRows) {
            if (row->loaderType() == kindId)
                return row;
        }
        return nullptr;
    }

    // 缓存键:(加载器, MC 版本, 来源)。换 MC 版本 = 键变了 = 自动重取。
    QString catalogCacheKey(const QString &kindId, int source) const {
        return QStringLiteral("%1|%2|%3").arg(kindId, m_versionId, QString::number(source));
    }

    // 页面构造后:四行各取一次(缓存里已有结论的会直接回填,不联网)。
    void preloadLoaderCatalogs() {
        for (LoaderRow *row : m_loaderRows)
            requestLoaderVersions(row->loaderType(), false);
    }

    // 取数的唯一入口。retry 只由"用户又点开这一行"带进来。
    void requestLoaderVersions(const QString &kindId, bool retry) {
        LoaderRowFetchState &state = m_catalogState[kindId];
        if (state.busy)
            return; // 正在取:不重复取
        // 这一行已经有**当前 MC 版本**的数据:既不再联网,也不重建列表
        // (重建会把用户已经选中的版本重置成第一条)。
        if (state.hasData && state.mc == m_versionId && !retry)
            return;
        // 按设置里的顺序算"这一轮还要试哪几路":
        //   * 顺序里**第一条已经在手里的可用结论**就是答案,它后面的源一律不再试;
        //   * 没取过的要取(顺序在前面的先试,拿到就停,见 fetchCatalogAttempts);
        //   * 取过但失败的只有"再点一次"(retry)才重试 —— 失败不会自己无限重试。
        const QVector<int> order = catalogSourceOrder(m_sourceSetting);
        QVector<int> pending;
        for (int source : order) {
            const auto it = m_catalogCache.constFind(catalogCacheKey(kindId, source));
            const bool cached = (it != m_catalogCache.constEnd());
            if (cached && it->ok)
                break;
            if (!cached || retry)
                pending.append(source);
        }
        if (pending.isEmpty()) {
            fillLoaderRow(kindId); // 缓存里已经有结论:直接回填,一个字节都不联网
            return;
        }
        if (LoaderRow *row = rowForKind(kindId))
            row->setLoading(true); // 取数期间如实显示"加载中…"
        state.busy = true;

        const QString mc = m_versionId;
        // 页面可能在取数期间被销毁(会话池淘汰临时页)。QPointer + 以页面为 context 的
        // invokeMethod:页面没了就不回填,绝不拿悬空指针去碰界面。
        QPointer<DownloadConfigPage> guard(this);
        auto *thread = QThread::create([guard, kindId, mc, pending] {
            const QVector<LoaderCatalogAttempt> attempts = fetchCatalogAttempts(kindId, mc, pending);
            if (guard.isNull())
                return;
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, kindId, mc, attempts] {
                    if (!guard.isNull())
                        guard->onLoaderCatalogLoaded(kindId, mc, attempts);
                },
                Qt::QueuedConnection);
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }

    // 工作线程回来(主线程):先把每一路的结果记进缓存,再回填界面。
    void onLoaderCatalogLoaded(const QString &kindId, const QString &mc,
                               const QVector<LoaderCatalogAttempt> &attempts) {
        for (const LoaderCatalogAttempt &attempt : attempts) {
            LoaderCatalogSourceResult entry;
            entry.ok = attempt.ok;
            entry.error = attempt.error;
            entry.versions = attempt.versions;
            m_catalogCache.insert(catalogCacheKey(kindId, static_cast<int>(attempt.source)), entry);
        }
        LoaderRowFetchState &state = m_catalogState[kindId];
        state.busy = false;
        state.mc = mc;
        fillLoaderRow(kindId);
    }

    // 用缓存里的结论回填一行(**不联网**)。源顺序按设置走:第一条能用的就用它;
    // 两条都不行 -> setError 写**两路的真实原因**(不是"加载失败"这种空话)。
    void fillLoaderRow(const QString &kindId) {
        LoaderRow *row = rowForKind(kindId);
        if (row == nullptr)
            return;
        LoaderRowFetchState &state = m_catalogState[kindId];
        const QVector<int> order = catalogSourceOrder(m_sourceSetting);
        QStringList failures;
        for (int source : order) {
            const auto it = m_catalogCache.constFind(catalogCacheKey(kindId, source));
            if (it == m_catalogCache.constEnd())
                continue; // 这一路还没取过(排在成功那一路后面,或本轮没轮上)
            if (it->ok) {
                state.hasData = true;
                state.failed = false;
                state.mc = m_versionId;
                row->setVersions(it->versions); // 顺序/标记都是核心库给的,这里不再排一遍
                uiTrace(QStringLiteral("【loader-ui | kind=%1 mc=%2 source=%3 rows=%4 summary=%5】")
                            .arg(kindId, m_versionId,
                                 catalogSourceName(static_cast<sxcl_catalog_source>(source)),
                                 QString::number(row->listCount()), row->summaryText()));
                if (kindId == QLatin1String("optifine"))
                    updateOptifineStatus(true, static_cast<int>(it->versions.size()));
                return;
            }
            failures << QStringLiteral("%1: %2")
                            .arg(catalogSourceName(static_cast<sxcl_catalog_source>(source)),
                                 it->error);
        }
        state.hasData = false;
        state.failed = true;
        const QString reason = failures.isEmpty() ? QStringLiteral("取不到加载器版本列表")
                                                  : failures.join(QStringLiteral("; "));
        row->setError(reason);
        uiTrace(QStringLiteral("【loader-ui | kind=%1 mc=%2 rows=0 summary=%3】")
                    .arg(kindId, m_versionId, reason));
        if (kindId == QLatin1String("optifine"))
            updateOptifineStatus(false, 0);
    }

    // OptiFine 状态行(docs/04-页面规格.md §2.7):初始"检测中…" -> 有数据 "✅ 支持"(success)
    // / 无数据 "—"(tertiary)。它以前永远停在"检测中…",因为没人给它结论。
    void updateOptifineStatus(bool ok, int count) {
        if (m_optifineStatus == nullptr)
            return;
        const bool supported = ok && count > 0;
        const ThemeTokens &tokens = FluentTheme::instance().tokens();
        m_optifineStatus->setText(supported ? QStringLiteral("✅ 支持") : QStringLiteral("—"));
        m_optifineStatus->setTextColor(supported ? tokens.success : tokens.textTertiary);
        uiTrace(QStringLiteral("【loader-ui | kind=optifine mc=%1 count=%2 status=%3】")
                    .arg(m_versionId, QString::number(count), m_optifineStatus->text()));
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
    // (:138 的 m_nameTaken 是只写不读的死状态,已删 —— "有没有被占"由 checkVersionExists
    //  现算现用,判定在核心库那一份里,界面不再存一份可能过期的副本。)
    bool m_userEditedName = false; // :139

    // 加载器取数层(文件上半部分 fetchCatalogAttempts 的调用方)
    QString m_sourceSetting;                               // 页面打开时的下载源(uiDownloadSource)
    QHash<QString, LoaderCatalogSourceResult> m_catalogCache; // 键 = (加载器, MC, 来源)
    QHash<QString, LoaderRowFetchState> m_catalogState;      // 每一行的取数状态
};

} // namespace

QWidget *createDownloadConfigPage(const VersionRef &version, QWidget *parent) {
    return new DownloadConfigPage(version, parent);
}

} // namespace sxcl::ui
