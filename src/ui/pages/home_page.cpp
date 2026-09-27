/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 主页 —— **2026-09-22 界面重构**(用户口述,规格见 docs/25 §2)。
//
// 用户原话:「原"主页"替换当前游戏版本名(不是版本号,是可以自定义的版本名……区分的方法是
// versions 下各个文件夹名字)。然后左边栏右侧的大部分空间分为正版启动和离线。我们支持已经
// 登陆正版的玩家以离线登录。记得离线提供 ID 输入框,也加入状态保留哈。然后,原来的所有安装
// 版本都堆在主页整体砍掉。改成版本选择页。」
//
// 所以这一页现在是:
//   ① 当前版本卡 —— 大号显示**文件夹名**(可点/「更换」→ 版本选择页 select)
//   ② 启动区左右两半 —— 左:正版启动(登录/启动) 右:离线启动(ID 输入框 + 启动)
//   ③ 游戏目录卡(改名"当前文件夹",显示文件夹自己的名字 + 完整路径)
//   ④ 联机入口卡(Python 版原有,保留)
// 原来的版本卡网格(renderCards/createVersionCard)整块搬去了 versions_select_page.cpp。
//
// 状态保留(写我们自己的设置文件,见 game_folders.h):game.selected_version /
// launch.offline_name / game.default_dir / game.known_dirs。

#include "page_factory.h"
#include "elided_label.h" // 单行标签"装不下就省略号"(左栏被窗口挤窄时要用)
#include "game_folders.h"

#include "dialogs/account.h"
#include "dialogs/auth_dialog.h" // 未登录/凭据过期 -> 直接开登录窗（而不是偷偷用离线跑起来）
#include "fluent_theme.h"
#include "icon_select_button.h" // 正版/离线两枚 LOGO 的"图标当选项"控件(选中态 = 图标下一条 accent)
#include "libqf.h"
#include "main_window.h"
#include "theme_bridge.h"
#include "workers/ui_error.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "fluent/fluent_cards.h"
#include "fluent/fluent_controls.h"
#include "fluent/fluent_input.h"
#include "fluent/fluent_labels.h"
#include "fluent/fluent_scroll.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdio>

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace sxcl::ui {
namespace {

/* 右栏(账号)固定宽度:300 逻辑像素 —— 用户要的是新版那个版面,新版就是 300。
 * 固定宽度意味着窗口变窄时被挤的是左栏,账号卡不会被压成一条。 */
constexpr int kRightColumnWidth = 300;

} // namespace

class HomePage : public ScrollArea {
public:
    explicit HomePage(QWidget *parent);

protected:
    void showEvent(QShowEvent *event) override; // 从版本选择页/设置页回来要刷新(版本名可能变了)

private:
    void buildContent();
    // 四张卡各建各的(版面 = 左右两栏;说明见 buildContent 开头)
    void buildHeroCard(const ThemeTokens &tokens);
    void buildFolderCard(const ThemeTokens &tokens);
    void buildMultiplayerCard(const ThemeTokens &tokens);
    void buildPlayerCard(const ThemeTokens &tokens);
    void refresh();              // 重读"当前版本"与账户状态,重画卡上的字
    QString currentVersion() const;
    void setEdition(bool premium); // 正版 / 离线:两枚 LOGO 的选中态 + 账号卡换页 + 启动落到哪一路
    void changeVersion();        // → 路由 select(版本选择页)
    void changeDirectory();
    void autoDetectDirectory();
    void openDirectory();
    void launch();               // 主页那一枚「启动游戏」:按当前形态落到正版/离线两路之一
    void launchWithAccount();
    void launchOffline();
    void launchFallback(); // window() 不是 MainWindow 时的统一错误出口
    void openMultiplayer();

    QString m_gameDir;
    QWidget *m_view = nullptr;
    QVBoxLayout *m_vBox = nullptr;      // 整页:只有"左右两栏"这一行
    QHBoxLayout *m_columns = nullptr;
    QWidget *m_leftHost = nullptr;      // 左栏宿主(游戏)
    QWidget *m_rightHost = nullptr;     // 右栏宿主(账号,固定 300)
    QVBoxLayout *m_leftCol = nullptr;
    QVBoxLayout *m_rightCol = nullptr;

    BodyLabel *m_versionName = nullptr;   // 大号:当前版本(文件夹名)
    BodyLabel *m_versionDetail = nullptr; // 小字:加载器 + 有没有自己的 jar
    PrimaryPushButton *m_launchButton = nullptr; // 主页唯一的一枚启动
    BodyLabel *m_accountState = nullptr;  // 账号卡的状态行
    PushButton *m_accountButton = nullptr;
    QLineEdit *m_offlineEdit = nullptr;   // 离线 ID(状态保留)
    QStackedWidget *m_accountPane = nullptr; // 正版页 / 离线页 二选一
    IconSelectButton *m_premiumLogo = nullptr; // microsoft.svg(四色,不染)
    IconSelectButton *m_offlineLogo = nullptr; // disconnected.svg(线稿,按令牌染)
    int m_panePremium = -1;
    int m_paneOffline = -1;
    bool m_premium = false;               // 当前形态;默认离线(用户点名)
    BodyLabel *m_dirName = nullptr;       // 当前文件夹的名字
    BodyLabel *m_dirDisplay = nullptr;    // 完整路径
};

HomePage::HomePage(QWidget *parent) : ScrollArea(parent) {
    m_gameDir = resolveGameDirectory();

    setObjectName(QStringLiteral("HomePage"));
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
    m_vBox->setContentsMargins(24, 20, 24, 24);
    m_vBox->setSpacing(16);
    m_vBox->setAlignment(Qt::AlignTop);

    /* 原来"主页 / Silent X Craft Launcher v0.2.0"那两行大标题按新版版面**删掉**:
     * 路由名已经在左边导航里写着,版本号属于关于页;这一页第一眼该是"当前版本 + 启动"。 */
    buildContent();
    refresh();

    // home_page.py:62-66:每 8 秒检查一次文件变化(用户手动增删版本/换目录后自动刷新)
    auto *watch = new QTimer(this);
    watch->setInterval(8000);
    connect(watch, &QTimer::timeout, this, [this] { refresh(); });
    watch->start();
}

void HomePage::showEvent(QShowEvent *event) {
    ScrollArea::showEvent(event);
    refresh(); // 从"版本选择页"选完回来,这一页必须立刻显示新选中的版本
}

void HomePage::buildContent() {
    /* 版面 = **左右两栏**(用户 2026-09-26 点名:「能不能把主页的布局改成原来新版的那个布局,
     * 但不是要新版啊,只是要它的布局」)。新版主页就是这一版:左栏游戏(拉伸)/ 右栏账号(**固定 300**)。
     * 控件仍然是老界面这一套(CardWidget / BodyLabel / PrimaryPushButton)——
     * ui2 那几个 HeroCard / PlayerCard / Accordion 类一枚都没搬回来(用户没要新版)。 */
    m_columns = new QHBoxLayout();
    m_columns->setContentsMargins(0, 0, 0, 0);
    m_columns->setSpacing(16);
    m_vBox->addLayout(m_columns);

    m_leftHost = new QWidget(m_view);
    m_leftHost->setObjectName(QStringLiteral("homeGameColumn"));
    m_leftHost->setStyleSheet(QStringLiteral("background: transparent;"));
    /* 左栏再被挤也要留 320:窗口最小尺寸 900x600 时(主窗口 kMinimumWidth/kMinimumHeight)
     * 内容宽 = 851 - 24*2 - 300 - 16 = 487,左栏在这个下限之上都能摆下。 */
    m_leftHost->setMinimumWidth(320);
    m_leftCol = new QVBoxLayout(m_leftHost);
    m_leftCol->setContentsMargins(0, 0, 0, 0);
    m_leftCol->setSpacing(16);
    m_columns->addWidget(m_leftHost, 1);

    m_rightHost = new QWidget(m_view);
    m_rightHost->setObjectName(QStringLiteral("homeAccountColumn"));
    m_rightHost->setStyleSheet(QStringLiteral("background: transparent;"));
    m_rightHost->setFixedWidth(kRightColumnWidth); // 300:新版同一个数
    m_rightCol = new QVBoxLayout(m_rightHost);
    m_rightCol->setContentsMargins(0, 0, 0, 0);
    m_rightCol->setSpacing(16);
    m_columns->addWidget(m_rightHost, 0);

    const ThemeTokens &tokens = FluentTheme::instance().tokens();
    buildHeroCard(tokens);
    buildFolderCard(tokens);
    buildMultiplayerCard(tokens);
    buildPlayerCard(tokens);
    m_leftCol->addStretch(1);
    m_rightCol->addStretch(1);
}

// ── 左栏 ①:当前版本大卡(版本名 + 元信息 + 启动 + 更换版本)──
void HomePage::buildHeroCard(const ThemeTokens &tokens) {
    auto *card = new CardWidget(m_leftHost);
    card->setObjectName(QStringLiteral("homeHeroCard"));
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(24, 20, 24, 20);
    lay->setSpacing(10);

    auto *caption = new BodyLabel(QStringLiteral("当前版本"), card);
    caption->setTextColor(tokens.textTertiary);
    lay->addWidget(caption);

    m_versionName = new BodyLabel(QStringLiteral("（还没有已安装的版本）"), card);
    m_versionName->setObjectName(QStringLiteral("homeVersionName"));
    /* 26px 的版本名是这一页最大的一行字:窗口压到最小尺寸时它也必须能省略,
     * 不能撑得整页横向溢出(否则右栏会被推出窗口外边 —— 2026-09-26 实测过)。 */
    makeLabelElide(m_versionName, 120);
    {
        QFont font = m_versionName->font();
        font.setPixelSize(26);
        font.setWeight(QFont::DemiBold);
        m_versionName->setFont(font);
    }
    lay->addWidget(m_versionName);

    m_versionDetail = new BodyLabel(QString(), card);
    m_versionDetail->setObjectName(QStringLiteral("homeVersionDetail"));
    m_versionDetail->setWordWrap(true);
    m_versionDetail->setTextColor(tokens.textTertiary);
    lay->addWidget(m_versionDetail);

    auto *row = new QHBoxLayout();
    row->setSpacing(12);
    /* 主页**只有这一枚启动**(新版版面就是这样):落到哪一路由当前形态决定 ——
     * 见 launch()。原来"正版卡一个启动 + 离线卡一个启动"的两枚按钮不再各摆一个。 */
    m_launchButton = new PrimaryPushButton(QStringLiteral("启动游戏"), card);
    m_launchButton->setObjectName(QStringLiteral("homeLaunchButton"));
    applyButtonFont(m_launchButton);
    connect(m_launchButton, &QAbstractButton::clicked, this, [this] { launch(); });
    row->addWidget(m_launchButton);
    auto *changeBtn = new PushButton(QStringLiteral("更换版本"), card);
    applyButtonFont(changeBtn);
    connect(changeBtn, &QAbstractButton::clicked, this, [this] { changeVersion(); });
    row->addWidget(changeBtn);
    row->addStretch(1);
    lay->addLayout(row);
    m_leftCol->addWidget(card);
}

// ── 左栏 ②:当前文件夹卡(文件夹自己的名字 + 完整路径 + 三个动作)──
void HomePage::buildFolderCard(const ThemeTokens &tokens) {
    auto *card = new CardWidget(m_leftHost);
    card->setObjectName(QStringLiteral("homeFolderCard"));
    auto *lay = new QHBoxLayout(card);
    lay->setContentsMargins(16, 12, 16, 12);
    lay->setSpacing(8); // 12 太宽:这一行有"名字 + 路径 + 三个按钮",窄窗口下先挤没了空间
    lay->addWidget(new BodyLabel(QStringLiteral("当前文件夹"), card));
    m_dirName = new BodyLabel(QString(), card);
    {
        QFont font = m_dirName->font();
        font.setPixelSize(14);
        font.setWeight(QFont::DemiBold);
        m_dirName->setFont(font);
    }
    makeLabelElide(m_dirName, 48); // 文件夹名可以很长("1.21.5-optifine-自定义"),必须能省略
    lay->addWidget(m_dirName);
    m_dirDisplay = new BodyLabel(m_gameDir, card);
    m_dirDisplay->setObjectName(QStringLiteral("homeFolderPath"));
    m_dirDisplay->setTextColor(tokens.textTertiary);
    makeLabelElide(m_dirDisplay, 60); // 路径最容易被挤:给它一个"再挤也留 60"的下限
    lay->addWidget(m_dirDisplay, 1);
    auto *changeDirBtn = new PushButton(QStringLiteral("更改"), card);
    applyButtonFont(changeDirBtn);
    connect(changeDirBtn, &QAbstractButton::clicked, this, [this] { changeDirectory(); });
    lay->addWidget(changeDirBtn);
    auto *detectBtn = new PushButton(QStringLiteral("自动检测"), card);
    applyButtonFont(detectBtn);
    connect(detectBtn, &QAbstractButton::clicked, this, [this] { autoDetectDirectory(); });
    lay->addWidget(detectBtn);
    auto *openBtn = new PushButton(QStringLiteral("打开"), card);
    applyButtonFont(openBtn);
    connect(openBtn, &QAbstractButton::clicked, this, [this] { openDirectory(); });
    lay->addWidget(openBtn);
    m_leftCol->addWidget(card);
}

// ── 左栏 ③:联机入口卡(home_page.py:112-136,原样保留)──
void HomePage::buildMultiplayerCard(const ThemeTokens &tokens) {
    auto *card = new CardWidget(m_leftHost);
    card->setObjectName(QStringLiteral("homeMultiplayerCard"));
    auto *lay = new QHBoxLayout(card);
    lay->setContentsMargins(20, 12, 20, 12);
    lay->setSpacing(12);
    auto *icon = new QLabel(card);
    icon->setPixmap(fluent::icon(QStringLiteral("Globe"), FluentTheme::instance().isDark())
                        .pixmap(28, 28));
    lay->addWidget(icon);
    auto *text = new QVBoxLayout();
    text->setSpacing(2);
    text->addWidget(new StrongBodyLabel(QStringLiteral("联机 · 和朋友一起玩"), card));
    auto *desc = new BodyLabel(
        QStringLiteral("房间码加入 / P2P 打洞 / 中继兜底（开发中，先留入口）"), card);
    desc->setTextColor(tokens.textTertiary);
    makeLabelElide(desc, 80); // 同上:窄窗口下先省略,不许把整页撑宽
    text->addWidget(desc);
    lay->addLayout(text, 1);
    auto *lookBtn = new PushButton(QStringLiteral("看看方案"), card);
    applyButtonFont(lookBtn);
    connect(lookBtn, &QAbstractButton::clicked, this, [this] { openMultiplayer(); });
    lay->addWidget(lookBtn);
    m_leftCol->addWidget(card);
}

// ── 右栏:账号卡(正版 / 离线 = 新版那两枚 LOGO + 各自的输入)──
void HomePage::buildPlayerCard(const ThemeTokens &tokens) {
    auto *card = new CardWidget(m_rightHost);
    card->setObjectName(QStringLiteral("homePlayerCard"));
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(12);

    /* 顶行:标题 + 右上角两枚 LOGO(用户 2026-09-26 点名:「正版和离线登录,按照新版原来那个
     * 微软和断线的 logo 来做」)。素材是自绘的,出处见 assets/icons/ui/NOTICE.md:
     *   microsoft.svg    —— 四色方块,**保留官方四色,绝不染色**(识别度就在这里);
     *   disconnected.svg —— 断开的链环 + 一道斜杠,单色线稿,按主题令牌现染。 */
    auto *top = new QHBoxLayout();
    top->setSpacing(8);
    auto *caption = new BodyLabel(QStringLiteral("账户"), card);
    caption->setTextColor(tokens.textTertiary);
    top->addWidget(caption);
    top->addStretch(1);

    m_premiumLogo = new IconSelectButton(card);
    m_premiumLogo->setObjectName(QStringLiteral("homeEditionPremiumButton"));
    m_premiumLogo->setIconDir(QStringLiteral("ui"));
    m_premiumLogo->setIconFile(QStringLiteral("microsoft.svg"));
    m_premiumLogo->setIconHeight(20);
    m_premiumLogo->setToolTip(QStringLiteral("正版"));
    connect(m_premiumLogo, &QAbstractButton::clicked, this, [this] { setEdition(true); });
    top->addWidget(m_premiumLogo);

    m_offlineLogo = new IconSelectButton(card);
    m_offlineLogo->setObjectName(QStringLiteral("homeEditionOfflineButton"));
    m_offlineLogo->setIconDir(QStringLiteral("ui"));
    m_offlineLogo->setIconFile(QStringLiteral("disconnected.svg"));
    m_offlineLogo->setIconHeight(20);
    m_offlineLogo->setMonochrome(true);
    m_offlineLogo->setToolTip(QStringLiteral("离线"));
    connect(m_offlineLogo, &QAbstractButton::clicked, this, [this] { setEdition(false); });
    top->addWidget(m_offlineLogo);
    lay->addLayout(top);

    m_accountPane = new QStackedWidget(card);
    m_accountPane->setObjectName(QStringLiteral("homeAccountPane"));
    { // 正版那一页:账户状态 + 登录 / 刷新
        auto *pane = new QWidget(m_accountPane);
        pane->setStyleSheet(QStringLiteral("background: transparent;"));
        auto *v = new QVBoxLayout(pane);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(10);
        m_accountState = new BodyLabel(QStringLiteral("读取账户状态…"), pane);
        m_accountState->setObjectName(QStringLiteral("homeAccountState"));
        m_accountState->setWordWrap(true);
        m_accountState->setTextColor(tokens.textTertiary);
        v->addWidget(m_accountState);
        m_accountButton = new PushButton(QStringLiteral("登录"), pane);
        m_accountButton->setObjectName(QStringLiteral("homeAccountButton"));
        applyButtonFont(m_accountButton);
        connect(m_accountButton, &QAbstractButton::clicked, this, [this] { launchWithAccount(); });
        v->addWidget(m_accountButton, 0, Qt::AlignLeft);
        v->addStretch(1);
        m_panePremium = m_accountPane->addWidget(pane);
    }
    { // 离线那一页:就一个 ID 输入框(状态保留)
        auto *pane = new QWidget(m_accountPane);
        pane->setStyleSheet(QStringLiteral("background: transparent;"));
        auto *v = new QVBoxLayout(pane);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(10);
        m_offlineEdit = new LineEdit(pane);
        m_offlineEdit->setObjectName(QStringLiteral("homeOfflineNameEdit"));
        m_offlineEdit->setPlaceholderText(QStringLiteral("离线 ID（默认 Player）"));
        m_offlineEdit->setText(offlinePlayerName());
        m_offlineEdit->setFixedHeight(32);
        v->addWidget(m_offlineEdit);
        v->addStretch(1);
        m_paneOffline = m_accountPane->addWidget(pane);
    }
    lay->addWidget(m_accountPane, 1);
    m_rightCol->addWidget(card);

    setEdition(false); // 默认离线(用户点名;正版那条要用户主动切过去)
}

QString HomePage::currentVersion() const {
    // 状态键优先;它指向的文件夹没了(用户删了/换目录了)就退回第一个已安装的。
    const QStringList installed = scanInstalledVersions(m_gameDir);
    const QString saved = selectedVersionName();
    if (!saved.isEmpty() && installed.contains(saved))
        return saved;
    return installed.isEmpty() ? QString() : installed.first();
}

void HomePage::refresh() {
    // ① 当前版本
    const QString version = currentVersion();
    if (version.isEmpty()) {
        m_versionName->setText(QStringLiteral("（还没有已安装的版本）"));
        m_versionDetail->setText(
            QStringLiteral("去「下载 → Minecraft 版本」装一个，装完这里就能启动"));
    } else {
        m_versionName->setText(version);
        QStringList bits;
        const QString tag = versionLoaderTag(m_gameDir, version);
        if (!tag.isEmpty())
            bits << tag;
        const QString json = m_gameDir + QStringLiteral("/versions/") + version + QLatin1Char('/') +
                             version + QStringLiteral(".json");
        const QString jar = m_gameDir + QStringLiteral("/versions/") + version + QLatin1Char('/') +
                            version + QStringLiteral(".jar");
        bits << (QFileInfo::exists(jar) ? QStringLiteral("有自己的 jar")
                                        : QStringLiteral("靠继承 / 没有 jar"));
        if (!QFileInfo::exists(json))
            bits << QStringLiteral("缺版本 JSON");
        m_versionDetail->setText(bits.join(QStringLiteral(" · ")));
    }

    // ② 账户状态(右栏账号卡)
    const AccountSnapshot account = loadAccountSnapshot();
    if (accountCanLaunch(account)) {
        m_accountState->setText(QStringLiteral("已登录：%1（玩家名 %2）")
                                    .arg(account.accountName, account.playerName));
        /* 已登录就**不再摆第二枚"启动"**:启动在上面那张大卡上(新版版面只有那一枚),
         * 这里留下的只有"还能补做的事"(没登录 -> 登录;凭据过期 -> 刷新 / 重新登录)。 */
        m_accountButton->setVisible(false);
    } else if (account.loggedIn) {
        m_accountState->setText(
            QStringLiteral("登录了，但这个账号这次用不上：%1")
                .arg(!account.hasMcToken || account.mcExpired
                         ? QStringLiteral("凭据已过期（设置 → 账户 里刷新）")
                         : QStringLiteral("没有 Java 版档案")));
        m_accountButton->setText(QStringLiteral("刷新 / 重新登录"));
        m_accountButton->setVisible(true);
    } else {
        m_accountState->setText(QStringLiteral("还没登录。登录后可以用正版身份启动；"
                                               "不想登录就用离线。"));
        m_accountButton->setText(QStringLiteral("登录"));
        m_accountButton->setVisible(true);
    }

    // 启动那一枚:有版本才点得动(没版本时按下去只会弹一句"先装一个",不如直接禁用)
    if (m_launchButton != nullptr)
        m_launchButton->setEnabled(!version.isEmpty());

    // ③ 当前文件夹(名字 + 路径)
    const QDir dir(m_gameDir);
    m_dirName->setText(dir.dirName().isEmpty() ? m_gameDir : dir.dirName());
    m_dirDisplay->setText(QDir::toNativeSeparators(m_gameDir));
}

void HomePage::setEdition(bool premium) {
    m_premium = premium;
    /* 两枚 LOGO 就是选项本身(用户点名要新版那两枚):选中的那枚图标下面一条 accent 指示条,
     * 另一枚没有 —— 这个控件(IconSelectButton)的选中态本来就是按这个口径画的。 */
    if (m_premiumLogo != nullptr)
        m_premiumLogo->setChecked(premium);
    if (m_offlineLogo != nullptr)
        m_offlineLogo->setChecked(!premium);
    if (m_accountPane != nullptr)
        m_accountPane->setCurrentIndex(premium ? m_panePremium : m_paneOffline);
    std::fprintf(stderr, "[sxcl-ui] 主页形态: %s\n", premium ? "正版" : "离线");
}

void HomePage::launch() {
    /* 主页只有这一枚「启动游戏」:落到哪一路由**当前形态**决定。
     *   离线 -> launchOffline()(先把 ID 落盘,再强制离线身份起);
     *   正版 -> launchWithAccount()(账号不能用就去登录窗,**绝不**偷偷用离线身份跑起来)。 */
    if (m_premium) {
        launchWithAccount();
        return;
    }
    launchOffline();
}

void HomePage::changeVersion() {
    // 用户点名:"原来的所有安装版本都堆在主页整体砍掉。改成版本选择页。"
    if (auto *mw = qobject_cast<MainWindow *>(window())) {
        mw->switchToRoute(QStringLiteral("select"));
        return;
    }
    InfoBar::push(InfoBar::Type::Info, QStringLiteral("版本选择"),
                  QStringLiteral("版本选择页还没接上"), window(), 3000);
}

void HomePage::changeDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择游戏文件夹（不一定要叫 .minecraft）"), m_gameDir);
    if (dir.isEmpty())
        return;
    m_gameDir = QDir::fromNativeSeparators(dir);
    setGameDirectory(m_gameDir); // **状态保留**:以前这里只改内存,重启就回去了
    refresh();
}

void HomePage::autoDetectDirectory() {
    const QVector<GameFolder> folders = detectGameFolders(m_gameDir);
    const GameFolder *best = bestGameFolder(folders);
    if (!best) {
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("没找到游戏目录"),
                      QStringLiteral("点「更改」手动选一个"), window(), 5000);
        return;
    }
    m_gameDir = best->path;
    setGameDirectory(m_gameDir);
    refresh();
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

void HomePage::openDirectory() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_gameDir));
}

void HomePage::launchWithAccount() {
    if (currentVersion().isEmpty()) {
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("还没有版本可以启动"),
                      QStringLiteral("先去「下载 → Minecraft 版本」装一个"), window(), 4000);
        return;
    }
    const AccountSnapshot account = loadAccountSnapshot();
    /* 用户 2026-09-22 晚点名：「正版登录不登录就启动？」—— 这一路（「正版登录」那一页）
     * 的按钮**只负责登录**：账号不能用就**绝不起游戏**。以前会掉进"离线身份启动"，
     * 用户看到的就是"没登录也能开"，以为正版登录是摆设。想不用账号玩，切到「离线启动」那一页。 */
    if (!accountCanLaunch(account)) {
        if (!account.loggedIn) {
            InfoBar::push(InfoBar::Type::Info, QStringLiteral("先登录正版账号"),
                          QStringLiteral("这一路要用正版身份启动；不想登录就用离线（断线那枚）。"),
                          window(), 5000);
        } else if (!account.hasMcToken || account.mcExpired) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("登录凭据过期了，先刷新或重新登录"),
                          QStringLiteral("凭据已过期：在下面的登录窗口里重新登录一次（或去 设置 → 账户 "
                                         "点「刷新」免密续期）。"),
                          window(), 8000);
        } else {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("这个账户没有 Java 版档案"),
                          QStringLiteral("没买 Java 版（或档案没取到），正版这一路用不了；"
                                         "想进游戏就切到离线。"),
                          window(), 8000);
        }
        if (AuthLoginDialog *dialog = AuthLoginDialog::open(window())) {
            QObject::connect(dialog, &AuthLoginDialog::accountChanged, this,
                             [this] { refresh(); });
        }
        return;
    }
    if (accountCanLaunch(account)) {
        InfoBar::push(InfoBar::Type::Info, QStringLiteral("用已登录的正版账户启动"),
                      QStringLiteral("%1（玩家名 %2，内存 %3 MB）")
                          .arg(currentVersion(), account.playerName)
                          .arg(configuredMemoryMb()),
                      window(), 4000);
    }
    // 正版那一路:交给启动页按"能用账户就用"的规则走(可能是登录页/设备码)
    if (auto *mw = qobject_cast<MainWindow *>(window())) {
        mw->setNextLaunchOffline(false);
        mw->switchToLaunch(currentVersion());
        return;
    }
    launchFallback();
}

void HomePage::launchOffline() {
    // 用户点名:离线 ID 要能填、要状态保留;已登录正版的玩家也该能走这一路。
    setOfflinePlayerName(m_offlineEdit->text());
    if (currentVersion().isEmpty()) {
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("还没有版本可以启动"),
                      QStringLiteral("先去「下载 → Minecraft 版本」装一个"), window(), 4000);
        return;
    }
    m_offlineEdit->setText(offlinePlayerName());
    if (auto *mw = qobject_cast<MainWindow *>(window())) {
        mw->setNextLaunchOffline(true); // 强制离线:哪怕账户可用也不用它
        mw->switchToLaunch(currentVersion());
        return;
    }
    launchFallback();
}

void HomePage::launchFallback() {
    UiErrorContext ctx;
    ctx.page = QStringLiteral("主页 / home");
    ctx.action = QStringLiteral("启动 %1").arg(currentVersion());
    ctx.reason = QStringLiteral("启动器未初始化(window() 不是 MainWindow)");
    ctx.title = QStringLiteral("无法启动");
    pushUiError(window(), ctx, 5000);
}

void HomePage::openMultiplayer() {
    if (auto *mw = qobject_cast<MainWindow *>(window())) {
        mw->switchToRoute(QStringLiteral("multiplayer"));
        return;
    }
    InfoBar::push(InfoBar::Type::Info, QStringLiteral("联机"),
                  QStringLiteral("联机页即将上线"), window(), 2500);
}

QWidget *createHomePage(QWidget *parent) { return new HomePage(parent); }

} // namespace sxcl::ui
