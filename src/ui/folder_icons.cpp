/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 文件夹自定义图标的实现（声明与理由见 folder_icons.h）。

#include "folder_icons.h"

#include "icon_registry.h"
#include "pages/game_folders.h"   /* ownSettingsFilePath（设置文件那条唯一权威） */
#include "pages/page_shell.h"     /* pageTokenColor / pageTokenText */
#include "sxcl_icons.h"

#include "fluent/fluent_cards.h"
#include "fluent/fluent_controls.h"
#include "fluent/fluent_labels.h"
#include "fluent/fluent_layouts.h"

#include <QCursor>
#include <QDir>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QVBoxLayout>
#include <QWidget>

#include "sxcl/settings.h"

namespace sxcl::ui {
namespace {

constexpr const char *kBlockPrefix = "block:";
constexpr const char *kPclPrefix = "pcl:";
constexpr const char *kSettingsKey = "game.folder_icons";

// 方块图的人话名（SxclIcons 的 kind -> 中文）。
// 名字照 assets/icons/blocks/NOTICE.md 的对应关系（那是 PCL ModMinecraft.vb:785-809 的口径）。
struct BlockTitle {
    const char *kind;
    const char *title;
};
const BlockTitle kBlockTitles[] = {
    {"vanilla", "草方块"},        {"snapshot", "命令方块"},   {"old", "圆石"},
    {"forge", "铁砧"},            {"neoforge", "狐狸"},       {"fabric", "布料"},
    {"quilt", "拼布"},            {"optifine", "土径"},       {"liteloader", "鸡蛋"},
    {"error", "红石块"},          {"gold", "金块"},           {"optifabric", "白布料"},
    {"lamp_on", "红石灯（亮）"},  {"lamp_off", "红石灯（灭）"},
};

QString blockTitle(const QString &kind) {
    for (const BlockTitle &row : kBlockTitles) {
        if (kind == QLatin1String(row.kind))
            return QString::fromUtf8(row.title);
    }
    return kind;
}

QString settingsText() {
    const QByteArray path = ownSettingsFilePath().toUtf8();
    sxcl_settings *st = sxcl_settings_open(path.constData());
    if (!st)
        return QString();
    const char *raw = sxcl_settings_get(st, kSettingsKey, "");
    const QString out = QString::fromUtf8(raw ? raw : "");
    sxcl_settings_free(st);
    return out;
}

void writeSettingsText(const QString &value) {
    const QByteArray path = ownSettingsFilePath().toUtf8();
    sxcl_settings *st = sxcl_settings_open(path.constData());
    if (!st)
        return;
    sxcl_settings_set(st, kSettingsKey, value.toUtf8().constData());
    sxcl_settings_save(st, path.constData()); // **必须存盘**(用户要的就是"关掉重开还在")
    sxcl_settings_free(st);
}

// 路径比较：Windows 大小写不敏感、正/反斜杠等价；两边都先 cleanPath。
QString normPath(const QString &path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}
bool samePath(const QString &a, const QString &b) {
#if defined(Q_OS_WIN)
    return normPath(a).compare(normPath(b), Qt::CaseInsensitive) == 0;
#else
    return normPath(a) == normPath(b);
#endif
}

// "id=路径|id=路径" <-> QVector<QPair<QString,QString>>
struct IconRow {
    QString id;
    QString path;
};
QVector<IconRow> parseRows(const QString &text) {
    QVector<IconRow> rows;
    const QStringList parts = text.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const int eq = part.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        IconRow row;
        row.id = part.left(eq);
        row.path = part.mid(eq + 1);
        if (!row.path.isEmpty())
            rows.push_back(row);
    }
    return rows;
}
QString joinRows(const QVector<IconRow> &rows) {
    QStringList parts;
    for (const IconRow &row : rows)
        parts << row.id + QLatin1Char('=') + row.path;
    return parts.join(QLatin1Char('|'));
}

} // namespace

QVector<FolderIconOption> folderIconCatalog() {
    QVector<FolderIconOption> out;
    // 方块（14 种）——顺序跟随 SxclIcons 的 kFiles，图标含义见 NOTICE.md
    for (const QString &kind : SxclIcons::kinds()) {
        FolderIconOption opt;
        opt.id = QString::fromLatin1(kBlockPrefix) + kind;
        opt.title = blockTitle(kind);
        opt.group = QStringLiteral("方块");
        out.push_back(opt);
    }
    // 界面（PCL 语义图，13 个）——key/title 直接用 IconRegistry 那份，不另立一张表
    for (int i = 0; i < int(IconRegistry::Count); ++i) {
        const auto semantic = static_cast<IconRegistry::Semantic>(i);
        FolderIconOption opt;
        opt.id = QString::fromLatin1(kPclPrefix) + IconRegistry::key(semantic);
        opt.title = IconRegistry::title(semantic);
        opt.group = QStringLiteral("界面");
        out.push_back(opt);
    }
    return out;
}

bool folderIconIdValid(const QString &id) {
    for (const FolderIconOption &opt : folderIconCatalog()) {
        if (opt.id == id)
            return true;
    }
    return false;
}

QIcon folderIconForId(const QString &id, int size) {
    // 先保证方块资产目录解析过（幂等）：没解析过时 hasAsset() 一律 false，
    // 连 "block:vanilla" 都会掉到兜底分支去（曾因此返回空图标）。
    (void)SxclIcons::instance().resolveBlockDir();
    if (id.startsWith(QLatin1String(kBlockPrefix))) {
        const QString kind = id.mid(int(qstrlen(kBlockPrefix)));
        if (SxclIcons::instance().hasAsset(kind))
            return SxclIcons::instance().blockIcon(kind, size);
    } else if (id.startsWith(QLatin1String(kPclPrefix))) {
        const QString key = id.mid(int(qstrlen(kPclPrefix)));
        // 图标目录可能还没被谁加载过(比如单测里)—— 现取一次,免得界面上是个洞
        if (!IconRegistry::instance().loaded() && IconRegistry::instance().resolveIconDir())
            (void)IconRegistry::instance().load();
        for (int i = 0; i < int(IconRegistry::Count); ++i) {
            const auto semantic = static_cast<IconRegistry::Semantic>(i);
            if (IconRegistry::key(semantic) == key) {
                const QIcon icon = IconRegistry::instance().themedIcon(semantic);
                if (!icon.isNull())
                    return icon;
                break;
            }
        }
    }
    // 认不出来（或资产缺失）：一律回默认草方块 —— 用户设置里存了旧版本删掉的图标、
    // 或者机器上缺某个 png 时，界面上都不会留一个洞。
    return SxclIcons::instance().blockIcon(QStringLiteral("vanilla"), size);
}

QString defaultFolderIconId() {
    return QString::fromLatin1(kBlockPrefix) + QStringLiteral("vanilla");
}

QString folderIconId(const QString &folderPath) {
    const QVector<IconRow> rows = parseRows(settingsText());
    for (const IconRow &row : rows) {
        if (samePath(row.path, folderPath) && folderIconIdValid(row.id))
            return row.id;
    }
    return defaultFolderIconId();
}

void setFolderIconId(const QString &folderPath, const QString &id) {
    const QString wanted = folderIconIdValid(id) ? id : defaultFolderIconId();
    QVector<IconRow> rows = parseRows(settingsText());
    QVector<IconRow> kept;
    for (const IconRow &row : rows) {
        if (!samePath(row.path, folderPath))
            kept.push_back(row);
    }
    IconRow row;
    row.id = wanted;
    row.path = normPath(folderPath);
    kept.push_back(row);
    writeSettingsText(joinRows(kept));
}

QWidget *createFolderIconPicker(const QString &folderPath, const QString &folderTitle,
                                const QString &currentId,
                                const std::function<void(const QString &)> &onPick,
                                QWidget *parent) {
    auto *card = new CardWidget(parent);
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(10);

    auto *head = new StrongBodyLabel(QStringLiteral("文件夹图标"), card);
    head->setStyleSheet(QStringLiteral("color: %1; font-size: 15px;").arg(pageTokenText("accent")));
    lay->addWidget(head);

    auto *hint = new BodyLabel(
        QStringLiteral("给「%1」挑一个图标 —— 点一下立刻生效，状态会记住。\n%2")
            .arg(folderTitle.isEmpty() ? folderPath : folderTitle,
                 QDir::toNativeSeparators(folderPath)),
        card);
    hint->setWordWrap(true);
    const QColor secondary = pageTokenColor("textSecondary");
    hint->setTextColor(secondary, secondary);
    lay->addWidget(hint);

    const QString chosen = folderIconIdValid(currentId) ? currentId : defaultFolderIconId();
    const QStringList groups = {QStringLiteral("方块"), QStringLiteral("界面")};
    for (const QString &group : groups) {
        auto *groupLabel = new BodyLabel(group, card);
        groupLabel->setTextColor(secondary, secondary);
        lay->addWidget(groupLabel);

        /* 网格布局(每行 8 个)。
         * 以前这里是 FlowLayout:它 hasHeightForWidth,而 QWidget 不会主动把 heightForWidth
         * 传给父布局 —— 于是那个子容器的高度算出来是 0,**所有图标全叠在同一个位置**
         * (用户 2026-09-22 晚:「版本选择的文件夹LOGO切换堆在一起」)。
         * 弹窗本来就是"内容多大就多大"的固定小窗,用网格把列数钉死最稳、也最好看。 */
        auto *grid = new QWidget(card);
        grid->setStyleSheet(QStringLiteral("background: transparent;"));
        auto *flow = new QGridLayout(grid);
        flow->setContentsMargins(0, 0, 0, 0);
        flow->setHorizontalSpacing(6);
        flow->setVerticalSpacing(6);
        int cell = 0;
        for (const FolderIconOption &opt : folderIconCatalog()) {
            if (opt.group != group)
                continue;
            auto *btn = new TransparentToolButton(card);
            btn->setIcon(folderIconForId(opt.id, 28));
            btn->setIconSize(QSize(28, 28));
            btn->setFixedSize(46, 46);
            btn->setToolTip(opt.title);
            btn->setCursor(Qt::PointingHandCursor);
            if (opt.id == chosen) {
                btn->setStyleSheet(QStringLiteral("QToolButton { border: 2px solid %1;"
                                                  " border-radius: 8px; }")
                                       .arg(pageTokenText("accent")));
            }
            const QString picked = opt.id;
            QObject::connect(btn, &QToolButton::clicked, card, [onPick, picked] {
                if (onPick)
                    onPick(picked);
            });
            flow->addWidget(btn, cell / 8, cell % 8, Qt::AlignLeft | Qt::AlignTop);
            ++cell;
        }
        lay->addWidget(grid);
    }

    auto *row = new QHBoxLayout();
    row->addStretch(1);
    auto *reset = new PushButton(QStringLiteral("恢复默认（草方块）"), card);
    QObject::connect(reset, &QAbstractButton::clicked, card, [onPick] {
        if (onPick)
            onPick(defaultFolderIconId());
    });
    row->addWidget(reset, 0, Qt::AlignVCenter);
    lay->addLayout(row);
    return card;
}

void showFolderIconPopup(QWidget *anchor, const QString &folderPath, const QString &folderTitle,
                         const std::function<void(const QString &)> &onPick) {
    auto *popup = new QWidget(anchor, Qt::Popup);
    popup->setObjectName(QStringLiteral("sxclIconPopup"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    /* 弹层是**顶层窗**(Qt::Popup):主窗口那套 QSS 里的卡片底色不一定作用到它身上,
     * 不自己钉一份的话整块弹窗就是 Qt 默认的浅色底,深色主题下看着像块白板
     * (真机截图:675x626 里 93% 是 #f8f8f8,图标在浅底上几乎看不见)。
     * 这里按主题令牌自己铺底 + 描边 + 圆角,与别的卡片同一种观感。 */
    popup->setStyleSheet(
        QStringLiteral("QWidget#sxclIconPopup { background: %1; border: 1px solid %2;"
                       " border-radius: 10px; }")
            .arg(pageTokenText("card"), pageTokenText("border")));
    auto *lay = new QVBoxLayout(popup);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    QWidget *card = createFolderIconPicker(
        folderPath, folderTitle, folderIconId(folderPath),
        [popup, onPick](const QString &id) {
            popup->close(); // 先关再回调（回调里会重建侧栏，别让弹窗卡在中间）
            if (onPick)
                onPick(id);
        },
        popup);
    lay->addWidget(card);
    popup->adjustSize();

    // 摆在锚点下方；贴到屏幕右边就向左挪，贴到底部就往上翻
    QPoint pos = anchor ? anchor->mapToGlobal(QPoint(0, anchor->height() + 4)) : QCursor::pos();
    if (QScreen *screen = QGuiApplication::screenAt(pos)) {
        const QRect area = screen->availableGeometry();
        const QSize size = popup->size();
        if (pos.x() + size.width() > area.right())
            pos.setX(qMax(area.left(), area.right() - size.width()));
        if (pos.y() + size.height() > area.bottom())
            pos.setY(qMax(area.top(), (anchor ? anchor->mapToGlobal(QPoint(0, 0)).y() : pos.y()) - size.height() - 4));
    }
    popup->move(pos);
    popup->show();
}

} // namespace sxcl::ui
