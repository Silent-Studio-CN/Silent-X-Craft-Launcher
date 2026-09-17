// versions_page.cpp —— 版本页(Python 版 src/app/pages/versions_page.py 的 1:1 C++ 移植)
//
// 逐条对照(每一处尺寸/文字/颜色都能指回下面某一条,见 docs/05-UI-1to1规格.md):
//   * 版面骨架 = Python src/app/common/base_page.py:32-66(BasePage:ScrollArea + view 的
//     QVBoxLayout margins(28,24,28,24)/spacing 16/AlignTop + TitleLabel + SubtitleLabel),
//     见下面的 PageScaffold;
//   * 工具栏 / 加载中区 / 状态行 / 列表 = versions_page.py:320-397;
//   * 行自绘 = VersionRowDelegate.paint(versions_page.py:145-260),常量逐字照抄:
//     ROW_HEIGHT 52、BTN_H 28、LOG_W 84、SRV_W 96、GAP 8、MARGIN 16,
//     行内文字起点 left+16 / +152(宽 64) / +226(宽 110) / +340(图标 14) / +360(宽 60) / +424(标签);
//   * 文案与过滤 = versions_page.py:453-503、622-645;
//   * 颜色一律走 FluentTheme 令牌(theme.py 的 hover/hover_strong/text/text_tertiary/success/
//     danger/accent);加载器色照抄 icons.py:67-75 LOADER_COLORS,标签文字照抄
//     icons.py:222-232 loader_chip_text 与 loaders.py LOADER_NAMES。
//
// 数据来源(docs/04-页面规格.md §2.2「数据来源」已把 C 版缺口写死):
//   UI 层还没有"取清单文本"的 HTTP 入口 —— sxcl_net_qt 只链进 sxcl-dl,没链进 sxcl-ui,
//   而跨过 CMake 私自链接 Qt6::Network 会破坏"唯一 UI 依赖"的约定(边界:不许改 CMake)。
//   所以本页按下面的顺序取数据,行结构/绘制三条路径完全一致:
//     1) SXCL_UI_MANIFEST=<json 路径> 指定的清单文件(验收/离线复现用的显式入口);
//     2) 共享缓存 <APPDATA>/SilentXCraftLauncher/cache/version_manifest_v2.json(若有);
//     3) 退路:本地实例扫描 sxcl_instance_scan()(离线可用,已安装/加载器标签/problem 全都在);
//     4) 三者都拿不到 -> 走 Python 的错误路径(状态行「加载失败」+ InfoBar.error)。
//   把 sxcl_net_qt 链进 sxcl-ui 后,只需把 fetchManifestText() 换成
//   sxcl_http_get_text(transport, url, ...) —— 解析、过滤、绘制都不用改。
#include "page_factory.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
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

QString sharedConfigPath() {
    const QByteArray appData = qgetenv("APPDATA");
    if (appData.isEmpty())
        return {};
    return QString::fromLocal8Bit(appData) +
           QStringLiteral("/SilentXCraftLauncher/config.json");
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
// 最后退回平台默认的 .minecraft(与 Python cfg.gameDirectory 的默认值同义)。
QString gameDirectory() {
    const QString env = qEnvironmentVariable("SXCL_UI_GAME_DIR");
    if (!env.isEmpty())
        return env;
    const QString cfg = sharedConfigValue(QStringLiteral("Game"),
                                          QStringLiteral("gameDirectory"), QString());
    if (!cfg.isEmpty())
        return cfg;
    const QByteArray appData = qgetenv("APPDATA");
    if (!appData.isEmpty())
        return QString::fromLocal8Bit(appData) + QStringLiteral("/.minecraft");
    return QDir::homePath() + QStringLiteral("/.minecraft");
}

// ── 本地已安装实例(最小实现)──
// Python scan_installed():versions/<id>/ 下只要有版本 JSON 就算"装好了"
// (Forge 1.13+/Fabric 的实例没有自己的 jar,按 jar+json 判会全漏)。
// 这里只做界面需要的那点:目录名 + version_type + releaseTime。
struct LocalInstance {
    QString id;
    QString type;
    QString releaseTime;
};

QVector<LocalInstance> scanLocalInstances(const QString &gameDir) {
    QVector<LocalInstance> out;
    const QDir versionsDir(gameDir + QStringLiteral("/versions"));
    if (!versionsDir.exists())
        return out;
    const QStringList names =
        versionsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        const QDir dir(versionsDir.filePath(name));
        QStringList jsons = dir.entryList({name + QStringLiteral(".json")}, QDir::Files);
        if (jsons.isEmpty())
            jsons = dir.entryList({QStringLiteral("*.json")}, QDir::Files);
        if (jsons.isEmpty())
            continue; // 没有版本 JSON 的目录不算实例(PCL 也跳过)
        LocalInstance inst;
        inst.id = name;
        QFile f(dir.filePath(jsons.first()));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            inst.type = o.value(QStringLiteral("type")).toString();
            inst.releaseTime = o.value(QStringLiteral("releaseTime")).toString();
        }
        if (inst.type.isEmpty())
            inst.type = QStringLiteral("release");
        out.append(inst);
    }
    return out;
}

QString manifestCachePath() {
    const QByteArray appData = qgetenv("APPDATA");
    if (appData.isEmpty())
        return {};
    return QString::fromLocal8Bit(appData) +
           QStringLiteral("/SilentXCraftLauncher/cache/version_manifest_v2.json");
}

// 清单文本(失败时 error 非空)。第 1/2 条路径见文件头说明。
QByteArray fetchManifestText(QString *error) {
    QStringList tried;
    const QString env = qEnvironmentVariable("SXCL_UI_MANIFEST");
    if (!env.isEmpty())
        tried << env;
    tried << manifestCachePath();
    for (const QString &path : tried) {
        if (path.isEmpty())
            continue;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly))
            return f.readAll();
    }
    if (error)
        *error = QStringLiteral("UI 层未链接传输后端(sxcl_net_qt),且本地没有清单缓存");
    return {};
}

// 清单 JSON -> GameVersion 列表(versions_page.py:99-137 的解析部分)
QVector<GameVersion> parseManifest(const QByteArray &text, QString *error) {
    QVector<GameVersion> out;
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(text, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = perr.errorString();
        return out;
    }
    const QJsonArray versions = doc.object().value(QStringLiteral("versions")).toArray();
    out.reserve(versions.size());
    for (const QJsonValue &item : versions) {
        const QJsonObject o = item.toObject();
        GameVersion v;
        v.id = o.value(QStringLiteral("id")).toString();
        v.type = o.value(QStringLiteral("type")).toString();
        v.url = o.value(QStringLiteral("url")).toString();
        v.releaseTime = o.value(QStringLiteral("releaseTime")).toString();
        v.sha1 = o.value(QStringLiteral("sha1")).toString();
        v.size = qint64(o.value(QStringLiteral("size")).toDouble());
        if (!v.id.isEmpty())
            out.append(v);
    }
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
            QString error;
            // 已安装集合/加载器标签/problem 与清单无关,任何时候都要扫一遍
            // (Python 的 _refresh_installed() 也是这么做的)。
            m_instances = scanLocalInstances(gameDirectory());
            QByteArray text = fetchManifestText(&error);
            QVector<GameVersion> versions;
            bool fromManifest = false;
            if (!text.isEmpty()) {
                versions = parseManifest(text, &error);
                fromManifest = !versions.isEmpty();
            }
            if (!fromManifest)
                versions = localInstanceVersions(&error);
            QMetaObject::invokeMethod(
                this,
                [this, versions, error, fromManifest] {
                    onLoaded(versions, error, fromManifest);
                },
                Qt::QueuedConnection);
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }

    // 退路:本地已安装实例(清单拿不到时页面不留白;行绘制/已安装标记与清单模式一致)。
    // TODO(需要主代理加一行 CMake):sxcl_ui_core 拿到 include/ 搜索路径后,这里换成
    // sxcl_instance_scan(),加载器小标签与 problem 提示就会一起出现。
    QVector<GameVersion> localInstanceVersions(QString *error) {
        if (m_instances.isEmpty()) {
            if (error)
                *error = QStringLiteral("本地没有已安装的版本");
            return {};
        }
        QVector<GameVersion> out;
        out.reserve(m_instances.size());
        for (const LocalInstance &inst : m_instances) {
            GameVersion v;
            v.id = inst.id;
            v.type = inst.type;
            v.releaseTime = inst.releaseTime;
            out.append(v);
        }
        return sortDescending(out);
    }
    void onLoaded(const QVector<GameVersion> &versions, const QString &error, bool ok) {
        m_refresh->setEnabled(true);
        if (!ok && versions.isEmpty()) { // versions_page.py:480-492(错误路径)
            m_loading->setVisible(false);
            m_status->setVisible(true);
            m_status->setText(QStringLiteral("加载失败"));
            InfoBar::push(InfoBar::Type::Error, QStringLiteral("加载失败"),
                          QStringLiteral("无法获取版本清单：") + error, this, 5000);
            return;
        }
        m_all = versions;
        m_loading->setVisible(false);
        m_status->setVisible(true);
        applyFilters();
        setStatusText();
        showVersions(m_filtered);
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

    // versions_page.py:412-451:已安装集合(加载器标签 / problem 见文件头的 TODO)
    void refreshInstalled() {
        m_installed.clear();
        m_loaders.clear();
        m_problems.clear();
        for (const LocalInstance &inst : m_instances)
            m_installed.insert(inst.id);
        m_model->setLoaders(m_loaders);
        m_model->setProblems(m_problems);
    }

    // versions_page.py:453-464:状态栏那行"已安装几个 + 各加载器各几个"
    // (加载器汇总要 instance.h 的 loader 列表,见文件头 TODO;现在恒为空串,与
    //  Python 在没有加载器时的行为一致)
    QString loaderSummaryText() const { return QString(); }
    void setStatusText() { // versions_page.py:475-503
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
