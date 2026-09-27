/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "page_factory.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QScrollBar>
#include <QSet>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <functional>

#if defined(_MSC_VER)
#pragma warning(push, 0) // libqf 的头在 /W4 下不是零警告,整体静音(见 libqf.h 的说明)
#endif
#include "../ui_icons.h"              // 自绘 svg 小图标(警告符/叉号)+ 版本行的状态图标
#include <QApplication>                  // 取证通路:把滚轮事件发给视口(SXCL_UI_LIST)
#include <QScrollBar>                    // 列表状态读数(范围/步长/当前位置)
#include <QShowEvent>                    // 逐行取证:这一份实例真的上屏时再打坐标
#include <QWheelEvent>                   // 取证通路:证明"滚轮真的能滚到旧版本"
#include "fluent/fluent_controls.h"      // PushButton / InfoBar
#include "fluent/fluent_input.h"         // SearchLineEdit
#include "fluent/fluent_labels.h"        // TitleLabel / SubtitleLabel / BodyLabel
#include "fluent/fluent_progress.h"      // IndeterminateProgressRing
#include "fluent/fluent_scroll.h"        // ScrollArea(= Python qf ScrollArea)
#include "fluent/fluent_setting_cards.h" // ComboBox
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "fluent_theme.h"
#include "sxcl_icons.h"
#include "workers/bg_task.h"       // 一次性后台任务(阻塞活进工作线程,结果回界面线程)
#include "workers/instance_scan.h" // 已安装版本扫描(界面层唯一实现;**只许工作线程调**)
#include "workers/ui_error.h"  // 统一错误出口:完整上下文 + 自动复制剪贴板
#include "workers/ui_paths.h"  // uiLauncherDataRoot():缓存/配置根必须跨平台(Android 没有 %APPDATA%)

// ── 核心库(纯 C)—— 版本页的取数链全部走这里,界面层不再自己解析清单 ──
// 以前这一页只能读本地缓存/环境变量里的清单文件,拿不到就退化成"把本地已安装的实例
// 当成版本清单显示" —— 那是两种不同的东西互相冒充。现在:
//   远端清单:核心库双路(官方 piston-meta -> BMCLAPI 镜像)+ sxcl_json + sxcl_version_list_build
//   本地已安装:sxcl_instance_scan(带加载器标签与 problem 人话)
//   两者分开呈现;清单失败(网络/解析)与"没安装任何版本"是两种状态。
#include "sxcl/http.h"      // sxcl_http_get_text(双路取清单文本)
#include "sxcl/instance.h"  // sxcl_instance_scan(本地已安装实例)
#include "sxcl/json.h"      // sxcl_json_parse_file / sxcl_json_parse
#include "sxcl/manifest.h"  // sxcl_version_list_build + sxcl_manifest_mirror_url
#include "sxcl/net.h"       // sxcl_transport_qt_create(Qt 传输后端)
#include "sxcl/paths.h"     // sxcl_paths_default_game_dir(平台默认游戏目录;不再自己拼 APPDATA)

// 注意:sxcl_ui_core 目前没有源码树 include/ 目录的搜索路径(include/ 只挂在可执行目标
// sxcl-ui 上),所以这里**够不着** sxcl/instance.h。等主代理把那行 include 目录补进
// sxcl_ui_core(或让 sxcl_ui_core 链 sxcl)之后,把本地扫描换成
// sxcl_instance_scan(),加载器小标签与 problem 提示就会跟着来(见最终报告)。

namespace sxcl::ui {
namespace {

// ──────────────────────────────────────────────────────────────── 令牌小工具

QColor tokenColor(const QString &name) {
    return FluentTheme::parseCssColor(FluentTheme::instance().tokenText(name));
}

// ──────────────────────────────────────────────────────────────── 数据模型

// Python GameVersion(manifest.py:72-96):id / version_type / url / release_time / sha1 / size
struct GameVersion {
    QString id;
    QString type;
    QString url;
    QString releaseTime;
    QString sha1;
    qint64 size = 0;

    int category() const { // Python GameVersion.category
        if (type == QLatin1String("release"))
            return 1;
        if (type == QLatin1String("snapshot"))
            return 2;
        return 3;
    }
    // Python GameVersion.release_label: releaseTime -> "YYYY-MM-DD"
    QString releaseLabel() const {
        const QDateTime dt = QDateTime::fromString(releaseTime, Qt::ISODate);
        return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd")) : releaseTime;
    }
};

QString typeText(const QString &type) { // versions_page.py:184
    if (type == QLatin1String("release"))
        return QStringLiteral("正式版");
    if (type == QLatin1String("snapshot"))
        return QStringLiteral("快照");
    if (type == QLatin1String("old"))
        return QStringLiteral("旧版");
    return type;
}

/* 版本类型 -> 方块图 kind(用户 2026-09-26:「版本下载版本儿左面儿的那个草方块儿,你忘了」)。
 *   release -> Grass.png(vanilla) / snapshot -> CommandBlock.png / old_beta|old_alpha -> CobbleStone.png。
 * 映射**只有一份**:统一走 sxcl_icons.cpp 的 stateIconKind + kFiles,这里不另造表。
 * 清单里出现认不出的类型时返回**空串** —— 宁可这一格空着,也不拿别的方块冒充它。 */
QString typeBlockKind(const QString &type) {
    const QString t = type.toLower();
    static const char *const kKnown[] = {"release", "snapshot", "old", "old_beta", "old_alpha"};
    for (const char *k : kKnown) {
        if (t == QLatin1String(k))
            return SxclIcons::stateIconKind(t, QStringList(), false);
    }
    return QString();
}

// icons.py:67-75 LOADER_COLORS(硬编码十六进制,Python 里就不是令牌)
QString loaderColor(const QString &kind) {
    static const QHash<QString, QString> kColors = {
        {QStringLiteral("forge"), QStringLiteral("#c9a227")},
        {QStringLiteral("neoforge"), QStringLiteral("#e08a3c")},
        {QStringLiteral("fabric"), QStringLiteral("#b5752f")},
        {QStringLiteral("quilt"), QStringLiteral("#b04ac0")},
        {QStringLiteral("optifine"), QStringLiteral("#3f8fd0")},
        {QStringLiteral("liteloader"), QStringLiteral("#9a8f5f")},
        {QStringLiteral("vanilla"), QStringLiteral("#5fa03a")},
    };
    return kColors.value(kind, QStringLiteral("#8a8a8a"));
}

// loaders.py LOADER_NAMES
QString loaderName(const QString &kind) {
    static const QHash<QString, QString> kNames = {
        {QStringLiteral("vanilla"), QStringLiteral("原版")},
        {QStringLiteral("forge"), QStringLiteral("Forge")},
        {QStringLiteral("neoforge"), QStringLiteral("NeoForge")},
        {QStringLiteral("fabric"), QStringLiteral("Fabric")},
        {QStringLiteral("quilt"), QStringLiteral("Quilt")},
        {QStringLiteral("optifine"), QStringLiteral("OptiFine")},
        {QStringLiteral("liteloader"), QStringLiteral("LiteLoader")},
    };
    return kNames.value(kind, kind);
}

// icons.py:222-232 loader_chip_text:">18 字符只留名字"
QString loaderChipText(const QString &kind, const QString &version) {
    const QString name = loaderName(kind);
    const QString v = version.trimmed();
    if (v.isEmpty())
        return name;
    const QString text = name + QLatin1Char(' ') + v;
    return text.size() <= 18 ? text : name;
}

// ──────────────────────────────────────────────────────────────── 模型

class VersionListModel : public QAbstractListModel {
public:
    enum Roles {
        RowRole = Qt::UserRole, // Python: Qt.UserRole(= 该行的 GameVersion)
        InstalledRole = Qt::UserRole + 1,
        LoadersRole = Qt::UserRole + 2, // QVariantList<QStringList{kind, version}>
        ProblemRole = Qt::UserRole + 3, // 装过但起不来时的**一句话**(不是核心库那段完整原因)
        StateRole = Qt::UserRole + 4,   // 行首状态图标 id:"grass" / "warn" / 空 = 这行没装过
    };

    explicit VersionListModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

    void setVersions(const QVector<GameVersion> &versions, const QSet<QString> &installed) {
        beginResetModel();
        m_items = versions;
        m_installed = installed;
        endResetModel();
    }
    void setInstalled(const QSet<QString> &installed) {
        m_installed = installed;
        if (!m_items.isEmpty())
            emit dataChanged(index(0, 0), index(int(m_items.size()) - 1, 0), {InstalledRole});
    }
    void setLoaders(const QHash<QString, QVariantList> &mapping) {
        m_loaders = mapping;
        if (!m_items.isEmpty())
            emit dataChanged(index(0, 0), index(int(m_items.size()) - 1, 0),
                             {LoadersRole, ProblemRole});
    }
    void setProblems(const QHash<QString, QString> &mapping) {
        m_problems = mapping;
        if (!m_items.isEmpty())
            emit dataChanged(index(0, 0), index(int(m_items.size()) - 1, 0), {ProblemRole});
    }
    void setStates(const QHash<QString, QString> &mapping) {
        m_states = mapping;
        if (!m_items.isEmpty())
            emit dataChanged(index(0, 0), index(int(m_items.size()) - 1, 0), {StateRole});
    }
    void setDetails(const QHash<QString, QString> &mapping) { m_details = mapping; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : int(m_items.size());
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
            return {};
        const GameVersion &v = m_items.at(index.row());
        switch (role) {
        case Qt::DisplayRole:
            return v.id;
        case RowRole:
            return index.row(); // 行号;取 GameVersion 用 versionAt()
        case InstalledRole:
            return m_installed.contains(v.id);
        case LoadersRole:
            return m_loaders.value(v.id);
        case ProblemRole:
            return m_problems.value(v.id);
        case StateRole:
            return m_states.value(v.id);
        case Qt::ToolTipRole: {
            /* 装过但起不来的行:tooltip = 版本名 + **完整原因** + 它在哪个目录
             * (用户 2026-09-26:「详细原因与路径放 tooltip」)—— 行内那点地方只放一句话。 */
            const QString detail = m_details.value(v.id);
            if (!detail.isEmpty())
                return detail;
            // 其余行不评判"装没装过"（用户点名）；只报这一行是什么版本。
            return QStringLiteral("%1\n%2 | %3").arg(v.id, v.type, v.releaseLabel());
        }
        default:
            return {};
        }
    }

    const GameVersion &versionAt(int row) const { return m_items.at(row); }
    QVariantList loadersAt(int row) const { return m_loaders.value(m_items.at(row).id); }
    QString problemAt(int row) const { return m_problems.value(m_items.at(row).id); }
    QString stateAt(int row) const { return m_states.value(m_items.at(row).id); }
    QString detailAt(int row) const { return m_details.value(m_items.at(row).id); }
    bool installedAt(int row) const { return m_installed.contains(m_items.at(row).id); }
    const QVector<GameVersion> &items() const { return m_items; }

private:
    QVector<GameVersion> m_items;
    QSet<QString> m_installed;
    QHash<QString, QVariantList> m_loaders;
    QHash<QString, QString> m_problems; // id -> 行内那一句话(含动作词)
    QHash<QString, QString> m_states;   // id -> 行首图标状态
    QHash<QString, QString> m_details;  // id -> tooltip 全文(完整原因 + 路径)
};

// ──────────────────────────────────────────────────────────────── 行自绘

class VersionRowDelegate : public QStyledItemDelegate {
public:
    // versions_page.py:150-155
    enum {
        ROW_HEIGHT = 52,
        BTN_H = 28,
        LOG_W = 84,
        MARGIN = 16,
        // 行首那个状态图标位(用户 2026-09-26):ICON = 边长,ICON_LEFT = 原来文字的左距
        // (图标就长在文字该在的位置上),ICON_GAP = 图标与版本名之间的呼吸。
        ICON = 20,
        ICON_LEFT = 16,
        ICON_GAP = 10,
    };

    explicit VersionRowDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return QSize(0, ROW_HEIGHT);
    }

    /* 行右侧只剩「版本日志」一枚(用户 2026-09-26:「获取服务端先不做后续再说」——
     * 一枚点了只会说"开发中"的死入口删掉;功能以后真做出来再加回来)。
     * 按钮**仍然贴右边距**:右边缘 = 行右 - MARGIN,不因为少了一枚就飘到中间。 */
    static void buttonRects(const QRect &rect, QRect *logRect) {
        const int y = rect.top() + (rect.height() - BTN_H) / 2;
        *logRect = QRect(rect.right() - MARGIN - LOG_W, y, LOG_W, BTN_H);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        if (!index.isValid() || !index.model())
            return;
        const VersionListModel *model = static_cast<const VersionListModel *>(index.model());
        const GameVersion &version = model->versionAt(index.row());

        painter->save(); // versions_page.py:166-260
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect rect = option.rect.adjusted(0, 2, 0, -2);
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        const ThemeTokens &t = FluentTheme::instance().tokens();

        painter->setPen(Qt::NoPen);
        painter->setBrush(
            tokenColor(hovered ? QStringLiteral("hoverStrong") : QStringLiteral("hover")));
        painter->drawRoundedRect(rect, 6, 6);

        /* 行首图标位 —— **每一行都画**(用户 2026-09-26:「版本下载版本儿左面儿的那个草方块儿,
         * 你忘了」)。原来的口径是"装过才画、没装过留白",用户要的是**每一行的左边都有图标**:
         *   按**版本类型**取方块图(正式版 = 草方块 Grass.png / 快照 = 命令方块 / 旧版 = 圆石);
         *   装过但起不来 -> 仍然由我们自绘的红色警告符**盖过**方块图(优先级最高);
         *   认不出的类型或素材缺失 -> 这一格空着,不拿别的方块冒充。
         * "装没装过"早就不该在下载清单上评判(用户 2026-09-22 点名),所以这回也不再用
         * "有没有图标"来表达它。这一列宽度恒定,免得同一列里文字起点忽左忽右。 */
        const int iconLeft = rect.left() + ICON_LEFT;
        const QString state = model->stateAt(index.row());
        QPixmap rowIcon;
        if (state == QLatin1String(version_state::kWarn)) {
            rowIcon = uiVersionStatePixmap(version_state::kWarn, ICON);
        } else {
            const QString kind = typeBlockKind(version.type);
            if (!kind.isEmpty())
                rowIcon = SxclIcons::instance().blockPixmap(kind, ICON);
        }
        if (!rowIcon.isNull()) {
            painter->drawPixmap(QRect(iconLeft, rect.top() + (rect.height() - ICON) / 2, ICON, ICON),
                                rowIcon);
        }
        const int left = iconLeft + ICON + ICON_GAP;
        QFont font = painter->font();
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(t.text);
        painter->drawText(QRect(left, rect.top(), 144, rect.height()),
                          Qt::AlignVCenter | Qt::AlignLeft, version.id);

        font.setBold(false);
        painter->setFont(font);
        painter->setPen(t.textTertiary);
        painter->drawText(QRect(left + 152, rect.top(), 64, rect.height()),
                          Qt::AlignVCenter | Qt::AlignLeft, typeText(version.type));
        painter->drawText(QRect(left + 226, rect.top(), 110, rect.height()),
                          Qt::AlignVCenter | Qt::AlignLeft, version.releaseLabel());

        {
            const QVariantList loaders = model->loadersAt(index.row());
            const QString problem = model->problemAt(index.row());
            /* 这里以前会画一个绿字"已安装" + 状态方块图。**2026-09-22 晚按用户的话删掉**：
             *   「用户下无数个同版本你也管不着？游戏下载用得着你告诉用户那个下载过了？」
             * "版本 = versions/ 下的文件夹名"，同一个原版装几个都正常，这份清单是**下载**用的，
             * 不该在这上面评判用户装没装过。留下的是真正有用的信息：
             * 这个 MC 版本下已经装了哪些**加载器**、以及"装了却启动不了"的原因。 */
            int chipX = left + 340;
            const QFontMetrics metrics = painter->fontMetrics();
            const int shown = qMin(3, int(loaders.size()));
            for (int i = 0; i < shown; ++i) {
                const QStringList pair = loaders.at(i).toStringList();
                if (pair.size() < 2)
                    continue;
                const QString text = loaderChipText(pair.at(0), pair.at(1));
                const int width = metrics.horizontalAdvance(text) + 16;
                if (chipX + width > rect.right() - 8)
                    break;
                const QRect chip(chipX, rect.top() + (rect.height() - 20) / 2, width, 20);
                const QColor color(loaderColor(pair.at(0)));
                QColor fill(color);
                fill.setAlpha(48);
                painter->setPen(QPen(color, 1));
                painter->setBrush(fill);
                painter->drawRoundedRect(chip, 4, 4);
                painter->setPen(color);
                painter->drawText(chip, Qt::AlignCenter, text);
                chipX += width + 6;
            }

            /* 装过但起不来:chip 里只放**一句话 + 动作**(展示口径 versionRowInfo 给的),
             * 不再把核心库那段完整原因倒进行里 —— 用户 2026-09-26:「那行小字把原因全倒出来,
             * 读着像报错」。完整原因与路径在整行的 tooltip 里(Qt::ToolTipRole)。
             * 这颗 chip 以前还带一颗自绘警示三角:行首已经有**同一套**状态图标了,
             * 同一个状态标两遍就是噪音,这里删掉(三角本身留在键位页/多人页那些地方用)。 */
            if (!problem.isEmpty()) {
                const QString text = problem;
                const int width = metrics.horizontalAdvance(text) + 20;
                if (chipX + width <= rect.right() - 8) {
                    const QRect chip(chipX, rect.top() + (rect.height() - 20) / 2, width, 20);
                    const QColor color = t.danger;
                    QColor fill(color);
                    fill.setAlpha(48);
                    painter->setPen(QPen(color, 1));
                    painter->setBrush(fill);
                    painter->drawRoundedRect(chip, 4, 4);
                    painter->setPen(color);
                    painter->drawText(chip, Qt::AlignCenter, text);
                }
            }
            painter->setBrush(Qt::NoBrush);
        }

        if (hovered) {
            QRect logRect;
            buttonRects(rect, &logRect);
            painter->setPen(QPen(t.accent));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(logRect, 4, 4);
            painter->drawText(logRect, Qt::AlignCenter, QStringLiteral("版本日志"));
        }
        painter->restore();
    }
};

// ──────────────────────────────────────────────────────────────── 视图

// 行内两枚按钮的命中测试(versions_page.py:273-301):
// 松手时 Qt 不给 MouseOver,所以不能在 delegate 里判 —— 由视图自己算矩形。
class VersionListView : public QListView {
public:
    // action: 0 = 无, 1 = wiki(版本日志)。(2 = server 已按用户 2026-09-26 删除)
    std::function<void(int row, int action)> onAction;

    explicit VersionListView(QWidget *parent = nullptr) : QListView(parent) {}

protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            const QModelIndex index = indexAt(event->position().toPoint());
            // 本视图的 delegate 一定是 VersionRowDelegate(见 buildContent);
            // 它没走 moc,所以用 static_cast 而不是 qobject_cast。
            auto *delegate = static_cast<VersionRowDelegate *>(itemDelegate());
            if (index.isValid() && delegate) {
                QRect logRect;
                VersionRowDelegate::buttonRects(visualRect(index).adjusted(0, 2, 0, -2), &logRect);
                const QPoint pos = event->position().toPoint();
                if (logRect.contains(pos)) {
                    emitRowAction(index.row(), 1);
                    event->accept();
                    return;
                }
            }
        }
        QListView::mouseReleaseEvent(event);
    }

private:
    void emitRowAction(int row, int action) {
        if (onAction)
            onAction(row, action);
    }
};

// ──────────────────────────────────────────────────────────────── 版面骨架(BasePage)

// Python src/app/common/base_page.py:32-66
class PageScaffold : public ScrollArea {
public:
    PageScaffold(const QString &title, const QString &subtitle, QWidget *parent = nullptr)
        : ScrollArea(parent) {
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // 页面底色钉令牌 bg(#202020):抓参考图时 Python 也是这么钉的(grab_reference_ui.py:141
        //   page.setStyleSheet("QWidget { background: %s }" % token("bg")))——参考图的内容区
        // 就是 #202020,不是内容栈那层半透明白。栈自己的 rgba(255,255,255,0.0314) 只该在
        // 窗口左上圆角那半像素露出来(见 main_window.cpp 的圆角取证),页面不钉底色整片会变 #272727。
        setStyleSheet(QStringLiteral("QScrollArea { background: %1; }")
                          .arg(FluentTheme::instance().tokens().bg.name()));

        m_view = new QWidget(this);
        m_view->setStyleSheet(QStringLiteral("background: transparent;"));
        setWidget(m_view);
        m_box = new QVBoxLayout(m_view);
        m_box->setContentsMargins(28, 24, 28, 24);
        m_box->setSpacing(16);
        m_box->setAlignment(Qt::AlignTop);
        m_title = new TitleLabel(title, m_view);
        m_subtitle = new SubtitleLabel(subtitle, m_view);
        m_subtitle->setTextColor(QColor(QStringLiteral("#606060")),
                                 QColor(QStringLiteral("#AAAAAA")));
        m_box->addWidget(m_title);
        m_box->addWidget(m_subtitle);
    }

    QWidget *view() const { return m_view; }
    QVBoxLayout *box() const { return m_box; }
    QWidget *titleLabel() const { return m_title; }
    QWidget *subtitleLabel() const { return m_subtitle; }

private:
    QWidget *m_view = nullptr;
    QVBoxLayout *m_box = nullptr;
    TitleLabel *m_title = nullptr;
    SubtitleLabel *m_subtitle = nullptr;
};

// ──────────────────────────────────────────────────────────────── 取数据

// 共享配置(Python 版写的那份,过渡期共用)挂在**启动器数据根**下。
// 以前这里直接拼 %APPDATA%,APPDATA 为空时返回空串 -> 读不到任何配置(Android 上恒如此);
// 现在路径由 ui_paths.cpp 的 uiLauncherDataRoot() 按平台给出,读不到只是"文件不存在",
// 而不是"路径不存在"。
QString sharedConfigPath() {
    return uiLauncherDataRoot() + QStringLiteral("/config.json");
}

QString sharedConfigValue(const QString &section, const QString &key, const QString &def) {
    QFile f(sharedConfigPath());
    if (!f.open(QIODevice::ReadOnly))
        return def;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return def;
    const QJsonValue v = doc.object().value(section).toObject().value(key);
    if (v.isString())
        return v.toString(def);
    if (v.isDouble())
        return QString::number(v.toDouble(), 'g', 10);
    return def;
}

// 游戏目录:先看验收/调试入口,再看共享配置(Python 版写的那份,过渡期共用),
// 最后退回**核心库的平台默认**(不再自己拼 %APPDATA%)。
//
// 为什么最后一跳必须走核心库:安卓上 qgetenv("APPDATA") 恒为空 ——
// 旧实现会掉到 "~/.minecraft",而安卓的 $HOME 是 "/",那个路径根本不存在,
// 于是版本页永远扫不到本地实例(同一个缺陷家族:清单缓存 :576 那处也是 %APPDATA% 为空就没兜底)。
// sxcl_paths_default_game_dir 在 Windows 上给的就是 %APPDATA%/.minecraft(**桌面行为不变**),
// 安卓上给 <应用私有 files>/.minecraft(与安装/启动用的那一个同源,见 paths.c)。
QString gameDirectory() {
    const QString env = qEnvironmentVariable("SXCL_UI_GAME_DIR");
    if (!env.isEmpty())
        return env;
    const QString cfg = sharedConfigValue(QStringLiteral("Game"),
                                          QStringLiteral("gameDirectory"), QString());
    if (!cfg.isEmpty())
        return cfg;
    char buf[4096];
    char err[256];
    if (sxcl_paths_default_game_dir(buf, sizeof(buf), err, sizeof(err)) == SXCL_PATHS_OK)
        return QDir::fromNativeSeparators(QString::fromUtf8(buf));
    return QDir::fromNativeSeparators(QDir::homePath()) + QStringLiteral("/.minecraft");
}

// ── 本地已安装实例:走 workers/instance_scan.h(界面层唯一实现)──
// 原来这里自己抄了一份"枚举 + 装箱";现在两页(版本页 / 版本选择页)共用同一份,
// 而且**只在工作线程里调**(界面线程一次都不许直接扫盘 —— 见 loadVersions 的说明)。
// 判据、加载器标签、problem 人话全部来自核心库 sxcl_instance_scan。

const char *kManifestOfficialUrl = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

// 清单缓存路径。**保证非空** —— 这正是"设备上拉不到版本列表"的根因之一:
// 旧实现返回空串时,写缓存与读缓存两处都被 if(!cache.isEmpty()) 静默跳过,
// 于是远端不通 = 连兜底都没有(桌面有 %APPDATA% 才看不出来)。
QString manifestCachePath() {
    return uiLauncherDataRoot() + QStringLiteral("/cache/version_manifest_v2.json");
}

// 清单文本:**远端双路**(官方 -> BMCLAPI 镜像),失败才退回本地缓存。
// 返回空 = 连缓存都没有;error 里是真实原因(网络/状态码/解析),界面照实显示。
// 明确区分三件事(这是之前踩过的坑,别再退回去):
//   1) 远端拿到了            -> 正常路径,顺手写缓存;
//   2) 远端不通但有缓存      -> 用缓存,并在状态里说明"用的是本地缓存";
//   3) 两条路都不通且没缓存  -> 空 + 人话原因(界面走**错误态**,不是空态)。
QByteArray fetchManifestText(QString *error, bool *fromCache, QString *notice) {
    if (error)
        error->clear();
    if (fromCache)
        *fromCache = false;
    if (notice)
        notice->clear();

    // 1) 环境变量指定的清单文件:验收/离线复现用的显式入口,优先级最高(与老行为一致)
    const QString env = qEnvironmentVariable("SXCL_UI_MANIFEST");
    if (!env.isEmpty()) {
        QFile f(env);
        if (f.open(QIODevice::ReadOnly))
            return f.readAll();
        if (error)
            *error = QStringLiteral("SXCL_UI_MANIFEST 指定的文件打不开: ") + env;
        return {};
    }

    // 2) 远端双路。transport 拿不到(没编进 Qt 传输后端)时如实说明,不假装成功。
    QStringList failures;
#if defined(SXCL_UI_HAVE_QT_TRANSPORT)
    sxcl_transport *tr = sxcl_transport_qt_create();
    if (tr != nullptr) {
        char *text = nullptr;
        size_t len = 0;
        char err[256];
        char mirror[512];
        mirror[0] = '\0';
        const bool haveMirror =
            sxcl_manifest_mirror_url(kManifestOfficialUrl, nullptr, mirror, sizeof(mirror)) == 0;

        // 取证通路:把"官方源"这个位置换成一个**连不上的地址**,用来确定性地复现
        // "官方超时 -> 自动切 BMCLAPI -> 弹窗告知"这条分支(否则两条 URL 都是真的,
        // 想让官方不通就得改机器网络,要管理员权限 —— 与 SXCL_UI_MANIFEST_URL 同一族入口)。
        // 不设时**完全走原路**,生产行为一个字节不变。
        const QByteArray officialOverride = qEnvironmentVariable("SXCL_UI_MANIFEST_OFFICIAL").toUtf8();
        const char *const officialUrl =
            officialOverride.isEmpty() ? kManifestOfficialUrl : officialOverride.constData();

        // ★ 候选顺序**按设置里的下载源排**(用户报过"设置里换了源,刷新还是走 Mojang" ——
        //   以前这里写死"官方在前镜像在后",download.source 根本没人读,等于设置是个摆设):
        //     bmclapi(默认) / auto -> BMCLAPI 优先,不通**自动**切回官方
        //     mojang             -> 官方优先,不通**自动**切回 BMCLAPI
        //   "自动切"就是这张表的第二条候选:换源由下面这个循环自己完成,不需要开关。
        const QString source = uiDownloadSource();
        const bool mirrorFirst = (source != QLatin1String("mojang"));
        const char *urls[3];
        int urlCount = 0;
        if (mirrorFirst && haveMirror)
            urls[urlCount++] = mirror;
        urls[urlCount++] = officialUrl;
        if (!mirrorFirst && haveMirror)
            urls[urlCount++] = mirror;
        urls[urlCount] = nullptr;
        uiTrace(QStringLiteral("versions | 清单源=%1 顺序=%2")
                    .arg(source, QString::fromUtf8(urls[0])));

        // 验收/离线复现:钉死清单地址(SXCL_UI_MANIFEST_URL)。
        // 与上面那个 SXCL_UI_MANIFEST(钉死"文件")是同一族入口,区别只在钉的是"地址":
        //   SXCL_UI_MANIFEST_URL=<url> -> **只试这一个地址,不换镜像**。
        // 为什么需要它:"远端不通 + 有缓存 -> 走缓存并警告"这条分支原先没法测 ——
        // 两台 URL 都是真的,想让它不通就得改机器网络(改 hosts/防火墙,要管理员权限)。
        // 钉一个连不上的地址(http://127.0.0.1:9/…)就能**确定性地**复现这条分支。
        // 生产路径不设这个变量,行为与从前完全一致。
        QByteArray pinnedUrl;
        const QString pinnedEnv = qEnvironmentVariable("SXCL_UI_MANIFEST_URL");
        if (!pinnedEnv.isEmpty()) {
            pinnedUrl = pinnedEnv.toUtf8();
            urls[0] = pinnedUrl.constData();
            urls[1] = nullptr; // 钉住之后不再换镜像:要测的就是"两条路都不通"
        }
        // 超时按"这条路是谁"分开给(用户要求:选了官方源,超时要等久一点再判超时,
        // 判超时之后**要弹窗告诉用户已经切到 BMCLAPI**,不能悄悄换):
        //   官方:piston-meta / launchermeta 在部分地区很慢 -> 20s
        //   镜像:BMCLAPI 就在国内 -> 8s,不行就赶紧退回去
        // 这两个数字只影响"等多久算失败",不影响成功路径。
        const int64_t kOfficialTimeoutMs = 20000;
        const int64_t kMirrorTimeoutMs = 8000;
        // ★ 循环条件必须**同时**看 urlCount 与 nullptr:钉地址那次会把 urls[1] 置空,
        //   只按 urlCount 走会拿着 nullptr 去请求(实测是自己给自己挖的坑)。
        for (int i = 0; i < urlCount && urls[i] != nullptr; ++i) {
            text = nullptr;
            len = 0;
            err[0] = '\0';
            const bool isOfficial =
                urls[i] == officialUrl; // 比指针:两个候选就是这两个常量之一
            sxcl_http_opts hopts;
            std::memset(&hopts, 0, sizeof(hopts));
            hopts.timeout_ms = isOfficial ? kOfficialTimeoutMs : kMirrorTimeoutMs;
            const int rc =
                sxcl_http_get_text_ex(tr, urls[i], nullptr, &hopts, &text, &len, err, sizeof(err));
            if (rc == SXCL_HTTP_OK && text != nullptr && len > 0) {
                // ★ 走的是第二条路,而且第一条是**官方** -> 就是"官方源超时,已切 BMCLAPI"
                //   这一条必须让用户看见(他只会在设置里选了官方源,才期待走官方)。
                if (i > 0 && notice != nullptr) {
                    const bool firstWasOfficial = (urls[0] == officialUrl);
                    if (firstWasOfficial) {
                        *notice = QStringLiteral(
                                      "官方源超时（等待 %1 秒未完成），已自动切换到 BMCLAPI 镜像源")
                                      .arg(kOfficialTimeoutMs / 1000);
                        uiTrace(QStringLiteral("versions | 官方源超时 -> 已切换 BMCLAPI"));
                    }
                }
                QByteArray body(text, static_cast<int>(len));
                free(text);
                // 顺手写缓存(下次断网可用);写不进去不是错误
                const QString cache = manifestCachePath();
                if (!cache.isEmpty()) {
                    // 目录可能还不存在(全新设备/Android 私有目录)-> 先建再写
                    QDir().mkpath(QFileInfo(cache).absolutePath());
                    QFile cf(cache);
                    if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate))
                        cf.write(body);
                }
                return body;
            }
            if (text)
                free(text);
            failures << QStringLiteral("%1: %2")
                            .arg(QString::fromUtf8(urls[i]),
                                 QString::fromUtf8(err[0] ? err : "没有内容"));
        }
    } else if (error) {
        failures << QStringLiteral("没有可用的网络后端(sxcl_net_qt 未链接)");
    }
#else
    failures << QStringLiteral("本产物没有编译进 Qt 传输后端(sxcl_net_qt)");
#endif

    // 3) 本地缓存:只在远端不通时用,而且要告诉用户"这是旧数据"
    const QString cache = manifestCachePath();
    if (!cache.isEmpty()) {
        QFile f(cache);
        if (f.open(QIODevice::ReadOnly)) {
            if (fromCache)
                *fromCache = true;
            return f.readAll();
        }
    }
    if (error)
        *error = failures.isEmpty() ? QStringLiteral("取不到版本清单")
                                    : failures.join(QStringLiteral("; "));
    return {};
}

// 清单 JSON -> GameVersion 列表。解析走核心库(sxcl_json + sxcl_version_list_build),
// 界面层不再自己认字段 —— 清单结构一旦变,只有核心库一处要改。
// 解析失败会填 error(给界面显示真实原因),并且**返回空列表**(调用方走错误态)。
QVector<GameVersion> parseManifest(const QByteArray &text, QString *error) {
    QVector<GameVersion> out;
    if (error)
        error->clear();
    if (text.isEmpty()) {
        if (error)
            *error = QStringLiteral("清单内容是空的");
        return out;
    }
    char err[256];
    err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(text.constData(), static_cast<size_t>(text.size()), err,
                                      sizeof(err));
    if (doc == nullptr) {
        if (error)
            *error = QStringLiteral("清单不是合法 JSON: ") + QString::fromUtf8(err);
        return out;
    }
    sxcl_version_list *list = sxcl_version_list_build(doc);
    if (list == nullptr) {
        if (error)
            *error = QStringLiteral("清单里没有 versions 数组(结构不对)");
        sxcl_json_free(doc);
        return out;
    }
    const size_t count = sxcl_version_list_count(list);
    out.reserve(static_cast<int>(count));
    for (size_t i = 0; i < count; ++i) {
        const sxcl_version_entry *e = sxcl_version_list_at(list, i);
        if (e == nullptr || e->id == nullptr || e->id[0] == '\0')
            continue;
        GameVersion v;
        v.id = QString::fromUtf8(e->id);
        v.type = QString::fromUtf8(e->type ? e->type : "release");
        v.url = QString::fromUtf8(e->url ? e->url : "");
        v.sha1 = QString::fromUtf8(e->sha1 ? e->sha1 : "");
        v.size = static_cast<qint64>(e->size);
        out.append(v);
    }
    sxcl_version_list_free(list);
    sxcl_json_free(doc);
    if (out.isEmpty() && error)
        *error = QStringLiteral("清单解析成功但一个版本都没有");
    return out;
}

// 版本号倒序(与清单顺序同向:新版本在前)
QVector<GameVersion> sortDescending(QVector<GameVersion> items) {
    std::stable_sort(items.begin(), items.end(), [](const GameVersion &a, const GameVersion &b) {
        return a.releaseTime > b.releaseTime;
    });
    return items;
}

// 过滤(manifest.py:148-179 filter_versions)
QVector<GameVersion> filterVersions(const QVector<GameVersion> &all, const QString &query,
                                    int category) {
    const QString q = query.trimmed().toLower();
    QVector<GameVersion> filtered;
    for (const GameVersion &v : all) {
        if (category != 0 && v.category() != category)
            continue;
        if (!q.isEmpty() && !v.id.toLower().contains(q))
            continue;
        filtered.append(v);
    }
    return filtered;
}

// ──────────────────────────────────────────────────────────────── 页面

/* **裸内容**:工具栏 + 状态行 + 版本列表。它有两种"住法":
 *
 *   * 嵌在下载页右列那一格(草方块那一栏)—— 下载页自己已经有**一层**滚动外壳(PageShell),
 *     这里再自带 ScrollArea 就是**套两层**:用户 2026-09-26 原话「右面儿的菜单上下不要再套两层了;
 *     套两层压缩完只有 1/4 大小」。实测(1100x750、修复前)版本框只有 643x349,而右列可用区是
 *     701x677 —— 里面那层的 28px 页边距 + 标题副标题又吃掉一圈,列表被压成不到一半。
 *   * 独立路由页(侧边栏"版本"那一项,或版本选择页齿轮进来的"版本管理")——
 *     由 VersionsRoutePage 在外面**包一层** PageScaffold(页名 + 页面边距 + 滚动),
 *     内容一模一样。
 * 列表自己会滚(QListView),所以"只留一层容器"之后竖向滚动仍然可用。 */
class VersionsPage : public QWidget {
public:
    explicit VersionsPage(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("VersionsPage"));
        m_box = new QVBoxLayout(this); // 页边距 = 0:外层(下载页右列 / VersionsRoutePage)负责留白
        m_box->setContentsMargins(0, 0, 0, 0);
        /* 行距 16 -> 8:用户 2026-09-26 一直在说"别套两层、给足空间",而不说废话的页面
         * (工具栏/状态行/加载态三样平时两样是藏着的)本来就不需要 16px 的行距 —— 实测
         * 列表净高因此多出 16px(见 tools/ui_versions_list.ps1 的读数)。 */
        m_box->setSpacing(8);
        m_box->setAlignment(Qt::AlignTop);
        m_fetch = new BgTask(this);
        buildContent();
        loadVersions();
    }

    QVBoxLayout *box() const { return m_box; }

    /** 工具栏那一行(搜索 + 分类 + 刷新)。独立路由页要把它和**页名摆成同一行** ——
     *  用户:"把上面那 94 像素的页内元素(页名/说明/搜索框)压到最小(搜索框可以挪到与页名同一行)"。 */
    QWidget *toolbar() const { return m_toolbar; }

private:
    void buildContent() {
        // ---- 工具栏(versions_page.py:321-348)----
        auto *toolbar = new QWidget(this);
        m_toolbar = toolbar;
        auto *toolbarLayout = new QHBoxLayout(toolbar);
        toolbarLayout->setContentsMargins(0, 0, 0, 0);
        toolbarLayout->setSpacing(12);

        m_search = new SearchLineEdit(toolbar);
        m_search->setPlaceholderText(QStringLiteral("搜索版本号…"));
        m_search->setClearButtonEnabled(true);
        connect(m_search, &QLineEdit::textChanged, this, [this] { m_reloadTimer->start(); });

        // 搜索防抖:每敲一个字符就重建全部卡片(实测 0.6~0.8 秒/字符)
        m_reloadTimer = new QTimer(this);
        m_reloadTimer->setSingleShot(true);
        m_reloadTimer->setInterval(260);
        connect(m_reloadTimer, &QTimer::timeout, this, [this] { reloadList(); });

        m_category = new ComboBox(toolbar);
        m_category->addItems({QStringLiteral("全部"), QStringLiteral("正式版"),
                              QStringLiteral("快照"), QStringLiteral("旧版")});
        m_category->setCurrentIndex(1); // 默认"正式版"
        connect(m_category, &QComboBox::currentIndexChanged, this, [this] { reloadList(); });

        m_refresh = new PushButton(QStringLiteral("刷新"), toolbar);
        connect(m_refresh, &QPushButton::clicked, this, [this] { loadVersions(); });

        toolbarLayout->addWidget(m_search, 1);
        toolbarLayout->addWidget(m_category);
        toolbarLayout->addWidget(m_refresh);
        box()->addWidget(toolbar);

        // ---- 加载中(居中旋转圈 + 文字,versions_page.py:352-363)----
        m_loading = new QWidget(this);
        auto *loadingLayout = new QVBoxLayout(m_loading);
        loadingLayout->setAlignment(Qt::AlignCenter);
        m_spinner = new IndeterminateProgressRing(m_loading);
        m_spinner->setFixedSize(48, 48);
        m_loadingLabel = new BodyLabel(QStringLiteral("正在加载版本清单…"), m_loading);
        m_loadingLabel->setAlignment(Qt::AlignCenter);
        loadingLayout->addStretch(2);
        loadingLayout->addWidget(m_spinner, 0, Qt::AlignCenter);
        loadingLayout->addSpacing(12);
        loadingLayout->addWidget(m_loadingLabel, 0, Qt::AlignCenter);
        loadingLayout->addStretch(3);
        box()->addWidget(m_loading);

        // ---- 状态行(versions_page.py:366-367)+ 失败时的**重试**键 ----
        // 用户口径(2026-09-26):失败不能只写一行红字 —— 得给一个**能点**的重试。
        // 所以状态行与「重试」并排:平时重试藏着,清单没拿到时露出来(点它重跑一遍
        // 加载:本地扫描 + 清单双路,全部在工作线程里)。
        m_status = new BodyLabel(QString(), this);
        m_status->setVisible(false);
        m_retry = new PushButton(QStringLiteral("重试"), this);
        m_retry->setVisible(false);
        connect(m_retry, &QPushButton::clicked, this, [this] { loadVersions(); });
        /* 状态行那一**整行**平时是藏着的:两个子控件都藏了,行本身留着的话,布局里
         * 那一格 + 上下两条行距白吃 ~20 像素(实测列表因此矮一截)。用户要的是"给足空间",
         * 而这一行平时本来就一句话都没有(见 setStatusText 的说明)。 */
        m_statusRow = new QWidget(this);
        auto *statusLay = new QHBoxLayout(m_statusRow);
        statusLay->setContentsMargins(0, 0, 0, 0);
        statusLay->setSpacing(12);
        statusLay->addWidget(m_status, 1);
        statusLay->addWidget(m_retry, 0, Qt::AlignVCenter);
        m_statusRow->setVisible(false);
        box()->addWidget(m_statusRow);

        // ---- 虚拟化列表(versions_page.py:370-392)----
        m_list = new VersionListView(this);
        m_list->setObjectName(QStringLiteral("versionList"));
        m_list->setUniformItemSizes(true);
        m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_list->setSelectionMode(QAbstractItemView::NoSelection);
        m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_list->setMouseTracking(true);
        m_list->setAttribute(Qt::WA_Hover, true);
        m_list->viewport()->setAttribute(Qt::WA_Hover, true);
        m_list->setMinimumHeight(260);
        // Python 版这段样式来自 app 的 global_qss(theme.py:194-228);C 版还没有全局 QSS,
        // 就按同一份规则的逐条搬到本页(滚动手柄用 border_strong、hover 用 text_tertiary)。
        m_list->setStyleSheet(
            QStringLiteral("QListView { background: transparent; border: none; }\n"
                           "QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }"
                           "QScrollBar::handle:vertical { background: %1; border-radius: 4px;"
                           " min-height: 28px; }"
                           "QScrollBar::handle:vertical:hover { background: %2; }"
                           "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }"
                           "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }")
                .arg(tokenColor(QStringLiteral("borderStrong")).name(),
                     tokenColor(QStringLiteral("textTertiary")).name()));

        m_model = new VersionListModel(this);
        m_delegate = new VersionRowDelegate(this);
        m_list->setModel(m_model);
        m_list->setItemDelegate(m_delegate);
        connect(m_list, &QListView::clicked, this, [this](const QModelIndex &index) {
            if (index.isValid())
                openDownloadConfig(m_model->versionAt(index.row()));
        });
        m_list->onAction = [this](int row, int action) {
            if (row < 0 || row >= m_model->items().size())
                return;
            if (action == 1)
                openVersionWiki(m_model->versionAt(row));
        };

        // 主题一变就重画(对应 Python on_theme_changed(lambda: viewport.update()))
        connect(&FluentTheme::instance(), &FluentTheme::changed, m_list->viewport(),
                [this] { m_list->viewport()->update(); m_status->update(); });

        box()->addWidget(m_list, 1);
    }

    // ---- 数据 ----

    void loadVersions() { // versions_page.py:399-410
        // 骨架先出来:转圈 + 一行"正在加载…",列表清空、刷新键禁用(避免连点排队)
        m_loading->setVisible(true);
        m_loadingLabel->setText(QStringLiteral("正在加载版本清单…"));
        m_status->setVisible(false);
        m_retry->setVisible(false);
        syncStatusRow();
        m_model->setVersions({}, m_installed);
        m_refresh->setEnabled(false);

        // 取数据放到工作线程(对应 Python 的 FetchWorker);UI 线程只负责回填。
        // 两件阻塞活都在这一条工作线程上:**本地已安装扫描**(扫盘)+ **远端清单双路**
        // (官方 -> BMCLAPI,失败才退缓存)。界面线程一个字节都不等。
        if (m_fetch->running())
            return; // 上一轮还在跑:等它回来(刷新键此时是禁用的)
        m_fetch->start(
            QStringLiteral("versions-manifest"),
            [this] {
                // ① 本地已安装实例:与清单**各算各的**(清单不通也要能显示"你装了哪些")
                m_instances = scanInstalledInstances(gameDirectory(), nullptr);
                // ② 远端清单:核心库双路(官方 -> BMCLAPI),失败才退缓存
                m_fetchError.clear();
                m_fetchNotice.clear();
                m_fetchFromCache = false;
                const QByteArray text = fetchManifestText(&m_fetchError, &m_fetchFromCache, &m_fetchNotice);
                m_fetchVersions.clear();
                if (!text.isEmpty())
                    m_fetchVersions = parseManifest(text, &m_fetchError);
                // 线程取证在 BgTask 里统一打(bg-task[versions-manifest] 那一行)
            },
            [this] { onLoaded(m_fetchVersions, m_fetchError, !m_fetchVersions.isEmpty(), m_fetchFromCache, m_fetchNotice); });
    }

    // 只剩"本地已安装"时用来铺列表:此时状态行会**明说**这不是版本清单,
    // 行内的"已安装"标记照旧 —— 两种数据来源绝不互相冒充。
    QVector<GameVersion> installedOnlyVersions() const {
        QVector<GameVersion> out;
        out.reserve(m_instances.size());
        for (const InstalledInstance &inst : m_instances) {
            GameVersion v;
            v.id = inst.id;
            v.type = inst.type;
            out.append(v);
        }
        return sortDescending(out);
    }
    // 三态(别再合并):
    //   1) 清单拿到了          -> m_all = 远端清单;本地已安装只在行上打标记;
    //   2) 清单拿不到,但装了版本 -> **错误态**(状态行 + InfoBar 写真实原因),
    //                              列表另起一段只显示本地已安装,并在状态行里说明;
    //   3) 清单拿不到,也没装版本 -> 错误态 + "本地也没有已安装的版本"(空态文案)同时给出,
    //                              两者是两句话,不会互相冒充。
    void onLoaded(const QVector<GameVersion> &versions, const QString &error, bool ok,
                  bool fromCache, const QString &notice) {
        m_refresh->setEnabled(true);
        m_loading->setVisible(false);
        refreshInstalled(); // 状态行的可见性由 setStatusText()/错误分支按"有没有话说"决定

        if (ok) {
            m_manifestOk = true;
            m_all = versions;
            applyFilters();
            setStatusText();
            showVersions(m_filtered);
            if (!notice.isEmpty())
                // 用户点名要的那条:"官方源超时,已切换 BMC…" —— 弹出来,不悄悄换源
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("已自动切换下载源"), notice,
                              this, 8000);
            if (fromCache)
                InfoBar::push(InfoBar::Type::Warning, QStringLiteral("用的是本地缓存清单"),
                              QStringLiteral("远端清单暂时取不到，本次显示的是上次缓存下来的版本清单。"),
                              this, 5000);
            logState(ok, versions.size(), fromCache, error);
            return;
        }

        // 清单没拿到 = 错误态。原因照实说(网络 / 状态码 / 解析),绝不吞掉。
        m_manifestOk = false;
        m_all = installedOnlyVersions(); // 只放"本地已安装",不是清单
        applyFilters();
        const QString reason = error.isEmpty() ? QStringLiteral("未知原因") : error;
        const QString localNote =
            m_all.isEmpty()
                ? QStringLiteral("本地也没有已安装的版本(全新设备属于正常情况,去「下载」页装一个即可)")
                : QStringLiteral("下面列出的是**本地已安装**的 %1 个版本，不是远端清单")
                      .arg(m_all.size());
        m_manifestNote = QStringLiteral("版本清单加载失败：%1 · %2").arg(reason, localNote);
        m_status->setText(m_manifestNote);
        m_status->setVisible(true); // 错误必须有话说(这一行 + 重试键都要露出来)
        m_retry->setVisible(true); // 失败也要能用:给一个能点的重试(不是只写一行红字)
        syncStatusRow();
        // 统一错误出口:InfoBar 里只放短句,**完整上下文(页面/操作/原始原因/路径/版本)
        // 一并进剪贴板** —— 用户报障时直接粘,不用再问"什么错"。
        UiErrorContext ctx;
        ctx.page = QStringLiteral("版本页 / versions");
        ctx.action = QStringLiteral("获取版本清单");
        ctx.reason = reason; // 核心库人话(网络/状态码/解析),原样进剪贴板
        ctx.detail = QStringLiteral("游戏目录:%1 · 清单缓存:%2 · 本地已安装:%3 个")
                         .arg(QDir::toNativeSeparators(gameDirectory()),
                              QDir::toNativeSeparators(manifestCachePath()))
                         .arg(m_instances.size());
        ctx.title = QStringLiteral("版本清单加载失败");
        pushUiError(this, ctx, 6000);
        showVersions(m_filtered);
        logState(ok, versions.size(), fromCache, error);
    }

    // 验收/取证通路(Android 真机也看这条 logcat):一行说清这一页现在的状态 ——
    // 清单从哪儿来、几个版本、本地装了几个、状态行原文。截图看不出文案时靠它。
    void logState(bool ok, int manifestCount, bool fromCache, const QString &error) const {
        // 源(自动/官方/镜像)也打出来:用户报"换了源还走 Mojang"时,这一行就是判据。
        std::fprintf(stderr,
                     "[sxcl-ui] 版本页: 源=%s 清单=%s 版本数=%d 已安装=%d 缓存=%s 原因=%s 状态行=%s\n",
                     uiDownloadSource().toUtf8().constData(), ok ? "OK" : "失败", manifestCount,
                     int(m_instances.size()), fromCache ? "是" : "否",
                     error.isEmpty() ? "-" : error.toUtf8().constData(),
                     m_status->text().toUtf8().constData());
        /* 加载器汇总另打一行**纯 ASCII**(kind=数量),免得中文/编码把取证信息糊掉 */
        QString ascii;
        for (const InstalledInstance &inst : m_instances) {
            for (const QVariant &entry : inst.loaders) {
                const QStringList pair = entry.toStringList();
                if (!pair.isEmpty())
                    ascii += pair.at(0) + QLatin1Char(' ');
            }
        }
        std::fprintf(stderr, "[sxcl-ui] 版本页-加载器: %s\n",
                     ascii.isEmpty() ? "(none)" : ascii.trimmed().toUtf8().constData());
    }

    void applyFilters() { // versions_page.py:625-638
        static const int kCategoryMap[4] = {0, 1, 2, 3};
        int category = 0;
        const int idx = m_category->currentIndex();
        if (idx >= 0 && idx < 4)
            category = kCategoryMap[idx];
        m_filtered = filterVersions(m_all, m_search->text(), category);
    }

    void reloadList() { // versions_page.py:640-642
        applyFilters();
        showVersions(m_filtered);
    }

    void showVersions(const QVector<GameVersion> &versions) { // versions_page.py:494-503
        refreshInstalled();
        m_model->setVersions(versions, m_installed);
        setStatusText();
        scheduleRowEvidence(); // 行首图标是自绘的:坐标与状态由这一页自己报(见上)
    }

    // versions_page.py:412-451:已安装集合 + 加载器小标签 + problem 提示。
    // 这三样都来自核心库 sxcl_instance_scan(以前界面层自己读 JSON,拿不到加载器与原因)。
    void refreshInstalled() {
        m_installed.clear();
        m_loaders.clear();
        m_problems.clear();
        m_states.clear();
        m_details.clear();
        const QString gameDir = gameDirectory();
        for (const InstalledInstance &inst : m_instances) {
            m_installed.insert(inst.id);
            if (!inst.loaders.isEmpty())
                m_loaders.insert(inst.id, inst.loaders);
            /* 行首图标状态 / 行内那一句话 / tooltip 全文 —— 全部来自**同一个**展示口径
             * (workers/instance_scan.h 的 VersionRowInfo,与版本选择页共用)。
             * 以前这里塞进 chip 的是核心库那段**完整原因**("需要安装 1.12.2 作为前置版本"),
             * 31 个字压在 20px 高的 chip 里,读着像报错(用户 2026-09-26 点名)。
             * 现在 chip 只有"一句话 · 动作",完整原因与路径进 tooltip。 */
            const VersionRowInfo row = versionRowInfo(inst, gameDir);
            if (!row.note.isEmpty())
                m_problems.insert(inst.id, QStringLiteral("%1 · %2").arg(row.note, row.action));
            m_states.insert(inst.id, row.state);
            if (!inst.launchable && !row.tip.isEmpty())
                m_details.insert(inst.id, row.tip);
        }
        m_model->setLoaders(m_loaders);
        m_model->setProblems(m_problems);
        m_model->setStates(m_states);
        m_model->setDetails(m_details);
    }

    /* 列表**自己**的状态读数(一行,stderr):模型多少行、视口放得下几行、滚动条的范围与步长。
     * 为什么必须有它:截图与控件树都分不出"模型只有 5 行"和"模型 916 行、屏幕只放得下 10 行"
     * —— 用户报"就 5 个版本"时,这一行就是判据。fitsRows = 视口高 / 行高(含最后一行露出一部分)。 */
    void printListTrace() const {
        if (m_model == nullptr || m_list == nullptr)
            return;
        /* **只报在屏幕上那一份**:同一刻可能有两份实例(下载页右列那一份 + 独立路由页那一份),
         * 藏着的那一份的几何是布局过程中的中间值,拿它当"用户看到几行"是假读数。 */
        if (!m_list->isVisible())
            return;
        // 这一份到底是谁(祖先里最近几个有名字的控件)—— 两份实例的读数靠它区分
        QString host;
        for (QWidget *w = parentWidget(); w != nullptr && host.count(QLatin1Char(0x2F)) < 3;
             w = w->parentWidget()) {
            if (!w->objectName().isEmpty())
                host += w->objectName() + QLatin1Char(0x2F);
        }
        const QScrollBar *bar = m_list->verticalScrollBar();
        const int rowH = VersionRowDelegate::ROW_HEIGHT;
        const int vw = m_list->viewport()->width();
        const int vh = m_list->viewport()->height();
        const int rows = m_model->rowCount();
        const int fits = (vh + rowH - 1) / rowH;
        const QPoint origin = m_list->viewport()->mapTo(m_list->window(), QPoint(0, 0));
        const QModelIndex first = m_list->indexAt(QPoint(vw / 2, 1));
        const QModelIndex last = m_list->indexAt(QPoint(vw / 2, vh - 2));
        const QString firstId = first.isValid() ? m_model->versionAt(first.row()).id : QStringLiteral("-");
        const QString lastId = last.isValid() ? m_model->versionAt(last.row()).id : QStringLiteral("-");
        std::fprintf(stderr,
                     "[sxcl-ui] versions-list: host=%s rows=%d all=%d rowH=%d viewport=%dx%d fitsRows=%d "
                     "scrollMin=%d scrollMax=%d pageStep=%d value=%d top=%d,%d "
                     "firstVisible=%s lastVisible=%s\n",
                     host.toUtf8().constData(), rows, int(m_all.size()), rowH, vw, vh, fits,
                     bar != nullptr ? bar->minimum() : -1, bar != nullptr ? bar->maximum() : -1,
                     bar != nullptr ? bar->pageStep() : -1, bar != nullptr ? bar->value() : -1,
                     origin.x(), origin.y(), firstId.toUtf8().constData(),
                     lastId.toUtf8().constData());
        /* 排序口径的取证:模型前 5 条 / 后 5 条的真实版本号。模型顺序 = 清单顺序
         * (sxcl_version_list_build 给的,manifest 本身就是新版在前;界面不再二次排序)。 */
        QString head;
        QString tail;
        for (int i = 0; i < rows; ++i) {
            const QString id = m_model->versionAt(i).id;
            if (i < 5)
                head += (head.isEmpty() ? QString() : QStringLiteral(",")) + id;
            if (i >= rows - 5)
                tail += (tail.isEmpty() ? QString() : QStringLiteral(",")) + id;
        }
        std::fprintf(stderr, "[sxcl-ui] versions-list-order: host=%s head=%s tail=%s\n",
                     host.toUtf8().constData(), head.toUtf8().constData(),
                     tail.toUtf8().constData());
    }

    /* 取证通路 SXCL_UI_LIST:把列表**真的**滚一下,证明"老版本在后面"不是猜的。
     *   SXCL_UI_LIST=bottom -> 滚动条拖到底(与用户拖手柄等价),再打一次读数;
     *   SXCL_UI_LIST=wheel  -> 先往视口发 5 次滚轮事件(与用户滚轮等价),打滚了多远,再拖到底。
     * 只驱动产品控件(滚动条 / 视口事件),不改任何页面状态、不动模型。 */
    void applyListProbe() {
        const QString spec = qEnvironmentVariable("SXCL_UI_LIST").trimmed().toLower();
        if (spec.isEmpty() || m_list == nullptr)
            return;
        if (m_model == nullptr || m_model->rowCount() == 0)
            return; // 清单还没回来:没有东西可滚(这一趟不报,免得留下 value=0/0 的假读数)
        QScrollBar *bar = m_list->verticalScrollBar();
        if (bar == nullptr)
            return;
        if (spec == QLatin1String("wheel")) {
            QWidget *vp = m_list->viewport();
            const int before = bar->value();
            for (int i = 0; i < 5; ++i) {
                const QPoint pos(vp->width() / 2, vp->height() / 2);
                QWheelEvent wheel(QPointF(pos), QPointF(vp->mapToGlobal(pos)), QPoint(),
                                  QPoint(0, -360), Qt::NoButton, Qt::NoModifier,
                                  Qt::NoScrollPhase, false);
                QApplication::sendEvent(vp, &wheel);
            }
            std::fprintf(stderr, "[sxcl-ui] versions-list-scroll: spec=wheel events=5 from=%d to=%d\n",
                         before, bar->value());
        }
        bar->setValue(bar->maximum());
        std::fprintf(stderr, "[sxcl-ui] versions-list-scroll: spec=%s bottom value=%d/%d\n",
                     spec.toUtf8().constData(), bar->value(), bar->maximum());
        printListTrace();
    }

    /* 取证通路 SXCL_UI_CATEGORY=all|release|snapshot|old:在**真的**分类下拉框里换一档
     * (等价于用户点下拉框;不绕过去自己改模型)。为什么要它:清单里不同**版本类型**的行
     * 要同屏才验得到"每行左侧的方块图按类型取",而默认那一档是"正式版"。
     * 换档走 m_category->setCurrentIndex —— 与用户操作同一条信号链(currentIndexChanged
     * -> reloadList)。已经是这一档就什么都不做,免得白重建一次列表。 */
    void applyCategoryProbe() {
        const QString spec = qEnvironmentVariable("SXCL_UI_CATEGORY").trimmed().toLower();
        if (spec.isEmpty() || m_category == nullptr)
            return;
        static const QHash<QString, int> kMap = {{QStringLiteral("all"), 0},
                                                 {QStringLiteral("release"), 1},
                                                 {QStringLiteral("snapshot"), 2},
                                                 {QStringLiteral("old"), 3}};
        if (!kMap.contains(spec))
            return;
        const int idx = kMap.value(spec);
        if (m_category->currentIndex() == idx)
            return;
        m_category->setCurrentIndex(idx);
        std::fprintf(stderr, "[sxcl-ui] versions-category: spec=%s idx=%d rows=%d\n",
                     spec.toUtf8().constData(), idx, m_model != nullptr ? m_model->rowCount() : -1);
    }

    /* 逐行取证(stderr):这一列的行首图标是**自绘**的(控件树里没有它),所以坐标由这一页
     * 自己报 —— 报的是**窗口坐标**下的图标槽矩形,验收脚本拿它去截图上数像素。
     * 只在"这一份实例真的在屏幕上"时打(下载页里还嵌着同一份,藏着的那份不该混进来)。
     * 与版本选择页那一行同一个格式(前缀 version-row page),只写事实、不改任何状态。 */
    void printRowEvidence() {
        if (m_model == nullptr || m_list == nullptr)
            return;
        QWidget *win = m_list->window();
        if (win == nullptr)
            return;
        const QVector<GameVersion> &items = m_model->items();
        const QPoint origin = m_list->viewport()->mapTo(win, QPoint(0, 0));
        const QScrollBar *bar = m_list->verticalScrollBar();
        const int scroll = (bar != nullptr) ? bar->value() : 0;
        const QRect viewportRect = m_list->viewport()->rect();
        for (int i = 0; i < items.size(); ++i) {
            /* **每一行都报**(2026-09-26 之后):行首图标改成按**版本类型**画,每一行都有图标,
             * 所以"没装过的行留白、没有状态可报"这个前提没了 —— 报的是行上**真的画了什么**:
             *   state = 实例状态(grass/warn/空,来自核心库判据)
             *   block = 版本类型对应的方块图 kind(vanilla/snapshot/old/none)
             *   icon  = 这一格**实际**画出来的东西(warn 盖过方块图;认不出类型 = none) */
            const QString state = m_model->stateAt(i);
            /* 行矩形**自己算**,不问 visualRect:这一页的模型刚 setVersions 完、视图的布局
             * 还没跑,visualRect 会给出上一版布局的坐标(实测整体偏了一行的距离,取证脚本
             * 照它去截图上数像素会数到别的行)。判据只有一条 —— 委托就是按
             * (0, i*行高 - 滚动量, 视口宽, 行高) 画的(setUniformItemSizes(true) + spacing 0)。 */
            const int icon = VersionRowDelegate::ICON; // 与委托同一个常量,不写第二遍
            const QRect rowRect(0, i * VersionRowDelegate::ROW_HEIGHT - scroll, viewportRect.width(),
                                VersionRowDelegate::ROW_HEIGHT);
            const QRect rowDraw = rowRect.adjusted(0, 2, 0, -2); // 委托里的 option.rect
            const QRect slot(origin.x() + rowDraw.left() + VersionRowDelegate::ICON_LEFT,
                             origin.y() + rowDraw.top() + (rowDraw.height() - icon) / 2, icon, icon);
            QString chip = m_model->problemAt(i);
            QString tip = m_model->detailAt(i);
            chip.replace(QLatin1Char('\n'), QStringLiteral(" | "));
            tip.replace(QLatin1Char('\n'), QStringLiteral(" | "));
            /* 这一行**实际**画出来的方块图:类型映射(typeBlockKind)+ 警告符盖过方块图的规则,
             * 与委托 paint 里那份是同一套判据;block=none/icon=none 表示这一格真的没画东西。 */
            const QString blockKind = typeBlockKind(items.at(i).type);
            const QString block = blockKind.isEmpty() ? QStringLiteral("none") : blockKind;
            const QString iconKind = (state == QLatin1String(version_state::kWarn))
                                         ? QString::fromLatin1(version_state::kWarn)
                                         : block;
            /* 行右侧那枚「版本日志」的矩形:由**委托自己的** buttonRects 算,取证脚本不另写一份
             * 几何(用户 2026-09-26 删掉「获取服务端」之后,它必须仍然贴右边距)。 */
            QRect logRect;
            VersionRowDelegate::buttonRects(rowDraw, &logRect);
            std::fprintf(stderr,
                         "[sxcl-ui] version-row page: id=%s state=%s iconRect=%d,%d,%dx%d "
                         "chip=\"%s\" tip=\"%s\" vis=%d inView=%d list=%d,%d,%dx%d win=%dx%d "
                         "block=%s icon=%s logRect=%d,%d,%dx%d\n",
                         items.at(i).id.toUtf8().constData(), state.toUtf8().constData(),
                         slot.x(), slot.y(), slot.width(), slot.height(),
                         chip.toUtf8().constData(), tip.toUtf8().constData(),
                         (m_list->isVisible() && win->isVisible()) ? 1 : 0,
                         viewportRect.intersects(rowDraw) ? 1 : 0,
                         // 列表自己的窗口坐标:验收脚本拿它和**dump 里同一时刻**的 #versionList
                         // 对齐 —— 对不上就说明这组坐标是布局跑之前的旧值(那是假读数)
                         m_list->mapTo(win, QPoint(0, 0)).x(),
                         m_list->mapTo(win, QPoint(0, 0)).y(), m_list->width(), m_list->height(),
                         win->width(), win->height(), block.toUtf8().constData(),
                         iconKind.toUtf8().constData(), logRect.x(), logRect.y(),
                         logRect.width(), logRect.height());
        }
    }

    /** 逐行取证是**开关式**的(SXCL_UI_ROW_EVIDENCE=1):它是给验收脚本读的,不是给用户看的。
     *
     * 为什么要开关(用户 2026-09-27):这一页的行数是**整份清单**(实测 206 行),逐行
     * fprintf(stderr, …) 是界面线程上的一次几百毫秒停顿 —— 从终端跑起来时更狠(每行一次
     * 控制台写),现场日志里的 "页面卡住 3008ms" 就有它一份。产品路径上一行都不打。 */
    static bool rowEvidenceWanted() {
        static const bool on = qEnvironmentVariableIntValue("SXCL_UI_ROW_EVIDENCE") == 1;
        return on;
    }

    /** 排一次取证(界面线程,150ms 之后打):showVersions 之后与这一份实例上屏时各排一次。
     *
     * 为什么不是"下一拍"(singleShot(0)):模型刚换完时视图的**布局还没跑**(LayoutRequest
     * 还在队列里),那时量到的 viewport 位置是上一版布局的 —— 实测整体偏了 118px,
     * 取证脚本拿它去截图上数像素会数到空白。150ms 远小于抓图的 5s,不影响任何产品路径。 */
    void scheduleRowEvidence() {
        if (m_evidencePending)
            return;
        if (!rowEvidenceWanted())
            return; // 没开开关:产品路径上不打这几百行(见 rowEvidenceWanted)
        m_evidencePending = true;
        QTimer::singleShot(150, this, [this] {
            m_evidencePending = false;
            applyCategoryProbe(); // 先把分类换到验收要求的那一档(真的下拉框),再报这一屏的行
            printListTrace();
            printRowEvidence();
            applyListProbe();
        });
    }

protected:
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        scheduleRowEvidence(); // 上屏那一刻的行矩形才是可量的
    }

private:
    // versions_page.py:453-464:状态栏那行"已安装几个 + 各加载器各几个"
    // (加载器汇总要 instance.h 的 loader 列表,见文件头 TODO;现在恒为空串,与
    //  Python 在没有加载器时的行为一致)
    // 加载器汇总(versions_page.py:475-503 的那段):"Forge 2 个、Fabric 1 个"
    QString loaderSummaryText() const {
        QHash<QString, int> counts;
        for (const InstalledInstance &inst : m_instances) {
            for (const QVariant &entry : inst.loaders) {
                const QStringList pair = entry.toStringList();
                if (pair.isEmpty())
                    continue;
                const QString kind = pair.at(0);
                if (kind.isEmpty() || kind == QLatin1String("vanilla"))
                    continue;
                counts[kind] += 1;
            }
        }
        if (counts.isEmpty())
            return QString();
        QStringList parts;
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
            parts << QStringLiteral("%1 %2 个").arg(loaderName(it.key())).arg(it.value());
        std::sort(parts.begin(), parts.end());
        return QStringLiteral(" | ") + parts.join(QStringLiteral("、"));
    }
    void setStatusText() { // versions_page.py:475-503(本版按用户 2026-09-26 的文字纪律改了)
        /* 用户 2026-09-26:「共多少个版本也用你说吗?」—— 状态行只在**真的有事要说**时才出现:
         *   * 清单没拿到:写清原因 + 给「重试」(见 onLoaded 的错误分支);
         *   * 筛完一个都不剩:一句"没有匹配的版本"(否则列表空着像坏了);
         * 其余情况**一个字都不写** —— "共 N 个版本 / 当前显示 N 个 / Forge N 个…" 这类计数
         * 全删(列表就在眼前,计数不改变任何操作)。 */
        if (!m_manifestOk) {
            m_status->setText(m_manifestNote);
        } else if (m_filtered.isEmpty() && !m_all.isEmpty()) {
            m_status->setText(QStringLiteral("没有匹配的版本"));
        } else {
            m_status->clear();
        }
        m_status->setVisible(!m_status->text().isEmpty()); // 没话说就别占那一行(会挤列表)
        syncStatusRow();
    }

    /** 状态行那一整行的可见性 = 两个子控件里有没有一个要露脸。 */
    void syncStatusRow() {
        if (m_statusRow == nullptr)
            return;
        m_statusRow->setVisible(m_status->isVisible() || m_retry->isVisible());
    }

    // ---- 交互 ----

    void openDownloadConfig(const GameVersion &v) { // versions_page.py:516-529
        if (QMetaObject::invokeMethod(window(), "switchToDownloadConfig", Qt::DirectConnection,
                                      Q_ARG(QString, v.id)))
            return;
        InfoBar::push(InfoBar::Type::Info, QStringLiteral("下载配置"),
                      QStringLiteral("准备配置 ") + v.id, this, 3000);
    }

    void openVersionWiki(const GameVersion &v) { // versions_page.py:531-541
        const QString country =
            sharedConfigValue(QStringLiteral("System"), QStringLiteral("countryCode"),
                              QStringLiteral("CN"));
        const QString url =
            (country.isEmpty() || country.toUpper() == QLatin1String("CN"))
                ? QStringLiteral("https://zh.minecraft.wiki/w/Java版%1")
                      .arg(QString::fromUtf8(QUrl::toPercentEncoding(v.id)))
                : QStringLiteral("https://minecraft.wiki/w/Java_Edition_%1").arg(v.id);
        QDesktopServices::openUrl(QUrl(url));
    }

    BgTask *m_fetch = nullptr;     // 清单 + 本地扫描的工作线程外壳(loadVersions 里起)
    QVector<GameVersion> m_fetchVersions; // 工作线程写、界面线程读(队列投递保证先后)
    QString m_fetchError;
    QString m_fetchNotice;
    bool m_fetchFromCache = false;

    QWidget *m_toolbar = nullptr;    // 工具栏那一行(独立路由页会把它挪到页名旁边)
    QWidget *m_statusRow = nullptr;  // 状态行 + 「重试」那一行(没话说时整行藏掉)
    SearchLineEdit *m_search = nullptr;
    ComboBox *m_category = nullptr;
    PushButton *m_refresh = nullptr;
    PushButton *m_retry = nullptr; // 清单没拿到时才露出来(失败也要能用)
    QTimer *m_reloadTimer = nullptr;
    QWidget *m_loading = nullptr;
    IndeterminateProgressRing *m_spinner = nullptr;
    BodyLabel *m_loadingLabel = nullptr;
    BodyLabel *m_status = nullptr;
    VersionListView *m_list = nullptr;
    VersionListModel *m_model = nullptr;
    VersionRowDelegate *m_delegate = nullptr;

    bool m_manifestOk = false;  // 1 = 当前列表来自远端清单;0 = 只有本地已安装(错误态)
    QString m_manifestNote;     // 错误态要显示的那句(含真实原因 + 本地有没有已安装)
    QVector<GameVersion> m_all;
    QVector<GameVersion> m_filtered;
    QSet<QString> m_installed;
    QHash<QString, QVariantList> m_loaders;
    QHash<QString, QString> m_problems; // id -> chip 里那一句话(含动作词)
    QHash<QString, QString> m_states;   // id -> 行首图标状态("grass"/"warn")
    QHash<QString, QString> m_details;  // id -> tooltip 全文(完整原因 + 路径)
    bool m_evidencePending = false;     // 取证行只排一次(见 scheduleRowEvidence)
    QVector<InstalledInstance> m_instances;
    QVBoxLayout *m_box = nullptr; // 这一格自己的竖布局(页边距 0;留白由外面那层给)
};

/* 独立路由页(侧边栏"版本"那一项、版本选择页的齿轮进来):在外面**包一层** PageScaffold ——
 * 那是**页面级**的页名与页面边距,不是下载页右列里的重复标题。
 * 用户 2026-09-26 点名删掉的那些话("要装哪个版本 · 官方 / 镜像双路")在这里也没了。 */
class VersionsRoutePage : public PageScaffold {
public:
    explicit VersionsRoutePage(QWidget *parent)
        : PageScaffold(QStringLiteral("Minecraft 版本"), QString(), parent) {
        setObjectName(QStringLiteral("sxclPage_versions"));
        if (QWidget *subtitle = subtitleLabel())
            subtitle->setVisible(false); // 副标题是空串(那句"要装哪个版本…"是废话,已删)
        m_inner = new VersionsPage(this);
        /* 页名与**工具栏摆成同一行**(用户 2026-09-26:「把上面那 94 像素的页内元素
         * (页名/说明/搜索框)压到最小(搜索框可以挪到与页名同一行)」):
         * 以前是"页名 38px 一行 + 行距 + 搜索框 33px 一行",列表上面白吃 ~90 像素;
         * 现在左边页名、右边搜索框/分类/刷新,一行 38px 装下,列表净多出 ~50 像素。
         * PageScaffold 没有"标题右边还有东西"的版式,所以这里把标题从它的竖布局里摘出来,
         * 和自己那一行工具栏并进一个横向行(两个控件都还是原来的实例,样式一个字没改)。 */
        QWidget *title = titleLabel();
        box()->removeWidget(title);
        auto *titleRow = new QWidget(view());
        titleRow->setObjectName(QStringLiteral("sxclVersionsTitleRow")); // 验收 dump 按它认这一行
        auto *titleLay = new QHBoxLayout(titleRow);
        titleLay->setContentsMargins(0, 0, 0, 0);
        titleLay->setSpacing(16);
        titleLay->addWidget(title, 0, Qt::AlignVCenter);
        titleLay->addWidget(m_inner->toolbar(), 1, Qt::AlignVCenter);
        box()->addWidget(titleRow);
        box()->addWidget(m_inner, 1);
    }

private:
    VersionsPage *m_inner = nullptr;
};

} // namespace

/* embedded=true:**嵌在下载页右列那一格**——只给裸内容(下载页自己那层滚动是唯一的容器);
 * embedded=false:独立路由页(自带页名 + 页面边距 + 滚动)。 */
QWidget *createVersionsPage(QWidget *parent, bool embedded) {
    return embedded ? static_cast<QWidget *>(new VersionsPage(parent))
                    : static_cast<QWidget *>(new VersionsRoutePage(parent));
}

} // namespace sxcl::ui
