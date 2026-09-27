/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 主页 —— **2026-09-27 版面**(用户口述,逐条照做)。
//
// 用户原话:
//   * 「主页右边账户两个滑块大小缩放,让它们长宽高都差不多大小;整个右侧的账户就选项卡从顶拉到底,
//      分别展示账户、然后玩家的他全身的那个皮肤模型就可以了。」
//   * 「登录过了这个账户,这次用不上什么,这些文字又给我整那个流水账,赶紧删掉;什么刷新重新登录
//      都删掉。有已经存的,你要重新登录就点退出登录,然后重新登录;没有登录过的就是登录,
//      哪有那么多事儿?」
//   * 「主页除了右面的排版以外,当前文件夹保留,联机那个东西直接去掉(移到左侧一栏的联机,
//      后续再实现)。」
//   * 「当前版本 1.12.1 下面那个"有自己的 JAR"改成这个游戏的配置:要么"原版 1.12.1",
//      要么把 Mod 加载器/光影列出来。」
//
// 所以这一页现在长这样:
//   左栏(拉伸) ① 当前版本卡(版本名 + **这一版的配置** + 启动 / 更换版本)
//              ② 当前文件夹卡(名字 + 完整路径 + 更改 / 自动检测 / 打开)
//   右栏(定宽) ③ 账户卡(顶):两枚形态 LOGO(**等大**)+ 一枚按钮(登录 / 退出登录,按状态给)
//              ④ 皮肤卡(从顶拉到底):玩家**全身**皮肤模型
//
// 删掉的(不是没做,是用户点名删的):
//   * 联机入口卡(联机在左边栏那一栏里,后续再实现);
//   * 账户卡上那几行状态文字(「已登录:…」「登录了,但这个账号这次用不上:…」)与
//     「刷新 / 重新登录」按钮 —— 账户区只按状态给一枚按钮;
//   * 版本行里的「有自己的 jar」/「靠继承 / 没有 jar」/「缺版本 JSON」。
//
// 状态保留(写我们自己的设置文件,见 game_folders.h):game.selected_version /
// launch.offline_name / game.default_dir / game.known_dirs。

#include "page_factory.h"
#include "elided_label.h" // 单行标签"装不下就省略号"(左栏被窗口挤窄时要用)
#include "game_folders.h"

#include "dialogs/account.h"
#include "dialogs/auth_dialog.h" // 未登录/凭据过期 -> 直接开登录窗(而不是偷偷用离线跑起来)
#include "fluent_theme.h"
#include "icon_select_button.h" // 正版/离线两枚 LOGO 的"图标当选项"控件(选中态 = 图标下一条 accent)
#include "libqf.h"
#include "main_window.h"
#include "skin_image.h"
#include "skin_store.h"
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
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace sxcl::ui {
namespace {

/* 右栏(账户 + 皮肤)固定宽度:300 逻辑像素 —— 用户要的是新版那个版面,新版就是 300。
 * 固定宽度意味着窗口变窄时被挤的是左栏,账户卡与皮肤不会被压成一条。 */
constexpr int kRightColumnWidth = 300;

/* 两枚形态 LOGO 的绘制盒:**同一个正方形**。
 * 用户 2026-09-27:「主页右边账户两个滑块大小缩放,让它们长宽高都差不多大小」——
 * 素材横纵比不一样(微软四色方块接近 1:1、断线链环 24x24),以前按素材比例算盒子,
 * 两枚按钮的宽高就对不上;现在两枚都钉在这个盒子上,按钮尺寸逐像素相同。 */
constexpr int kEditionLogoBox = 24;

/* 皮肤卡里的全身模型:一块自绘的画布。皮肤从 SkinStore 现取(拿不到就画剪影占位),
 * 主题一切、账户一换都只重画这一块,不动版面。 */
class SkinView : public QWidget {
public:
    explicit SkinView(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("homeSkinView"));
        setMinimumHeight(160);
        connect(&SkinStore::instance(), &SkinStore::changed, this, [this] { update(); });
    }

    QSize sizeHint() const override { return QSize(160, 320); }

protected:
    void paintEvent(QPaintEvent *) override {
        const QImage skin = SkinStore::instance().skin();
        // 模型比例固定 16:32 —— 先按可用高度算,再按可用宽度收一次(绝不裁切、绝不变形)
        const int availH = qMax(16, height() - 16);
        const int availW = qMax(16, width() - 16);
        const int modelH = qMin(availH, availW * 2);
        QPixmap pm = skin.isNull() ? skinBodyPlaceholder(modelH) : skinBodyPixmap(skin, modelH);
        if (pm.isNull())
            pm = skinBodyPlaceholder(modelH);
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawPixmap(QPoint((width() - pm.width()) / 2, (height() - pm.height()) / 2), pm);
    }
};

// 这一版的"配置"行:要么"原版 <版本>",要么把 Mod 加载器 / 光影列出来(用户点名的规则)。
QStringList installedShaderPacks(const QString &gameDir) {
    const QDir dir(gameDir + QStringLiteral("/shaderpacks"));
    if (!dir.exists())
        return QStringList();
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    QStringList names;
    for (const QFileInfo &entry : entries) {
        QString name = entry.fileName();
        if (entry.isFile()) {
            if (!name.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive))
                continue; // 光影包就是 zip 或文件夹
            name.chop(4);
        }
        if (!name.isEmpty())
            names.append(name);
    }
    return names;
}

} // namespace

class HomePage : public ScrollArea {
public:
    explicit HomePage(QWidget *parent);

protected:
    void showEvent(QShowEvent *event) override; // 从版本选择页/设置页回来要刷新(版本名可能变了)

private:
    void buildContent();
    // 三张卡各建各的(版面 = 左右两栏;说明见文件头)
    void buildHeroCard(const ThemeTokens &tokens);
    void buildFolderCard(const ThemeTokens &tokens);
    void buildAccountCard(const ThemeTokens &tokens);
    void buildSkinCard();
    void refresh();              // 重读"当前版本"与账户状态,重画卡上的字
    QString currentVersion() const;
    QString versionDetailText() const; // 这一版的配置(原版 … / 加载器 · 光影 …)
    void setEdition(bool premium);     // 正版 / 离线:两枚 LOGO 的选中态 + 账号卡换页 + 启动落到哪一路
    void onAccountClicked();           // 账户区那唯一一枚按钮:登录 / 退出登录(按状态)
    void logout();
    void changeVersion();        // → 路由 select(版本选择页)
    void changeDirectory();
    void autoDetectDirectory();
    void openDirectory();
    void launch();               // 主页那一枚「启动游戏」:按当前形态落到正版/离线两路之一
    void launchWithAccount();
    void launchOffline();
    void launchFallback(); // window() 不是 MainWindow 时的统一错误出口

    QString m_gameDir;
    QWidget *m_view = nullptr;
    QVBoxLayout *m_vBox = nullptr;      // 整页:只有"左右两栏"这一行
    QHBoxLayout *m_columns = nullptr;
    QWidget *m_leftHost = nullptr;      // 左栏宿主(游戏)
    QWidget *m_rightHost = nullptr;     // 右栏宿主(账户 + 皮肤,固定 300)
    QVBoxLayout *m_leftCol = nullptr;
    QVBoxLayout *m_rightCol = nullptr;

    BodyLabel *m_versionName = nullptr;   // 大号:当前版本(文件夹名)
    BodyLabel *m_versionDetail = nullptr; // 小字:这一版的配置(原版 … / 加载器 · 光影)
    PrimaryPushButton *m_launchButton = nullptr; // 主页唯一的一枚启动
    PushButton *m_accountButton = nullptr;       // 账户区唯一的一枚按钮
    AccountTask *m_accountTask = nullptr;        // 退出登录(工作线程;主线程不阻塞)
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
    buildAccountCard(tokens);
    buildSkinCard();
    m_leftCol->addStretch(1);
    // 右栏**不加弹性项**:账户卡在顶、皮肤卡吃掉剩下的整条高度(用户:"从顶拉到底")
}

// ── 左栏 ①:当前版本大卡(版本名 + 这一版的配置 + 启动 + 更换版本)──
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
    /* **名字与路径各占一行**(用户 2026-09-27:「那游戏目录你不能让 .minecraft 和目录路径他妈
     * 重叠在一起吧?有那么玩儿的吗?」)。以前两个都塞在同一行的 QHBoxLayout 里,各挂一个
     * makeLabelElide 的"再挤也留 XX"下限 —— 窗口一窄就被压到 48 / 60,一个被切断、一个只剩
     * 省略号,看着就是两段文字叠在一起。现在按 docs/25 §0 的总则拆成两行:
     *   ① 第一行:「当前文件夹」+ 文件夹名 + 右侧三个动作;**名字绝不省略**
     *      (它是这一行里最短的一个,自己的 sizeHint 就是宽度下限,窗口再窄也轮不到它被压);
     *   ② 第二行:完整路径**独占整行宽度**;真的超过整行宽才在**行尾**省略,全文挂在 tooltip 上。 */
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(16, 12, 16, 12);
    lay->setSpacing(6);

    auto *nameRow = new QHBoxLayout();
    nameRow->setSpacing(8);
    nameRow->addWidget(new BodyLabel(QStringLiteral("当前文件夹"), card));
    m_dirName = new BodyLabel(QString(), card);
    {
        QFont font = m_dirName->font();
        font.setPixelSize(14);
        font.setWeight(QFont::DemiBold);
        m_dirName->setFont(font);
    }
    /* 这里**故意不挂 makeLabelElide**:那个守门人的口径是"再挤也留 XX、不够就上省略号",
     * 名字会因此被切。名字保持自己的自然宽(= 它自己的宽度下限):真要放不下,是**窗口**
     * 先放不下(窗口下限 900x600,见 tools/ui_min_size.ps1),而不是名字被压。 */
    nameRow->addWidget(m_dirName);
    nameRow->addStretch(1);
    auto *changeDirBtn = new PushButton(QStringLiteral("更改"), card);
    applyButtonFont(changeDirBtn);
    connect(changeDirBtn, &QAbstractButton::clicked, this, [this] { changeDirectory(); });
    nameRow->addWidget(changeDirBtn);
    auto *detectBtn = new PushButton(QStringLiteral("自动检测"), card);
    applyButtonFont(detectBtn);
    connect(detectBtn, &QAbstractButton::clicked, this, [this] { autoDetectDirectory(); });
    nameRow->addWidget(detectBtn);
    auto *openBtn = new PushButton(QStringLiteral("打开"), card);
    applyButtonFont(openBtn);
    connect(openBtn, &QAbstractButton::clicked, this, [this] { openDirectory(); });
    nameRow->addWidget(openBtn);
    lay->addLayout(nameRow);

    m_dirDisplay = new BodyLabel(m_gameDir, card);
    m_dirDisplay->setObjectName(QStringLiteral("homeFolderPath"));
    m_dirDisplay->setTextColor(tokens.textTertiary);
    /* 独占一行:整行宽度都归它。下限留 120(它是一行里最容易被挤的那个,但绝不会被挤到看不见);
     * 超过整行宽时按行尾省略,全文见 tooltip —— 用户允许的就这一种省略。 */
    makeLabelElide(m_dirDisplay, 120);
    m_dirDisplay->setToolTip(m_gameDir);
    lay->addWidget(m_dirDisplay);
    m_leftCol->addWidget(card);
}

// ── 右栏 ①:账户卡(两枚等大的形态 LOGO + 一枚按钮)──
void HomePage::buildAccountCard(const ThemeTokens &tokens) {
    auto *card = new CardWidget(m_rightHost);
    card->setObjectName(QStringLiteral("homePlayerCard"));
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(12);

    /* 顶行:标题 + 两枚 LOGO(用户 2026-09-26 点名:「正版和离线登录,按照新版原来那个
     * 微软和断线的 logo 来做」)。素材是自绘的,出处见 assets/icons/ui/NOTICE.md:
     *   microsoft.svg    —— 四色方块,**保留官方四色,绝不染色**(识别度就在这里);
     *   disconnected.svg —— 断开的链环 + 一道斜杠,单色线稿,按主题令牌现染。
     * 两枚**画在同一个正方形盒子里**(kEditionLogoBox):用户 2026-09-27 点名"长宽高都差不多大小",
     * 所以尺寸不再跟着素材横纵比走 —— 两枚按钮的宽高逐像素相同(差 0 px)。 */
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
    m_premiumLogo->setIconBoxSize(QSize(kEditionLogoBox, kEditionLogoBox));
    m_premiumLogo->setToolTip(QStringLiteral("正版"));
    connect(m_premiumLogo, &QAbstractButton::clicked, this, [this] { setEdition(true); });
    top->addWidget(m_premiumLogo);

    m_offlineLogo = new IconSelectButton(card);
    m_offlineLogo->setObjectName(QStringLiteral("homeEditionOfflineButton"));
    m_offlineLogo->setIconDir(QStringLiteral("ui"));
    m_offlineLogo->setIconFile(QStringLiteral("disconnected.svg"));
    m_offlineLogo->setIconBoxSize(QSize(kEditionLogoBox, kEditionLogoBox));
    m_offlineLogo->setMonochrome(true);
    m_offlineLogo->setToolTip(QStringLiteral("离线"));
    connect(m_offlineLogo, &QAbstractButton::clicked, this, [this] { setEdition(false); });
    top->addWidget(m_offlineLogo);
    lay->addLayout(top);

    /* 账户区**只有这一枚按钮**,文字按状态给(用户 2026-09-27):
     *   已登录(本机存着账户)= 「退出登录」;没登录过 = 「登录」。
     * 别的文字与按钮(状态行 / 「刷新 / 重新登录」)全删 —— 要重新登录就点退出登录再登录。 */
    m_accountButton = new PushButton(QStringLiteral("登录"), card);
    m_accountButton->setObjectName(QStringLiteral("homeAccountButton"));
    applyButtonFont(m_accountButton);
    connect(m_accountButton, &QAbstractButton::clicked, this, [this] { onAccountClicked(); });

    m_accountPane = new QStackedWidget(card);
    m_accountPane->setObjectName(QStringLiteral("homeAccountPane"));
    { // 正版那一页:就上面那一枚按钮
        auto *pane = new QWidget(m_accountPane);
        pane->setStyleSheet(QStringLiteral("background: transparent;"));
        auto *v = new QVBoxLayout(pane);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(10);
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
    m_rightCol->addWidget(card, 0);

    setEdition(false); // 默认离线(用户点名;正版那条要用户主动切过去)
}

// ── 右栏 ②:皮肤卡(玩家的**全身**皮肤模型,从顶拉到底)──
void HomePage::buildSkinCard() {
    auto *card = new CardWidget(m_rightHost);
    card->setObjectName(QStringLiteral("homeSkinCard"));
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(new SkinView(card), 1); // 卡片里只有模型本身,没有一个字
    m_rightCol->addWidget(card, 1);        // 吃掉账户卡下面剩下的整条高度
}

QString HomePage::currentVersion() const {
    // 状态键优先;它指向的文件夹没了(用户删了/换目录了)就退回第一个已安装的。
    const QStringList installed = scanInstalledVersions(m_gameDir);
    const QString saved = selectedVersionName();
    if (!saved.isEmpty() && installed.contains(saved))
        return saved;
    return installed.isEmpty() ? QString() : installed.first();
}

QString HomePage::versionDetailText() const {
    const QString version = currentVersion();
    if (version.isEmpty())
        return QStringLiteral("去「下载 → Minecraft 版本」装一个，装完这里就能启动");

    /* 用户 2026-09-27:「当前版本 1.12.1 下面那个"有自己的 JAR"改成这个游戏的配置:
     * 要么"原版 1.12.1",要么把 Mod 加载器/光影列出来。」—— 只留这两类事实,
     * "有自己的 jar / 靠继承 / 缺版本 JSON"这些都不再出现在界面上。 */
    QStringList bits;
    // 加载器按"人话名 + 版本号"写(Forge 14.23.5.2859 / Fabric 0.15.11 / OptiFine …);
    // 一个加载器都没有 = 原版,那就只写"原版 <版本号>"。见 game_folders.h 的口径说明。
    const QString loader = versionLoaderLabel(m_gameDir, version);
    if (!loader.isEmpty())
        bits << loader;
    const QStringList shaders = installedShaderPacks(m_gameDir);
    if (!shaders.isEmpty()) {
        QString text = shaders.mid(0, 2).join(QStringLiteral("、"));
        if (shaders.size() > 2)
            text += QStringLiteral(" 等 %1 个").arg(shaders.size());
        bits << QStringLiteral("光影：") + text;
    }
    if (bits.isEmpty())
        return QStringLiteral("原版 ") + version;
    return bits.join(QStringLiteral(" · "));
}

void HomePage::refresh() {
    // ① 当前版本 + 这一版的配置
    const QString version = currentVersion();
    if (version.isEmpty())
        m_versionName->setText(QStringLiteral("（还没有已安装的版本）"));
    else
        m_versionName->setText(version);
    m_versionDetail->setText(versionDetailText());

    // ② 账户(右栏账户卡):**只有一枚按钮**,文字按状态给 —— 登录 / 退出登录
    const AccountSnapshot account = loadAccountSnapshot();
    const bool signedIn = account.loggedIn;
    m_accountButton->setText(signedIn ? QStringLiteral("退出登录") : QStringLiteral("登录"));
    // 皮肤:登录了就用这个账户的皮肤(取图在工作线程),没登录就是空 -> 画剪影占位
    SkinStore::instance().setAccount(signedIn ? account.uuid : QString());

    // 启动那一枚:有版本才点得动(没版本时按下去只会弹一句"先装一个",不如直接禁用)
    if (m_launchButton != nullptr)
        m_launchButton->setEnabled(!version.isEmpty());

    // ③ 当前文件夹(名字 + 路径)
    const QDir dir(m_gameDir);
    const QString dirName = dir.dirName().isEmpty() ? m_gameDir : dir.dirName();
    m_dirName->setText(dirName);
    m_dirName->setToolTip(dirName); // 名字原则上不被切;窗口真小到放不下时,悬停还能看全
    m_dirDisplay->setText(QDir::toNativeSeparators(m_gameDir));
    m_dirDisplay->setToolTip(QDir::toNativeSeparators(m_gameDir));
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

/* 账户区那一枚按钮:本机**存着账户**就是「退出登录」,没存过就是「登录」。
 * 用户 2026-09-27:「有已经存的,你要重新登录就点退出登录,然后重新登录;没有登录过的就是登录,
 * 哪有那么多事儿?」—— 所以这里不弹任何确认/解释,也不再有第二枚按钮。 */
void HomePage::onAccountClicked() {
    if (loadAccountSnapshot().loggedIn) {
        logout();
        return;
    }
    if (AuthLoginDialog *dialog = AuthLoginDialog::open(window())) {
        connect(dialog, &AuthLoginDialog::accountChanged, this, [this] { refresh(); });
    }
}

void HomePage::logout() {
    if (m_accountTask != nullptr && m_accountTask->running())
        return;
    auto *task = new AccountTask(AccountTask::Operation::Logout, this);
    m_accountTask = task;
    connect(task, &AccountTask::finished, this,
            [this, task](bool ok, const QString &message, const QString &rawError) {
                if (!ok) {
                    QString detail = message;
                    if (!rawError.isEmpty())
                        detail += QStringLiteral("\n") + rawError;
                    InfoBar::push(InfoBar::Type::Error, QStringLiteral("退出登录"), detail, window(),
                                  8000);
                }
                if (m_accountTask == task)
                    m_accountTask = nullptr;
                task->deleteLater();
                refresh();
            });
    task->start();
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

void HomePage::launchWithAccount() {
    if (currentVersion().isEmpty()) {
        InfoBar::push(InfoBar::Type::Warning, QStringLiteral("还没有版本可以启动"),
                      QStringLiteral("先去「下载 → Minecraft 版本」装一个"), window(), 4000);
        return;
    }
    const AccountSnapshot account = loadAccountSnapshot();
    /* 用户 2026-09-22 晚点名:「正版登录不登录就启动?」—— 这一路的按钮**只负责登录**:
     * 账号不能用就**绝不起游戏**。账号为什么不能用**不再写一屏流水账**(用户 2026-09-27):
     * 没登录 / 凭据过期 / 没有 Java 版档案 —— 一律把登录窗摆出来,用户自己决定。 */
    if (!accountCanLaunch(account)) {
        if (AuthLoginDialog *dialog = AuthLoginDialog::open(window())) {
            connect(dialog, &AuthLoginDialog::accountChanged, this, [this] { refresh(); });
        }
        return;
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

QWidget *createHomePage(QWidget *parent) { return new HomePage(parent); }

} // namespace sxcl::ui
