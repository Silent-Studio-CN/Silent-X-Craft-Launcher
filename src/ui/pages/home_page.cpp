// home_page.cpp —— 主页(已安装版本列表 + 快速启动)
//
// 逐条移植 Python 版 src/app/pages/home_page.py(行号见每段注释),外壳照抄
// src/app/common/base_page.py(BasePage = qf ScrollArea)。外观的唯一依据:
// docs/05-UI-1to1规格.md + Python 源码;尺寸/颜色/字号在下面每条都能指回出处。
//
// 控件树(Python home_page.py:68-110):
//   HomePage(ScrollArea, objectName=HomePage, setWidgetResizable, 横向滚动条关)
//     └ view(QWidget, background: transparent)
//        └ QVBoxLayout(margins 28,24,28,24; spacing 16; AlignTop)
//           ├ TitleLabel     "主页"                          (28px/600)
//           ├ SubtitleLabel  "Silent X Craft Launcher v0.1.0" (20px/600, #606060/#AAAAAA)
//           ├ CardWidget     游戏目录卡   QHBox(20,12,20,12) spacing 6(布局默认值)
//           ├ CardWidget     联机入口卡   QHBox(20,12,20,12) spacing 12
//           ├ BodyLabel      空态提示(有版本时隐藏)
//           ├ QWidget        版本卡网格   QVBox(margins 0, spacing 8)
//           │   └ CardWidget 版本卡 ×N    定高 56,QHBox(20,0,16,0) spacing 16
//           └ 弹簧
//
// 游戏目录:C 版还没有自己的设置层,先读与 Python 版**同一份**用户配置
// (%APPDATA%/SilentXCraftLauncher/config.json 的 Game.GameDirectory,即
// launcher_config.py:144 的 cfg.gameDirectory),读不到再退回平台默认
// <home>/.minecraft(platform.py:165-167 default_game_directory)。见交付报告:
// 需要主代理给 sxcl_ui_core 接上 sxcl 核心库(settings / paths)后就能换掉。
#include "page_factory.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <algorithm>

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

#include "main_window.h" // 联机入口要跳到 multiplayer 路由(Python 里是 mw.switchTo(page))

namespace sxcl::ui {
namespace {

// Python 未给目录卡布局设间距,取的是布局默认值。参考图实测 = 6
// (路径标签左缘 111 = 卡片 28+1 边框 + 20 边距 + 56 标签宽 + 6)。
// 这里钉死 6,免得跟随平台样式(QWindows11Style)漂移。
constexpr int kDefaultLayoutSpacing = 6;

// base_page.py:57 —— 副标题的亮/暗两套颜色(不是令牌,Python 里就是这两个字面量)
const QColor kSubtitleLight(0x60, 0x60, 0x60);
const QColor kSubtitleDark(0xAA, 0xAA, 0xAA);

// home_page.py:104-106 —— 空态提示(字号/颜色/外边距都是 Python 里的字面量)
const QString kEmptyHintQss = QStringLiteral("color: #888; font-size: 14px; margin: 60px 0;");

// 按钮:qf 的 PushButton 构造里有一句 setFont(self)(button.py:36 → common/font.py
// setFont 默认 14px),libqf 的 PushButton::init() 只套了 QSS、没设字号,于是按钮吃了
// 应用默认字号(9pt≈12px):实测参考图按钮文字墨宽 54(4 字)/28(2 字)、按钮 82x32,
// C 版只出 46.7/24、76x30。这里按 docs/05-UI-1to1规格.md §4「PushButton 高 32,字体 14」
// 把字号钉回 14px;libqf 那边补上 setFont 后(已写进交付报告)这段可以删。
void applyButtonFont(QPushButton *button) {
    QFont font = button->font();
    font.setPixelSize(14);
    font.setWeight(QFont::Normal);
    button->setFont(font);
    button->setFixedHeight(32);
}

// ─────────────────────────────── 游戏目录与版本扫描 ───────────────────────────────

// platform.py:148-152 default_config_directory() —— Windows = %APPDATA%/SilentXCraftLauncher
QString legacyConfigFilePath() {
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/AppData/Roaming");
    return base + QStringLiteral("/SilentXCraftLauncher/config.json");
}

// 读旧版(Python)SXCL 配置里的某一项(launcher_config.py 的分组/键名)。
// 解析失败、文件不存在、类型不对一律返回空串(等价 Python 里"读不到就用默认值")。
QString legacyConfigString(const QString &group, const QString &key) {
    QFile file(legacyConfigFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return QString();
    const QJsonValue value = doc.object().value(group).toObject().value(key);
    return value.isString() ? value.toString() : QString();
}

// home_page.py:77 str(cfg.gameDirectory.value):用户配了就用配置里的原样字符串
// (Python 显示的就是配置里的写法,例如 C:/Users/x/Desktop/.minecraft);
// 没配就退回 platform.py:165-167 的平台默认 <home>/.minecraft(Path 的 str 也是正斜杠)。
QString resolveGameDirectory() {
    // 键名 = launcher_config.py 里的属性名(Game/gameDirectory,首字母小写)
    const QString configured = legacyConfigString(QStringLiteral("Game"),
                                                  QStringLiteral("gameDirectory"));
    if (!configured.isEmpty())
        return configured;
    return QDir::fromNativeSeparators(QDir::homePath()) + QStringLiteral("/.minecraft");
}

// installed.py:38-57 get_installed_versions():versions/ 下"jar + json 都齐全"的目录,
// 名字倒序(sorted(versions, reverse=True))。
QStringList scanInstalledVersions(const QString &gameDir) {
    QStringList versions;
    const QDir versionsDir(gameDir + QStringLiteral("/versions"));
    if (!versionsDir.exists())
        return versions;
    const QFileInfoList entries =
        versionsDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        const QString dir = entry.absoluteFilePath();
        if (QFileInfo::exists(dir + QLatin1Char('/') + name + QStringLiteral(".jar")) &&
            QFileInfo::exists(dir + QLatin1Char('/') + name + QStringLiteral(".json")))
            versions.append(name);
    }
    std::sort(versions.begin(), versions.end(), [](const QString &a, const QString &b) {
        return a > b; // Python: sorted(..., reverse=True)(按 Unicode 码位/码元倒序)
    });
    return versions;
}

// home_page.py:212-230 _get_version_info():能解析出版本 JSON 才算数,然后只看**版本 id**
// 里有没有加载器关键字。
// 注意(照抄 Python 的判定顺序):先判 "forge",所以 neoforge 永远命中 Forge 分支 ——
// 这一条在 Python 里就是死代码,移植后也保持同样的结果,不要"顺手修正"。
QString versionLoaderTag(const QString &gameDir, const QString &versionId) {
    QFile file(gameDir + QStringLiteral("/versions/") + versionId + QLatin1Char('/') +
               versionId + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    // Python: data = json.load(...); if not data: return ""(空对象/坏 JSON 都算没有信息)
    if (error.error != QJsonParseError::NoError || !doc.isObject() || doc.object().isEmpty())
        return QString();
    const QString id = versionId.toLower();
    if (id.contains(QStringLiteral("forge")))
        return QStringLiteral("Forge");
    if (id.contains(QStringLiteral("neoforge")))
        return QStringLiteral("NeoForge");
    if (id.contains(QStringLiteral("fabric")))
        return QStringLiteral("Fabric");
    return QString();
}

// folders.py:49-101 —— 游戏目录探测(Python 的 GameFolder/_count_versions/_inspect)。
// 只是"自动检测"按钮用;正式版应改调核心库 sxcl_paths_detect(见报告)。
struct GameFolder {
    QString path;
    QString label;
    int versions = 0;
    bool hasAssets = false;
    bool hasProfiles = false;
    bool exists = false;
    int score() const { // folders.py:64-74
        if (!exists)
            return -1;
        int value = versions * 10;
        if (hasAssets)
            value += 5;
        if (hasProfiles)
            value += 2;
        return value;
    }
};

int countVersionsIn(const QString &folder) { // folders.py:76-91 _count_versions
    const QDir versionsDir(folder + QStringLiteral("/versions"));
    if (!versionsDir.exists())
        return 0;
    int count = 0;
    const QFileInfoList entries =
        versionsDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        const QString dir = entry.absoluteFilePath();
        if (QFileInfo::exists(dir + QLatin1Char('/') + name + QStringLiteral(".json")) ||
            QFileInfo::exists(dir + QLatin1Char('/') + name + QStringLiteral(".jar"))) {
            ++count;
        } else if (!QDir(dir).entryList(QStringList() << QStringLiteral("*.json"), QDir::Files).isEmpty()) {
            ++count; // 加载器版本常常只有 JSON(jar 靠继承)
        }
    }
    return count;
}

GameFolder inspectFolder(const QString &path, const QString &label) { // folders.py:94-101 _inspect
    GameFolder folder;
    folder.path = path;
    folder.label = label;
    folder.exists = QFileInfo(path).isDir();
    folder.versions = countVersionsIn(path);
    folder.hasAssets = QFileInfo(path + QStringLiteral("/assets")).isDir();
    folder.hasProfiles = QFileInfo(path + QStringLiteral("/launcher_profiles.json")).isFile();
    return folder;
}

// folders.py:112-151 detect_game_folders():候选顺序 —— 启动器目录(便携)/APPDATA/用户目录/
// 桌面(含"桌面"中文名)/当前配置;按路径去重、只留存在的、按 score 倒序(稳定)。
QVector<GameFolder> detectGameFolders(const QString &configuredDir) {
    QVector<GameFolder> candidates;
    const QString programDir = QDir::fromNativeSeparators(QCoreApplication::applicationDirPath());
    for (const QString &name : {QStringLiteral(".minecraft"), QStringLiteral("minecraft"),
                                QStringLiteral("MC")})
        candidates.append(inspectFolder(programDir + QLatin1Char('/') + name,
                                        QStringLiteral("启动器目录（便携）")));
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty())
        candidates.append(inspectFolder(QDir::fromNativeSeparators(appData) +
                                            QStringLiteral("/.minecraft"),
                                        QStringLiteral("官方启动器（APPDATA）")));
    const QString home = QDir::fromNativeSeparators(QDir::homePath());
    candidates.append(inspectFolder(home + QStringLiteral("/.minecraft"), QStringLiteral("用户目录")));
    for (const QString &desktop : {QStringLiteral("Desktop"), QStringLiteral("桌面")})
        candidates.append(inspectFolder(home + QLatin1Char('/') + desktop +
                                            QStringLiteral("/.minecraft"),
                                        QStringLiteral("桌面")));
    if (!configuredDir.isEmpty())
        candidates.append(inspectFolder(configuredDir, QStringLiteral("当前配置")));

    QVector<GameFolder> folders;
    QSet<QString> seen;
    for (const GameFolder &folder : candidates) {
        const QString key = QDir(folder.path).absolutePath().toLower();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        if (folder.exists)
            folders.append(folder);
    }
    std::stable_sort(folders.begin(), folders.end(),
                     [](const GameFolder &a, const GameFolder &b) { return a.score() > b.score(); });
    return folders;
}

// folders.py:154-160 best_game_folder():有版本数的优先,再退回第一个。
const GameFolder *bestGameFolder(const QVector<GameFolder> &folders) {
    const GameFolder *first = nullptr;
    for (const GameFolder &folder : folders) {
        if (!first)
            first = &folder;
        if (folder.versions > 0)
            return &folder;
    }
    return first;
}

QString describeFolder(const GameFolder &folder) { // folders.py:163-164 describe()
    return QStringLiteral("%1（%2，%3 个版本）")
        .arg(folder.path, folder.label)
        .arg(folder.versions);
}

// ─────────────────────────────── 页面本体 ───────────────────────────────

class HomePage : public ScrollArea {
public:
    explicit HomePage(QWidget *parent);

private:
    void buildContent();     // home_page.py:68-110  _build_content
    void refreshInstalled(); // home_page.py:154-157 _refresh_installed
    void renderCards();      // home_page.py:159-177 _render_cards
    QWidget *createVersionCard(const QString &versionId); // home_page.py:179-210
    void checkFs();          // home_page.py:234-241 _check_fs

    void changeDirectory();     // home_page.py:286-295 _change_directory
    void autoDetectDirectory(); // home_page.py:260-284 _auto_detect_folder
    void openDirectory();       // home_page.py:297-314 _open_directory
    void launchVersion(const QString &versionId); // home_page.py:245-258 _launch
    void openMultiplayer();     // home_page.py:138-150 _open_multiplayer

    QString m_gameDir;
    QStringList m_installed;
    QStringList m_lastSnapshot; // Python: self._last_snapshot

    QWidget *m_view = nullptr;
    QVBoxLayout *m_vBox = nullptr;
    BodyLabel *m_dirDisplay = nullptr;
    BodyLabel *m_emptyHint = nullptr;
    QWidget *m_grid = nullptr;
    QVBoxLayout *m_gridLayout = nullptr;
};

HomePage::HomePage(QWidget *parent) : ScrollArea(parent) {
    m_gameDir = resolveGameDirectory();

    // ---- BasePage(src/app/common/base_page.py:41-60)----
    setObjectName(QStringLiteral("HomePage"));                    // :42
    setWidgetResizable(true);                                     // :43
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);         // :44

    // 页面底色 = 令牌 bg(#202020)。依据 docs/05-UI-1to1规格.md §10.2(内容区设计值 #202020)
    // 与 §11.2(抓参考图时把页面底色钉成 token(bg),否则 qf StackedWidget 的半透明白
    // rgba(255,255,255,0.0314) 会让内容区变 #272727、卡片变 #323232)。等价于 Python 抓图脚本的
    // page.setStyleSheet("QWidget { background: %s }" % token("bg"))。选择器只命中本页,
    // 子控件各有自己的样式表(标签走 FluentLabelBase、卡片自绘),外观不受影响。
    // 外壳已把 StackedWidget 的底色做成透明(内容底 = 窗口底 #202020),这一行现在是"保险",
    // 不留也无碍 —— 删掉它就与 Python 的 BasePage(透明)逐字一致。
    setStyleSheet(QStringLiteral("QScrollArea { background: %1; }")
                      .arg(FluentTheme::instance().tokens().bg.name()));
    // Python 的 BasePage 继承 qf 的 ScrollArea,没有改 frameShape → 默认 StyledPanel 的 1px 边框,
    // 视口因此落在页面 (1,1)(probe 实测 viewport=(1,1,1050,700));libqf 的 ScrollArea::initArea()
    // 把 frameShape 设成 NoFrame,内容会比参考图左上各少 1px。这里按 Python 的行为恢复 1px 边框。
    setFrameShape(QFrame::StyledPanel);
    setLineWidth(1);

    m_view = new QWidget(this);                                   // :46
    m_view->setStyleSheet(QStringLiteral("background: transparent;")); // :47
    setWidget(m_view);                                            // :48

    m_vBox = new QVBoxLayout(m_view);                             // :50
    m_vBox->setContentsMargins(28, 24, 28, 24);                   // :51
    m_vBox->setSpacing(16);                                       // :52
    m_vBox->setAlignment(Qt::AlignTop);                           // :53

    // home_page.py:55-56 title="主页" / subtitle=f"{APP_NAME} v{APP_VERSION}"
    // (src/core/constants.py:APP_NAME="Silent X Craft Launcher", APP_VERSION="0.1.0")
    auto *title = new TitleLabel(QStringLiteral("主页"), m_view);
    auto *subtitle =
        new SubtitleLabel(QStringLiteral("Silent X Craft Launcher v0.1.0"), m_view);
    subtitle->setTextColor(kSubtitleLight, kSubtitleDark);        // base_page.py:57
    m_vBox->addWidget(title);                                     // :59
    m_vBox->addWidget(subtitle);                                  // :60

    buildContent();
    refreshInstalled();

    // home_page.py:62-66:每 8 秒检查一次文件变化(用户手动增删版本后自动刷新)
    auto *watch = new QTimer(this);
    watch->setInterval(8000);
    connect(watch, &QTimer::timeout, this, [this] { checkFs(); });
    watch->start();
}

void HomePage::buildContent() {
    const ThemeTokens &tokens = FluentTheme::instance().tokens();

    // ── 游戏目录卡(home_page.py:69-93)──
    auto *dirCard = new CardWidget(m_view);                       // :70
    auto *dirLay = new QHBoxLayout(dirCard);                      // :71
    dirLay->setContentsMargins(20, 12, 20, 12);                   // :72
    dirLay->setSpacing(kDefaultLayoutSpacing);                    // :73-92 未设 → 默认 6

    auto *dirLabel = new BodyLabel(QStringLiteral("游戏目录"), dirCard); // :74
    dirLay->addWidget(dirLabel);                                  // :75

    m_dirDisplay = new BodyLabel(m_gameDir, dirCard);             // :77
    m_dirDisplay->setTextColor(tokens.textTertiary);              // :78(token("text_tertiary"))
    dirLay->addWidget(m_dirDisplay, 1);                           // :79

    auto *changeBtn = new PushButton(QStringLiteral("更改"), dirCard);      // :81
    applyButtonFont(changeBtn);
    connect(changeBtn, &QAbstractButton::clicked, this, [this] { changeDirectory(); });
    dirLay->addWidget(changeBtn);                                 // :83

    auto *detectBtn = new PushButton(QStringLiteral("自动检测"), dirCard);  // :85
    applyButtonFont(detectBtn);
    connect(detectBtn, &QAbstractButton::clicked, this, [this] { autoDetectDirectory(); });
    dirLay->addWidget(detectBtn);                                 // :87

    auto *openBtn = new PushButton(QStringLiteral("打开"), dirCard);        // :89
    applyButtonFont(openBtn);
    connect(openBtn, &QAbstractButton::clicked, this, [this] { openDirectory(); });
    dirLay->addWidget(openBtn);                                   // :91

    m_vBox->addWidget(dirCard);                                   // :93

    // ── 联机入口卡(home_page.py:112-136)──
    auto *mpCard = new CardWidget(m_view);                        // :114
    auto *mpLay = new QHBoxLayout(mpCard);                        // :115
    mpLay->setContentsMargins(20, 12, 20, 12);                    // :116
    mpLay->setSpacing(12);                                        // :117

    auto *mpIcon = new QLabel(mpCard);                            // :119
    mpIcon->setPixmap(fluent::icon(QStringLiteral("Globe"), FluentTheme::instance().isDark())
                          .pixmap(28, 28));                       // :120 FIF.GLOBE.icon().pixmap(28,28)
    mpLay->addWidget(mpIcon);                                     // :121

    auto *textBox = new QVBoxLayout();                            // :123
    textBox->setSpacing(2);                                       // :124
    textBox->addWidget(new StrongBodyLabel(QStringLiteral("联机 · 和朋友一起玩"), mpCard)); // :125
    auto *mpDesc = new BodyLabel(
        QStringLiteral("房间码加入 / P2P 打洞 / 中继兜底（开发中，先留入口）"), mpCard); // :126
    mpDesc->setTextColor(tokens.textTertiary);                    // :127
    textBox->addWidget(mpDesc);                                   // :128
    mpLay->addLayout(textBox, 1);                                 // :129

    auto *lookBtn = new PushButton(QStringLiteral("看看方案"), mpCard);     // :131
    applyButtonFont(lookBtn);
    connect(lookBtn, &QAbstractButton::clicked, this, [this] { openMultiplayer(); });
    mpLay->addWidget(lookBtn);                                    // :133
    mpCard->setCursor(Qt::PointingHandCursor);                    // :135
    m_vBox->addWidget(mpCard);                                    // :96

    // ── 空态提示(home_page.py:104-108)──
    m_emptyHint = new BodyLabel(QStringLiteral("暂无已安装的版本\n前往「版本」页下载"), m_view);
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setStyleSheet(kEmptyHintQss);
    m_vBox->addWidget(m_emptyHint);                               // :108

    // ── 版本卡网格(home_page.py:99-102,109-110)──
    m_grid = new QWidget(m_view);                                 // :99
    m_gridLayout = new QVBoxLayout(m_grid);                       // :100
    m_gridLayout->setContentsMargins(0, 0, 0, 0);                 // :101
    m_gridLayout->setSpacing(8);                                  // :102
    m_vBox->addWidget(m_grid);                                    // :109
    m_vBox->addStretch(1);                                        // :110
}

void HomePage::refreshInstalled() {
    m_installed = scanInstalledVersions(m_gameDir);               // :156
    m_lastSnapshot = m_installed;
    renderCards();                                                // :157
}

void HomePage::renderCards() {
    // 清掉旧卡片(home_page.py:161-165)
    while (QLayoutItem *item = m_gridLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    if (m_installed.isEmpty()) {                                  // :167-170
        m_emptyHint->setVisible(true);
        m_grid->setVisible(false);
        return;
    }

    m_emptyHint->setVisible(false);                               // :172-173
    m_grid->setVisible(true);
    for (const QString &versionId : m_installed)                  // :175-177
        m_gridLayout->addWidget(createVersionCard(versionId));
}

QWidget *HomePage::createVersionCard(const QString &versionId) {
    auto *card = new CardWidget(m_grid);                          // :180
    card->setFixedHeight(56);                                     // :181
    card->setCursor(Qt::PointingHandCursor);                      // :182

    auto *lay = new QHBoxLayout(card);                            // :184
    lay->setContentsMargins(20, 0, 16, 0);                        // :185
    lay->setSpacing(16);                                          // :186

    auto *name = new BodyLabel(versionId, card);                  // :189
    name->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600;")); // :190
    {
        // 同一件事再用 QFont 落地一遍:libqf 的 FluentLabelBase 在主题切换时会重套
        // 自己的 QSS,显式 setFont 保证 15px/DemiBold 不会丢(参考图实测 15px/600)。
        QFont font = name->font();
        font.setPixelSize(15);
        font.setWeight(QFont::DemiBold);
        name->setFont(font);
    }
    lay->addWidget(name);                                         // :191

    const QString info = versionLoaderTag(m_gameDir, versionId);  // :194
    if (!info.isEmpty()) {                                        // :195
        auto *detail = new BodyLabel(info, card);
        // 调用顺序照抄 Python(:197-198):先 setTextColor(tertiary) 再 setStyleSheet
        // ("font-size: 12px;")—— 后者会顶掉颜色规则,所以参考图里这行是**白色 12px**。
        detail->setTextColor(FluentTheme::instance().tokens().textTertiary);
        detail->setStyleSheet(QStringLiteral("font-size: 12px;"));
        QFont font = detail->font();
        font.setPixelSize(12);
        font.setWeight(QFont::Normal);
        detail->setFont(font);
        lay->addWidget(detail);                                   // :199
    }

    lay->addStretch();                                            // :201

    auto *launchBtn = new PrimaryPushButton(QStringLiteral("启动"), card); // :204
    applyButtonFont(launchBtn);
    launchBtn->setFixedSize(80, 32);                              // :205
    connect(launchBtn, &QAbstractButton::clicked, this,
            [this, versionId] { launchVersion(versionId); });     // :206
    lay->addWidget(launchBtn);                                    // :207
    return card;                                                  // :210
}

void HomePage::checkFs() { // home_page.py:234-241
    const QStringList current = scanInstalledVersions(m_gameDir);
    const QSet<QString> currentSet(current.begin(), current.end());
    const QSet<QString> lastSet(m_lastSnapshot.begin(), m_lastSnapshot.end());
    if (currentSet != lastSet) {
        m_lastSnapshot = current;
        m_installed = current;
        renderCards();
    }
}

void HomePage::changeDirectory() { // home_page.py:286-295
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择 .minecraft 目录"), m_gameDir);
    if (dir.isEmpty())
        return;
    // Python 这里还会 qconfig.set + save_config() 落盘;C 版还没有设置层(见报告),
    // 先只改内存里的当前目录 —— 行为可见部分一致,重启不保留。
    m_gameDir = QDir::fromNativeSeparators(dir);
    m_dirDisplay->setText(m_gameDir);
    refreshInstalled();
}

void HomePage::autoDetectDirectory() { // home_page.py:260-284
    const QVector<GameFolder> folders = detectGameFolders(m_gameDir);
    const GameFolder *best = bestGameFolder(folders);
    if (!best) {
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("没找到游戏目录"),
                      QStringLiteral("点「更改」手动选一个 .minecraft 目录"), window(), 5000);
        return;
    }
    // Python: qconfig.set(cfg.gameDirectory, str(best.path)) + save_config() (见上条说明)
    m_gameDir = best->path;
    m_dirDisplay->setText(m_gameDir);
    refreshInstalled();

    QString content = describeFolder(*best);
    QStringList others;
    for (const GameFolder &folder : folders) {
        if (folder.path != best->path)
            others.append(QStringLiteral("%1(%2 个版本)").arg(folder.path).arg(folder.versions));
    }
    if (!others.isEmpty())
        content += QStringLiteral("；还发现：") + others.mid(0, 3).join(QStringLiteral("、"));
    InfoBar::push(InfoBar::Type::Success, QStringLiteral("已切换游戏目录"), content, window(), 6000);
}

void HomePage::openDirectory() { // home_page.py:297-314
    // Windows 用 ShellExecute 打开资源管理器(Qt 的等价物),macOS/Linux 也由 Qt 分派
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_gameDir));
}

void HomePage::launchVersion(const QString &versionId) { // home_page.py:245-258
    Q_UNUSED(versionId);
    // Python:if not cfg.javaPath.value -> 警告"请先在设置中选择 Java 运行时";
    //        否则 mw.switch_to_launch(v),没有这个钩子就报"启动器未初始化"。
    // C 版主窗口还没有启动页(switch_to_launch 的对应物),设置层也还没接上,
    // 所以这里走到 Python 的兜底分支;等主代理加上启动页后把这里换成它即可。
    const QString javaPath = legacyConfigString(QStringLiteral("Game"), QStringLiteral("javaPath"));
    if (javaPath.isEmpty()) {
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("未选择 Java"),
                      QStringLiteral("请先在设置中选择 Java 运行时"), window(), 4000);
        return;
    }
    InfoBar::push(InfoBar::Type::Error, QStringLiteral("错误"),
                  QStringLiteral("启动器未初始化"), window(), 3000);
}

void HomePage::openMultiplayer() { // home_page.py:138-150
    // Python: mw = self.window(); page = getattr(mw, "multiplayer_page", None) -> switchTo
    if (auto *mw = qobject_cast<MainWindow *>(window())) {
        mw->switchToRoute(QStringLiteral("multiplayer"));
        return;
    }
    InfoBar::push(InfoBar::Type::Info, QStringLiteral("联机"),
                  QStringLiteral("联机页即将上线"), window(), 2500);
}

} // namespace

QWidget *createHomePage(QWidget *parent) { return new HomePage(parent); }

} // namespace sxcl::ui
