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
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <functional>

#if defined(_MSC_VER)
#pragma warning(push, 0) // libqf 的头在 /W4 下不是零警告,整体静音(见 libqf.h 的说明)
#endif
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
// sxcl_ui_core(或让 sxcl_ui_core 链 sxcl)之后,把 scanLocalInstances() 换成
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
        ProblemRole = Qt::UserRole + 3,
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
        case Qt::ToolTipRole: {
            const QString state =
                m_installed.contains(v.id) ? QStringLiteral("已安装") : QStringLiteral("未安装");
            return QStringLiteral("%1\n%2 | %3 | %4").arg(v.id, v.type, v.releaseLabel(), state);
        }
        default:
            return {};
        }
    }

    const GameVersion &versionAt(int row) const { return m_items.at(row); }
    QVariantList loadersAt(int row) const { return m_loaders.value(m_items.at(row).id); }
    QString problemAt(int row) const { return m_problems.value(m_items.at(row).id); }
    bool installedAt(int row) const { return m_installed.contains(m_items.at(row).id); }
    const QVector<GameVersion> &items() const { return m_items; }

private:
    QVector<GameVersion> m_items;
    QSet<QString> m_installed;
    QHash<QString, QVariantList> m_loaders;
    QHash<QString, QString> m_problems;
};

// ──────────────────────────────────────────────────────────────── 行自绘

class VersionRowDelegate : public QStyledItemDelegate {
public:
    // versions_page.py:150-155
    enum {
        ROW_HEIGHT = 52,
        BTN_H = 28,
        LOG_W = 84,
        SRV_W = 96,
        GAP = 8,
        MARGIN = 16,
    };

    explicit VersionRowDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return QSize(0, ROW_HEIGHT);
    }

    // versions_page.py:160-164
    static void buttonRects(const QRect &rect, QRect *logRect, QRect *srvRect) {
        const int y = rect.top() + (rect.height() - BTN_H) / 2;
        *srvRect = QRect(rect.right() - MARGIN - SRV_W, y, SRV_W, BTN_H);
        *logRect = QRect(srvRect->left() - GAP - LOG_W, y, LOG_W, BTN_H);
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

        const int left = rect.left() + 16;
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

        if (model->installedAt(index.row())) {
            const QVariantList loaders = model->loadersAt(index.row());
            const QString problem = model->problemAt(index.row());
            QStringList kinds;
            for (const QVariant &item : loaders) {
                const QStringList pair = item.toStringList();
                if (!pair.isEmpty())
                    kinds << pair.at(0);
            }
            // icons.py:78-97 state_icon_kind(草方块 = Minecraft 版本的通用符号)
            const QString iconKind =
                SxclIcons::stateIconKind(version.type, kinds, !problem.isEmpty());
            painter->drawPixmap(
                QRect(left + 340, rect.top() + (rect.height() - 14) / 2, 14, 14),
                SxclIcons::instance().blockPixmap(iconKind, 28));
            painter->setPen(t.success);
            painter->drawText(QRect(left + 360, rect.top(), 60, rect.height()),
                              Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("已安装"));

            // 同一个原版下装了哪些模组加载器(用户经常一个版本装好几套)
            int chipX = left + 424;
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

            // 不能启动的原因直接贴在行里(缺前置/JSON 坏),别等用户点启动才报错
            if (!problem.isEmpty()) {
                const QString text = QStringLiteral("⚠ ") + problem;
                const int width = metrics.horizontalAdvance(text) + 16;
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
            QRect srvRect;
            buttonRects(rect, &logRect, &srvRect);
            painter->setPen(QPen(t.accent));
            painter->setBrush(Qt::NoBrush);
            const QRect rects[2] = {logRect, srvRect};
            const QString labels[2] = {QStringLiteral("版本日志"), QStringLiteral("获取服务端")};
            for (int i = 0; i < 2; ++i) {
                painter->drawRoundedRect(rects[i], 4, 4);
                painter->drawText(rects[i], Qt::AlignCenter, labels[i]);
            }
        }
        painter->restore();
    }
};

// ──────────────────────────────────────────────────────────────── 视图

// 行内两枚按钮的命中测试(versions_page.py:273-301):
// 松手时 Qt 不给 MouseOver,所以不能在 delegate 里判 —— 由视图自己算矩形。
class VersionListView : public QListView {
public:
    // action: 0 = 无, 1 = wiki(版本日志), 2 = server(获取服务端)
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
                QRect srvRect;
                VersionRowDelegate::buttonRects(visualRect(index).adjusted(0, 2, 0, -2), &logRect,
                                                &srvRect);
                const QPoint pos = event->position().toPoint();
                if (logRect.contains(pos)) {
                    emitRowAction(index.row(), 1);
                    event->accept();
                    return;
                }
                if (srvRect.contains(pos)) {
                    emitRowAction(index.row(), 2);
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

// ── 本地已安装实例:走核心库 sxcl_instance_scan ──
// Python scan_installed():versions/<id>/ 下只要有版本 JSON 就算"装好了"
// (Forge 1.13+/Fabric 的实例没有自己的 jar,按 jar+json 判会全漏);
// C 版核心库那份还多给了加载器标签(Forge/Fabric/…)与 problem 人话,
// 以前界面层自己读 JSON 是拿不到这些的(文件头那条 TODO 就是这件事)。
struct LocalInstance {
    QString id;
    QString type;
    QString releaseTime;
    QString summary;  // "原版" / "Forge 47.2.0 + OptiFine I6"
    QString problem;  // 不能启动时的原因(空 = 没问题)
    bool launchable = true;
    QVariantList loaders; // QVariantList<QStringList{kind_id, version}>,与模型/委托的约定一致
};

QVector<LocalInstance> scanLocalInstances(const QString &gameDir, QString *errorOut) {
    QVector<LocalInstance> out;
    if (errorOut)
        errorOut->clear();
    sxcl_instance_list list;
    std::memset(&list, 0, sizeof(list));
    char err[SXCL_INSTANCE_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_instance_scan(gameDir.toUtf8().constData(), nullptr, &list, err, sizeof(err));
    if (rc != SXCL_INSTANCE_OK) {
        if (errorOut)
            *errorOut = QString::fromUtf8(err[0] ? err : "扫描本地实例失败");
        return out;
    }
    out.reserve(static_cast<int>(list.count));
    for (size_t i = 0; i < list.count; ++i) {
        const sxcl_instance &inst = list.items[i];
        LocalInstance item;
        item.id = QString::fromUtf8(inst.id);
        item.type = QString::fromUtf8(inst.version_type[0] ? inst.version_type : "release");
        item.summary = QString::fromUtf8(inst.summary);
        item.problem = QString::fromUtf8(inst.problem);
        item.launchable = inst.launchable != 0;
        for (size_t k = 0; k < inst.loader_count; ++k) {
            const sxcl_instance_loader &ld = inst.loaders[k];
            // 内层是 QStringList{kind_id, version} —— 与模型/委托的约定一致
            // (委托用 item.toStringList() 读它;写成 QVariantList 会让加载器小标签**静默消失**)
            item.loaders.append(QStringList{
                QString::fromUtf8(sxcl_instance_kind_id(ld.kind)),
                QString::fromUtf8(ld.version),
            });
        }
        out.append(item);
    }
    sxcl_instance_list_free(&list);
    return out;
}

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

class VersionsPage : public PageScaffold {
public:
    explicit VersionsPage(QWidget *parent = nullptr)
        : PageScaffold(QStringLiteral("游戏版本"),
                       QStringLiteral("选择要启动的 Minecraft 版本"), parent) {
        setObjectName(QStringLiteral("VersionsPage"));
        buildContent();
        loadVersions();
    }

private:
    void buildContent() {
        // ---- 工具栏(versions_page.py:321-348)----
        auto *toolbar = new QWidget(view());
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
        m_loading = new QWidget(view());
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

        // ---- 状态行(versions_page.py:366-367)----
        m_status = new BodyLabel(QString(), view());
        m_status->setVisible(false);
        box()->addWidget(m_status);

        // ---- 虚拟化列表(versions_page.py:370-392)----
        m_list = new VersionListView(view());
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
            const GameVersion v = m_model->versionAt(row);
            if (action == 1)
                openVersionWiki(v);
            else if (action == 2)
                showServerPlaceholder(v);
        };

        // 主题一变就重画(对应 Python on_theme_changed(lambda: viewport.update()))
        connect(&FluentTheme::instance(), &FluentTheme::changed, m_list->viewport(),
                [this] { m_list->viewport()->update(); m_status->update(); });

        box()->addWidget(m_list, 1);
    }

    // ---- 数据 ----

    void loadVersions() { // versions_page.py:399-410
        m_loading->setVisible(true);
        m_loadingLabel->setText(QStringLiteral("正在加载版本清单…"));
        m_status->setVisible(false);
        m_model->setVersions({}, m_installed);
        m_refresh->setEnabled(false);

        // 取数据放到工作线程(对应 Python 的 FetchWorker);UI 线程只负责回填。
        auto *thread = QThread::create([this] {
            // 1) 本地已安装实例:与清单**各算各的**(清单不通也要能显示"你装了哪些")
            m_instances = scanLocalInstances(gameDirectory(), nullptr);
            // 2) 远端清单:核心库双路(官方 -> BMCLAPI),失败才退缓存
            QString error;
            QString notice; // "官方源超时,已切 BMCLAPI" 这类要弹给用户看的话
            bool fromCache = false;
            const QByteArray text = fetchManifestText(&error, &fromCache, &notice);
            QVector<GameVersion> versions;
            if (!text.isEmpty())
                versions = parseManifest(text, &error);
            const bool ok = !versions.isEmpty();
            QMetaObject::invokeMethod(
                this,
                [this, versions, error, ok, fromCache, notice] {
                    onLoaded(versions, error, ok, fromCache, notice);
                },
                Qt::QueuedConnection);
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }

    // 只剩"本地已安装"时用来铺列表:此时状态行会**明说**这不是版本清单,
    // 行内的"已安装"标记照旧 —— 两种数据来源绝不互相冒充。
    QVector<GameVersion> installedOnlyVersions() const {
        QVector<GameVersion> out;
        out.reserve(m_instances.size());
        for (const LocalInstance &inst : m_instances) {
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
        m_status->setVisible(true);
        refreshInstalled();

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
        for (const LocalInstance &inst : m_instances) {
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
    }

    // versions_page.py:412-451:已安装集合 + 加载器小标签 + problem 提示。
    // 这三样都来自核心库 sxcl_instance_scan(以前界面层自己读 JSON,拿不到加载器与原因)。
    void refreshInstalled() {
        m_installed.clear();
        m_loaders.clear();
        m_problems.clear();
        for (const LocalInstance &inst : m_instances) {
            m_installed.insert(inst.id);
            if (!inst.loaders.isEmpty())
                m_loaders.insert(inst.id, inst.loaders);
            if (!inst.problem.isEmpty())
                m_problems.insert(inst.id, inst.problem);
        }
        m_model->setLoaders(m_loaders);
        m_model->setProblems(m_problems);
    }

    // versions_page.py:453-464:状态栏那行"已安装几个 + 各加载器各几个"
    // (加载器汇总要 instance.h 的 loader 列表,见文件头 TODO;现在恒为空串,与
    //  Python 在没有加载器时的行为一致)
    // 加载器汇总(versions_page.py:475-503 的那段):"Forge 2 个、Fabric 1 个"
    QString loaderSummaryText() const {
        QHash<QString, int> counts;
        for (const LocalInstance &inst : m_instances) {
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
    void setStatusText() { // versions_page.py:475-503
        // 清单没拿到时状态行写的是**错误原因**(见 onLoaded),这句汇总不能把它盖掉。
        if (!m_manifestOk) {
            m_status->setText(m_manifestNote);
            return;
        }
        const QString head = QStringLiteral("共 %1 个版本，已安装 %2 个")
                                 .arg(m_all.size())
                                 .arg(m_installed.size());
        if (m_filtered.isEmpty()) {
            m_status->setText(m_all.isEmpty() ? head : QStringLiteral("没有匹配的版本"));
            return;
        }
        m_status->setText(head + QStringLiteral(" | 当前显示 %1 个").arg(m_filtered.size()) +
                          loaderSummaryText());
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

    void showServerPlaceholder(const GameVersion &v) { // versions_page.py:543-552
        InfoBar::push(InfoBar::Type::Info, QStringLiteral("获取服务端"),
                      QStringLiteral("即将进入 %1 服务端下载页 (功能开发中)").arg(v.id), this, 3000);
    }

    SearchLineEdit *m_search = nullptr;
    ComboBox *m_category = nullptr;
    PushButton *m_refresh = nullptr;
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
    QHash<QString, QString> m_problems;
    QVector<LocalInstance> m_instances;
};

} // namespace

QWidget *createVersionsPage(QWidget *parent) { return new VersionsPage(parent); }

} // namespace sxcl::ui
