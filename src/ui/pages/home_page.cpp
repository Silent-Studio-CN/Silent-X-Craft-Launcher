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
#include "game_folders.h"
#include "page_shell.h" // pageTokenText(取令牌色;与下载/团队/更多同一份)

#include "dialogs/account.h"
#include "dialogs/auth_dialog.h" // 未登录/凭据过期 -> 直接开登录窗（而不是偷偷用离线跑起来）
#include "fluent_theme.h"
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
#include "fluent/fluent_segmented.h" // Pivot:离线/正版的滑动选项(PCL 形态)
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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

const QColor kSubtitleLight(0x60, 0x60, 0x60);
const QColor kSubtitleDark(0xAA, 0xAA, 0xAA);

} // namespace

class HomePage : public ScrollArea {
public:
    explicit HomePage(QWidget *parent);

protected:
    void showEvent(QShowEvent *event) override; // 从版本选择页/设置页回来要刷新(版本名可能变了)

private:
    void buildContent();
    void refresh();              // 重读"当前版本"与账户状态,重画两张卡
    QString currentVersion() const;
    void changeVersion();        // → 路由 select(版本选择页)
    void changeDirectory();
    void autoDetectDirectory();
    void openDirectory();
    void launchWithAccount();
    void launchOffline();
    void launchFallback(); // window() 不是 MainWindow 时的统一错误出口
    void openMultiplayer();

    QString m_gameDir;
    QWidget *m_view = nullptr;
    QVBoxLayout *m_vBox = nullptr;

    BodyLabel *m_versionName = nullptr;   // 大号:当前版本(文件夹名)
    BodyLabel *m_versionDetail = nullptr; // 小字:MC 版本 + 加载器 + 有没有 jar
    BodyLabel *m_accountState = nullptr;  // 正版卡的状态行
    PushButton *m_accountButton = nullptr;
    QLineEdit *m_offlineEdit = nullptr;   // 离线 ID(状态保留)
    Pivot *m_loginPivot = nullptr;        // 离线 / 正版 的滑动选项(PCL 形态)
    QStackedWidget *m_loginStack = nullptr; // 两张卡二选一显示
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
    m_vBox->setContentsMargins(28, 24, 28, 24);
    m_vBox->setSpacing(16);
    m_vBox->setAlignment(Qt::AlignTop);

    auto *title = new TitleLabel(QStringLiteral("主页"), m_view);
    auto *subtitle = new SubtitleLabel(QStringLiteral("Silent X Craft Launcher v0.1.0"), m_view);
    subtitle->setTextColor(kSubtitleLight, kSubtitleDark);
    m_vBox->addWidget(title);
    m_vBox->addWidget(subtitle);

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
    const ThemeTokens &tokens = FluentTheme::instance().tokens();

    // ── ① 当前版本卡 ──
    auto *versionCard = new CardWidget(m_view);
    auto *versionLay = new QHBoxLayout(versionCard);
    versionLay->setContentsMargins(20, 16, 20, 16);
    versionLay->setSpacing(16);
    {
        auto *text = new QVBoxLayout();
        text->setSpacing(4);
        auto *caption = new BodyLabel(QStringLiteral("当前游戏版本"), versionCard);
        caption->setTextColor(tokens.textTertiary);
        text->addWidget(caption);
        m_versionName = new BodyLabel(QStringLiteral("（还没有已安装的版本）"), versionCard);
        {
            QFont font = m_versionName->font();
            font.setPixelSize(26);
            font.setWeight(QFont::DemiBold);
            m_versionName->setFont(font);
        }
        text->addWidget(m_versionName);
        m_versionDetail = new BodyLabel(QString(), versionCard);
        m_versionDetail->setTextColor(tokens.textTertiary);
        text->addWidget(m_versionDetail);
        versionLay->addLayout(text, 1);
    }
    auto *changeBtn = new PrimaryPushButton(QStringLiteral("更换"), versionCard);
    applyButtonFont(changeBtn);
    connect(changeBtn, &QAbstractButton::clicked, this, [this] { changeVersion(); });
    versionLay->addWidget(changeBtn, 0, Qt::AlignVCenter);
    m_vBox->addWidget(versionCard);

    // ── ② 启动区:**PCL 那样的滑动选项**(离线 / 正版二选一,不同时摆在眼前) ──
    // 用户 2026-09-22 晚点名:「正版和离线做 PCL 一样的滑动选项，不同时存在，默认离线」。
    // Pivot 就是 libqf 里那套带滑动指示条的分段控件(与 PCL 的登录方式切换同一个形态)。
    /* 整块装进**一张卡**(与上下的版本卡 / 文件夹卡同宽、同内边距)。
     * 用户 2026-09-22 晚(第二次)点名:「正版登录和离线登录的块不对页」——
     * 以前这块是**裸的**:滑块贴着页面左边缘(没有卡的 20px 内边距),里面又各套了一层卡,
     * 于是"标题 / 滑块 / 卡里的文字"三样各对齐到不同的 x,和别的卡怎么都差一截。 */
    auto *loginCard = new CardWidget(m_view);
    auto *loginLay = new QVBoxLayout(loginCard);
    loginLay->setContentsMargins(20, 16, 20, 16);
    loginLay->setSpacing(12);
    m_loginPivot = new Pivot(loginCard);
    m_loginPivot->addItem(QStringLiteral("offline"), QStringLiteral("离线启动"));
    m_loginPivot->addItem(QStringLiteral("account"), QStringLiteral("正版登录"));
    m_loginPivot->setIndicatorColor(FluentTheme::instance().tokens().accent,
                                    FluentTheme::instance().tokens().accent);
    /* PivotItem 默认是 **18pt** 的大字(qf 移动端的口径),摆在主页上会和那条 3px 指示条挤在一起 ——
     * 用户 2026-09-22 晚点名「主页两个登录滑块和文字串了」。这里把字号/内边距/选中态**显式钉死**,
     * 不再依赖 qf 那套字号。 */
    for (const QString &key : {QStringLiteral("offline"), QStringLiteral("account")}) {
        if (PivotItem *it = m_loginPivot->item(key)) {
            QFont f = it->font();
            f.setPixelSize(14);
            f.setWeight(QFont::DemiBold);
            it->setFont(f);
            it->setProperty("hasIcon", false);
            it->setFixedHeight(34);
            it->setCursor(Qt::PointingHandCursor);
        }
    }
    m_loginPivot->setFixedHeight(38);
    m_loginPivot->setStyleSheet(
        QStringLiteral("Pivot { background: transparent; border: none; }"
                       // 内边距 10px:两个条目各 80 宽,文字 56 + 20 = 76 才装得下(18px 会被切掉)
                       "PivotItem { background: transparent; border: none; padding: 4px 10px; }"
                       "PivotItem[isSelected='true'] { color: %1; }"
                       "PivotItem[isSelected='false'] { color: %2; }")
            .arg(pageTokenText("accent"), pageTokenText("textSecondary")));
    loginLay->addWidget(m_loginPivot, 0, Qt::AlignLeft);

    auto *launchStack = new QStackedWidget(loginCard);
    m_loginStack = launchStack;
    {
        // 左:正版启动(裸页,**不再套第二层卡** —— 夹心卡在视觉上就是"不对页")
        auto *accountCard = new QWidget(launchStack);
        accountCard->setStyleSheet(QStringLiteral("background: transparent;"));
        auto *accountLay = new QVBoxLayout(accountCard);
        accountLay->setContentsMargins(0, 0, 0, 0);
        accountLay->setSpacing(8);
        auto *accountTitle = new StrongBodyLabel(QStringLiteral("正版启动"), accountCard);
        accountTitle->setStyleSheet(
            QStringLiteral("color: %1; font-size: 15px;").arg(pageTokenText("accent")));
        accountLay->addWidget(accountTitle);
        m_accountState = new BodyLabel(QStringLiteral("读取账户状态…"), accountCard);
        m_accountState->setWordWrap(true);
        m_accountState->setTextColor(tokens.textTertiary);
        accountLay->addWidget(m_accountState);
        accountLay->addStretch(1);
        m_accountButton = new PrimaryPushButton(QStringLiteral("启动"), accountCard);
        applyButtonFont(m_accountButton);
        connect(m_accountButton, &QAbstractButton::clicked, this, [this] { launchWithAccount(); });
        accountLay->addWidget(m_accountButton, 0, Qt::AlignLeft);
        launchStack->addWidget(accountCard);

        // 右:离线启动(用户点名:ID 输入框 + 状态保留;已登录正版也能用这一路)
        auto *offlineCard = new QWidget(launchStack);
        offlineCard->setStyleSheet(QStringLiteral("background: transparent;"));
        auto *offlineLay = new QVBoxLayout(offlineCard);
        offlineLay->setContentsMargins(0, 0, 0, 0);
        offlineLay->setSpacing(8);
        auto *offlineTitle = new StrongBodyLabel(QStringLiteral("离线启动"), offlineCard);
        offlineTitle->setStyleSheet(
            QStringLiteral("color: %1; font-size: 15px;").arg(pageTokenText("accent")));
        offlineLay->addWidget(offlineTitle);
        auto *offlineNote = new BodyLabel(
            QStringLiteral("离线 ID 就是游戏里的玩家名；已登录正版的账号也可以用这一路。"), offlineCard);
        offlineNote->setWordWrap(true);
        offlineNote->setTextColor(tokens.textTertiary);
        offlineLay->addWidget(offlineNote);
        offlineLay->addStretch(1);
        auto *row = new QWidget(offlineCard);
        auto *rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->setSpacing(8);
        m_offlineEdit = new LineEdit(row);
        m_offlineEdit->setPlaceholderText(QStringLiteral("离线 ID（默认 Player）"));
        m_offlineEdit->setText(offlinePlayerName());
        m_offlineEdit->setFixedHeight(32);
        rowLay->addWidget(m_offlineEdit, 1);
        auto *offlineBtn = new PrimaryPushButton(QStringLiteral("启动"), row);
        applyButtonFont(offlineBtn);
        /* 稳定 objectName:验收钩子(SXCL_UI_LAUNCH)点**真的那个启动按钮** ——
         * 这样"界面点启动 -> 补全文件 -> 起进程"整条链在真机上也能被验到,不必只靠命令行。 */
        offlineBtn->setObjectName(QStringLiteral("homeOfflineLaunchButton"));
        connect(offlineBtn, &QAbstractButton::clicked, this, [this] { launchOffline(); });
        rowLay->addWidget(offlineBtn, 0);
        offlineLay->addWidget(row);
        launchStack->addWidget(offlineCard);
    }
    // 默认**离线**(用户点名):正版那条要等用户主动切过去/登录
    m_loginStack->setCurrentIndex(0);
    m_loginPivot->setCurrentItem(QStringLiteral("offline"));
    connect(m_loginPivot, &Pivot::currentItemChanged, this, [this](const QString &key) {
        m_loginStack->setCurrentIndex(key == QLatin1String("account") ? 1 : 0);
    });
    loginLay->addWidget(launchStack, 1);
    m_vBox->addWidget(loginCard);

    // ── ③ 当前文件夹卡(用户:显示文件夹自己的名字,不强制叫 .minecraft)──
    auto *dirCard = new CardWidget(m_view);
    auto *dirLay = new QHBoxLayout(dirCard);
    dirLay->setContentsMargins(20, 12, 20, 12);
    dirLay->setSpacing(12);
    dirLay->addWidget(new BodyLabel(QStringLiteral("当前文件夹"), dirCard));
    m_dirName = new BodyLabel(QString(), dirCard);
    {
        QFont font = m_dirName->font();
        font.setPixelSize(14);
        font.setWeight(QFont::DemiBold);
        m_dirName->setFont(font);
    }
    dirLay->addWidget(m_dirName);
    m_dirDisplay = new BodyLabel(m_gameDir, dirCard);
    m_dirDisplay->setTextColor(tokens.textTertiary);
    dirLay->addWidget(m_dirDisplay, 1);
    auto *changeDirBtn = new PushButton(QStringLiteral("更改"), dirCard);
    applyButtonFont(changeDirBtn);
    connect(changeDirBtn, &QAbstractButton::clicked, this, [this] { changeDirectory(); });
    dirLay->addWidget(changeDirBtn);
    auto *detectBtn = new PushButton(QStringLiteral("自动检测"), dirCard);
    applyButtonFont(detectBtn);
    connect(detectBtn, &QAbstractButton::clicked, this, [this] { autoDetectDirectory(); });
    dirLay->addWidget(detectBtn);
    auto *openBtn = new PushButton(QStringLiteral("打开"), dirCard);
    applyButtonFont(openBtn);
    connect(openBtn, &QAbstractButton::clicked, this, [this] { openDirectory(); });
    dirLay->addWidget(openBtn);
    m_vBox->addWidget(dirCard);

    // ── ④ 联机入口卡(home_page.py:112-136,原样保留)──
    auto *mpCard = new CardWidget(m_view);
    auto *mpLay = new QHBoxLayout(mpCard);
    mpLay->setContentsMargins(20, 12, 20, 12);
    mpLay->setSpacing(12);
    auto *mpIcon = new QLabel(mpCard);
    mpIcon->setPixmap(fluent::icon(QStringLiteral("Globe"), FluentTheme::instance().isDark())
                          .pixmap(28, 28));
    mpLay->addWidget(mpIcon);
    auto *mpText = new QVBoxLayout();
    mpText->setSpacing(2);
    mpText->addWidget(new StrongBodyLabel(QStringLiteral("联机 · 和朋友一起玩"), mpCard));
    auto *mpDesc = new BodyLabel(
        QStringLiteral("房间码加入 / P2P 打洞 / 中继兜底（开发中，先留入口）"), mpCard);
    mpDesc->setTextColor(tokens.textTertiary);
    mpText->addWidget(mpDesc);
    mpLay->addLayout(mpText, 1);
    auto *lookBtn = new PushButton(QStringLiteral("看看方案"), mpCard);
    applyButtonFont(lookBtn);
    connect(lookBtn, &QAbstractButton::clicked, this, [this] { openMultiplayer(); });
    mpLay->addWidget(lookBtn);
    m_vBox->addWidget(mpCard);

    m_vBox->addStretch(1);
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
        bits << QStringLiteral("目录名就是版本名（可以随便改，不影响启动）");
        m_versionDetail->setText(bits.join(QStringLiteral(" · ")));
    }

    // ② 账户状态(正版卡)
    const AccountSnapshot account = loadAccountSnapshot();
    if (accountCanLaunch(account)) {
        m_accountState->setText(QStringLiteral("已登录：%1（玩家名 %2）")
                                    .arg(account.accountName, account.playerName));
        m_accountButton->setText(QStringLiteral("用正版身份启动"));
        m_accountButton->setEnabled(!version.isEmpty());
    } else if (account.loggedIn) {
        m_accountState->setText(
            QStringLiteral("登录了，但这个账号这次用不上：%1")
                .arg(!account.hasMcToken || account.mcExpired
                         ? QStringLiteral("凭据已过期（设置 → 账户 里刷新）")
                         : QStringLiteral("没有 Java 版档案")));
        m_accountButton->setText(QStringLiteral("登录 / 刷新"));
        m_accountButton->setEnabled(true);
    } else {
        m_accountState->setText(QStringLiteral("还没登录。登录后可以用正版身份启动；"
                                               "不想登录就切回上面的「离线启动」。"));
        m_accountButton->setText(QStringLiteral("登录"));
        m_accountButton->setEnabled(true);
    }

    // ③ 当前文件夹(名字 + 路径)
    const QDir dir(m_gameDir);
    m_dirName->setText(dir.dirName().isEmpty() ? m_gameDir : dir.dirName());
    m_dirDisplay->setText(QDir::toNativeSeparators(m_gameDir));
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
                          QStringLiteral("这一路要用正版身份启动；不想登录就切回上面的「离线启动」。"),
                          window(), 5000);
        } else if (!account.hasMcToken || account.mcExpired) {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("登录凭据过期了，先刷新或重新登录"),
                          QStringLiteral("凭据已过期：在下面的登录窗口里重新登录一次（或去 设置 → 账户 "
                                         "点「刷新」免密续期）。"),
                          window(), 8000);
        } else {
            InfoBar::push(InfoBar::Type::Warning, QStringLiteral("这个账户没有 Java 版档案"),
                          QStringLiteral("没买 Java 版（或档案没取到），正版这一路用不了；"
                                         "想进游戏请切回「离线启动」。"),
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
