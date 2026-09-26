/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 版本选择页 —— **2026-09-22 界面重构**（用户口述，规格见 docs/25 §3）+ 当日**晚**的第二轮重构。
//
// 用户原话（第一轮）：「原来的所有安装版本都堆在主页整体砍掉。改成版本选择页。用户可以更改已安装的
// 版本。选择页布局：左侧为文件夹列表"当前文件夹"，如果有用户导入历史，也提供切其他文件夹。我们不强制
// 根目录一定是 .minecraft，如果有别的名字，"当前文件夹"就显示这个文件夹的名字。」
//
// 用户原话（第二轮，看到第一版之后）：「游戏版本选择还是那个问题，整个设计理念给我顶下，**绝对不能
// 出现原生点选的画风**，版本选择现在也做侧 2 栏。另外当 2 栏被拉出，鼠标悬停的文件夹出现设置按钮。
// 可以自定义文件夹图标（目前内置狐狸头，铁砧，工作台，草方块等等），SXCL 用的图标都给他支持。
// 另外，记住了自适应！不要给我出现因为没放下导致的横向竖向滑动条。必须的除外（下载列表之类的）。」
//
// 所以这一版：
//   * 文件夹列表**不再是单选按钮那种原生味**，而是**复用主侧边栏同一个 NavPanel**（侧 2 栏）：
//     汉堡三横、48 <-> 322 折叠动画、150ms OutQuad、选中指示条 —— 与下载页那条、窗口左边那条
//     完全同一份实现（"不存在两套动画对不上"）。
//   * 文件夹图标**由用户挑**（folder_icons.h）：默认草方块，可换狐狸/铁砧/红石灯/模组/光影…
//     鼠标悬停那一行 **右侧出现齿轮**，点开在**页面内**挑图标（不弹原生文件框）。
//   * 自适应：图标用流式布局自动换行、卡片文字换行/省略，横向滚动条一律关掉
//     （只有"版本列表"这一栏允许竖向滚动 —— 那是列表本身，用户也认可"必须的除外"）。
//
// "版本 = versions/ 下的文件夹名"这条 PCL 概念由核心库保证：
//   启动靠目录名（实例名）+ 该目录里任意一份能解析出版本信息的 JSON + JSON 里的库/主类/资源索引，
//   与"MC 版本号"无关 —— 所以文件夹叫 114514、JSON 里的 id 也叫 114514 照样能跑。
//   右栏的元数据（加载器/能不能启动/为什么不能）全部来自 sxcl_instance_scan，与"版本页"同一份实现。

#include "page_factory.h"
#include "game_folders.h"
#include "page_shell.h"

#include "../folder_icons.h"
#include "../nav.h"

#include "fluent_theme.h"
#include "libqf.h"
#include "main_window.h"

#include "workers/bg_task.h"        // 一次性后台任务(阻塞活进工作线程,结果回界面线程)
#include "workers/ui_error.h"      // 统一错误出口(错误态要能写进剪贴板,不只一行红字)
#include "workers/instance_scan.h"  // 已安装版本扫描(界面层唯一实现;**只许工作线程调**)

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "fluent/fluent_cards.h"
#include "fluent/fluent_controls.h"
#include "fluent/fluent_labels.h"
#include "fluent/fluent_scroll.h"
#include "fluent/fluent_selection.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <cstring>

#include "sxcl/instance.h"

namespace sxcl::ui {
namespace {

const char *const kFolderPrefix = "folder:";
const char *const kImportKey = "folder_import";

// 当前版本那条指示条:贴着卡片左缘(卡片自己的描边在 x=1,所以从 2 起)、上下各留 12
constexpr int kMarkX = 2;
constexpr int kMarkInset = 12;

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

/* 当前版本 = **左侧那条强调色指示条**(用户 2026-09-26 口径:「用得着你告诉用户当前是什么」)。
 *
 * 形态与侧栏那条选中指示条是**同一套语言**(nav.cpp:144-174:3 逻辑像素宽、圆角 1.5、颜色取主题色),
 * 不再写"当前版本"四个字、也不再往版本名后面缀"← 当前"(两处都删掉了)。
 * 它**不占布局**(卡片里的行内容一个像素都不动),位置由 SelectPage::eventFilter 的 Resize 分支给。 */
class CurrentVersionMark : public QWidget {
public:
    explicit CurrentVersionMark(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents); // 点它 = 点这一行(整行仍然是"选它")
        setObjectName(QStringLiteral("sxclCurrentVersionMark")); // 验收 dump 按它认这条指示条
        setFixedWidth(kMarkWidth);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ThemeBridge::instance().accent());
        painter.drawRoundedRect(QRectF(rect()), kMarkRadius, kMarkRadius);
    }

private:
    static constexpr int kMarkWidth = 3;
    static constexpr qreal kMarkRadius = 1.5;
};

class SelectPage : public PageShell {
public:
    explicit SelectPage(QWidget *parent)
        : PageShell(QStringLiteral("版本选择"),
                    QStringLiteral("认的是文件夹名，不是 MC 版本号 · 悬停文件夹可换图标"),
                    QStringLiteral("sxclPage_select"), parent) {
        m_gameDir = resolveGameDirectory();
        m_folders = foldersWithoutProbe(); // 先给一份"不碰磁盘"的表:当前目录 + 用过的历史(探测在工作线程)
        m_scan = new BgTask(this);
        m_probe = new BgTask(this);
        buildBody();
        rebuildNav(true);
        reloadVersions();
    }

private:
    /* 一整行 = 一个版本:单击选它、悬停出齿轮(用户 2026-09-23 点名)。
     * 用**事件过滤器**而不是自定义控件:行是 CardWidget(库里给的),里面还有若干子控件,
     * 任何一个子控件收到鼠标事件都要当成"点了这一行" —— 过滤装在它们每一个身上最省事。 */
    bool eventFilter(QObject *watched, QEvent *event) override {
        QWidget *w = qobject_cast<QWidget *>(watched);
        if (w == nullptr) {
            return PageShell::eventFilter(watched, event);
        }
        QWidget *row = w;
        while (row != nullptr && !m_rowNames.contains(row)) {
            row = row->parentWidget();
        }
        if (row == nullptr) {
            return PageShell::eventFilter(watched, event);
        }
        NavToolButton *gear = m_rowGears.value(row, nullptr);
        if (gear != nullptr && (w == gear || gear->isAncestorOf(w))) {
            return PageShell::eventFilter(watched, event); // 齿轮自己的点击不进"整行选择"
        }
        switch (event->type()) {
        case QEvent::Resize:
            /* 当前版本那条指示条:卡片高度由内容与布局定(横向宽度也是自适应的),跟着量一次。
             * 索引里没有 = 这一行不是当前版本,什么都不做。 */
            if (CurrentVersionMark *mark = m_rowMarks.value(row, nullptr)) {
                mark->setGeometry(kMarkX, kMarkInset, mark->width(),
                                  qMax(0, row->height() - 2 * kMarkInset));
                mark->raise();
            }
            break;
        case QEvent::Enter:
            if (gear != nullptr)
                gear->setVisible(true);
            break;
        case QEvent::Leave:
            /* 行内的子控件之间来回移动也会来 Leave —— 只有光标真的出了整行才收齿轮,
             * 否则齿轮会一闪一闪(鼠标从文字移到卡片空白处就没了)。 */
            if (gear != nullptr && !row->rect().contains(row->mapFromGlobal(QCursor::pos())))
                gear->setVisible(false);
            break;
        case QEvent::MouseButtonRelease: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                choose(m_rowNames.value(row));
                return true;
            }
            break;
        }
        default:
            break;
        }
        return PageShell::eventFilter(watched, event);
    }

    /** 齿轮:选中这一版**并**进版本管理页(那一页才是改它设置的地方)。 */
    void openVersionSettings(const QString &name) {
        setSelectedVersionName(name);
        if (auto *mw = qobject_cast<MainWindow *>(window())) {
            mw->switchToRoute(QStringLiteral("versions"));
        }
        InfoBar::push(InfoBar::Type::Info, QStringLiteral("版本设置"),
                      QStringLiteral("已选中「%1」,在版本管理页改它的内存 / Java / 渲染后端")
                          .arg(name),
                      window(), 4000);
    }

    // ── 骨架：侧 2 栏(NavPanel) + 右内容(版本列表 / 图标选择) ──
    void buildBody() {
        // 侧 2 栏的容器：NavPanel 每次重建（换文件夹/换图标都要重排图标），放容器里好替换。
        m_navSlot = new QWidget(view());
        auto *slotLay = new QVBoxLayout(m_navSlot);
        slotLay->setContentsMargins(0, 0, 0, 0);
        slotLay->setSpacing(0);
        m_navLay = slotLay;

        /* 两栏版式（PageShell::beginSideLayout）：侧 2 栏**从内容区顶部开始**、贴页面左边缘。
         * 用户 2026-09-22 晚点名：「版本选择的侧2 没贴紧侧1，中间的空间很丑」——
         * 以前走 addContent()，整页左边距 28px + 标题压在上头，侧 2 既离侧 1 有 28px，
         * 又比侧 1 低一截。现在左外边距 0、标题只在右列上方。 */
        QVBoxLayout *rightLay = beginSideLayout(m_navSlot);

        m_stack = new QStackedWidget(view());
        m_stack->addWidget(buildVersionsPane());
        rightLay->addWidget(m_stack, 1);
    }

    QWidget *buildVersionsPane() {
        auto *pane = new QWidget(m_stack);
        auto *lay = new QVBoxLayout(pane);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(8);

        m_listHint = new BodyLabel(QString(), pane);
        m_listHint->setWordWrap(true);
        const QColor secondary = pageTokenColor("textSecondary");
        m_listHint->setTextColor(secondary, secondary);
        /* 失败也要能用:扫不出/扫不动时给一个**能点**的「重试」(用户口径,2026-09-26)。
         * 平时藏着,只有这一栏进错误态才露出来。 */
        m_retry = new PushButton(QStringLiteral("重试"), pane);
        m_retry->setVisible(false);
        connect(m_retry, &QPushButton::clicked, this, [this] { reloadVersions(); });
        auto *hintRow = new QWidget(pane);
        auto *hintLay = new QHBoxLayout(hintRow);
        hintLay->setContentsMargins(0, 0, 0, 0);
        hintLay->setSpacing(12);
        hintLay->addWidget(m_listHint, 1);
        hintLay->addWidget(m_retry, 0, Qt::AlignVCenter);
        lay->addWidget(hintRow);

        auto *scroll = new ScrollArea(pane);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // 横向一律不出（自适应）
        auto *holder = new QWidget(scroll);
        holder->setStyleSheet(QStringLiteral("background: transparent;"));
        scroll->setWidget(holder);
        m_listLay = new QVBoxLayout(holder);
        m_listLay->setContentsMargins(0, 0, 0, 0);
        m_listLay->setSpacing(8);
        m_listLay->setAlignment(Qt::AlignTop);
        lay->addWidget(scroll, 1);
        return pane;
    }

    /* ── 侧 2 栏：文件夹 ──
     *
     * 侧栏**只建一次**(探测结果回来那一刻)。为什么不是"先建一条只有当前目录的栏、
     * 探测回来再重建":重建会造出第二条 NavPanel(老的 deleteLater 还没走),外壳的状态机
     * 与"展开条数"读数都会跟着抖 —— 实测(tools/ui_rail_assert.ps1 route=select):
     * 侧2 被顶成 48、step2 的 expanded 变成 2。一次建好就没有第二条栏可言,
     * 与从前"构造期一次建好"是同一条时间线,只是那一次现在发生在工作线程给出结果之后。
     *
     * 展开状态**先按页面上那一条**(没有就按参数)定下来,建的时候一次到位。 */
    void rebuildNav(bool expand = false) {
        if (expand || m_nav == nullptr)
            m_navExpanded = true;
        else
            m_navExpanded = !m_nav->collapsed();
        probeFolders(); // 磁盘探测在工作线程;回来 -> buildNavPanel()(只建一次)
    }

    // 真正把侧栏建出来(界面线程;文件夹表来自工作线程的探测结果)
    void buildNavPanel(bool expanded) {
        if (m_nav) {
            m_navLay->removeWidget(m_nav);
            m_nav->deleteLater();
            m_nav = nullptr;
        }
        m_nav = new NavPanel(m_navSlot);
        m_nav->setObjectName(QStringLiteral("sxclVersionFolderNav")); // 用例按它找这一层
        // 侧栏**纵向撑满**(用户 2026-09-22 晚:"都改成侧边栏挤压右侧大页,不接受向下挤压"):
        // 这一列自己吃掉整条高度,右栏才是被挤的那一边 —— 以前 AlignTop 让面板只有内容高,
        // 视觉上像"东西被压到下面去了"。
        m_navLay->addWidget(m_nav, 1);
        /* 这条侧栏是**重建**出来的(换文件夹/换图标都走这里 -> 老那条 deleteLater):
         * 外壳的登记不能只做一次,否则状态机盯的是那条已经被删掉的老栏 ——
         * 重建之后"展开主栏"就收不回这一条(实测:SXCL_UI_RAILS_TEST 两步 expanded=2)。
         * 页面构造期 window() 还不是 MainWindow(那时页面还没有父),自动跳过;
         * 外壳在把页面挂进内容栈时会登记一次。 */
        if (auto *mw = qobject_cast<MainWindow *>(window()))
            mw->registerRail(m_nav);

        /* 文件夹表(m_folders)由**工作线程**探测后回填(probeFolders)。这一趟只用手上这份:
         * 构造期 = 当前目录 + 用过的历史(读一次设置,不碰磁盘)。探测结果回来会**再重建一次**
         * 侧栏(顺序/图标/命中集合与从前 detectGameFolders 同步跑时完全一致)。 */
        QVector<GameFolder> folders = m_folders;
        bool currentListed = false;
        for (const GameFolder &f : folders) {
            if (samePath(f.path, m_gameDir))
                currentListed = true;
        }
        if (!currentListed) {
            // 手动导入的目录还没进探测列表：补一条，保证"当前文件夹"永远在列表里
            GameFolder self;
            self.path = normPath(m_gameDir);
            self.label = QStringLiteral("当前配置");
            self.exists = true;
            folders.prepend(self);
        }

        for (const GameFolder &f : folders) {
            NavItem item;
            item.routeKey = QString::fromLatin1(kFolderPrefix) + f.path;
            // 名字都叫 .minecraft 的时候根本分不清谁是谁 —— 上面名字、下面小字路径
            // (用户 2026-09-22 晚点名),路径过长时中间省略。
            item.title = f.name();
            item.subtitle = QDir::toNativeSeparators(f.path);
            item.actionIcon = QStringLiteral("Setting"); // 悬停出现齿轮（用户点名）
            m_nav->addItem(item);
            // 图标在**应用起来之后**现拼（NavItem 里不能放 QPixmap，见 nav.h 的说明）
            m_nav->setItemIconPixmap(item.routeKey, folderIconForId(folderIconId(f.path), 24).pixmap(24, 24));
        }

        NavItem importItem;
        importItem.routeKey = QString::fromLatin1(kImportKey);
        importItem.qfIcon = QStringLiteral("Add");
        importItem.title = QStringLiteral("导入文件夹…");
        importItem.bottom = true;
        m_nav->addItem(importItem);

        for (const GameFolder &f : folders) {
            if (samePath(f.path, m_gameDir)) {
                m_nav->setCurrent(QString::fromLatin1(kFolderPrefix) + f.path);
                break;
            }
        }

        connect(m_nav, &NavPanel::routeChanged, this, [this](const QString &key) {
            if (key == QLatin1String(kImportKey)) {
                importFolder();
                return;
            }
            if (!key.startsWith(QLatin1String(kFolderPrefix)))
                return;
            const QString path = key.mid(int(qstrlen(kFolderPrefix)));
            if (samePath(path, m_gameDir))
                return;
            m_gameDir = path;
            setGameDirectory(m_gameDir); // 状态保留：选的文件夹要记住
            m_stack->setCurrentIndex(0);
            rebuildNav();
            reloadVersions();
        });
        connect(m_nav, &NavPanel::itemAction, this, [this](const QString &key) {
            if (!key.startsWith(QLatin1String(kFolderPrefix)))
                return;
            // 弹窗锚在**那颗齿轮**上（NavPanel 把齿轮挂在这一行下面）
            m_lastActionAnchor = m_nav->actionButton(key);
            showIconPicker(key.mid(int(qstrlen(kFolderPrefix))));
        });

        m_nav->setCollapsed(!expanded);
    }

    /* 文件夹探测(每个候选都要数 <dir>/versions 里的版本 —— 磁盘活)在工作线程里跑;
     * 结果回来才建侧栏。同一条栏被替换的那两处(换文件夹/换图标)走的是同一条路:
     * rebuildNav() -> probeFolders() -> buildNavPanel()。 */
    void probeFolders() {
        if (m_probe->running())
            return;
        m_probe->start(QStringLiteral("folder-probe"),
                       [this] { m_foldersProbed = detectGameFolders(m_gameDir); },
                       [this] {
                           if (m_probe->cancelled())
                               return;
                           m_folders = m_foldersProbed;
                           buildNavPanel(m_navExpanded);
                       });
    }

    // 不碰磁盘的那份表:当前目录 + 用过的历史(与 detectGameFolders 的前半段同口径)
    QVector<GameFolder> foldersWithoutProbe() const {
        QVector<GameFolder> out;
        GameFolder self;
        self.path = normPath(m_gameDir);
        self.label = QStringLiteral("当前配置");
        self.exists = true;
        out.append(self);
        for (const QString &known : knownGameDirs()) {
            if (samePath(known, m_gameDir))
                continue;
            GameFolder f;
            f.path = normPath(known);
            f.label = QStringLiteral("用过/导入的");
            f.exists = true;
            out.append(f);
        }
        return out;
    }

    // 当前文件夹的显示名(状态行用)
    QString shortName() const {
        const QString name = QDir(m_gameDir).dirName();
        return name.isEmpty() ? m_gameDir : name;
    }

    void importFolder() {
        // 只有"导入一个磁盘上的文件夹"这一步需要系统目录选择器（没有别的办法拿到任意路径）；
        // 页面本身与挑图标都不用它 —— 用户反对的是页面的"原生点选画风"。
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择游戏文件夹（不一定要叫 .minecraft）"), m_gameDir);
        if (dir.isEmpty())
            return;
        m_gameDir = normPath(dir);
        setGameDirectory(m_gameDir); // 顺带进 game.known_dirs 历史
        m_stack->setCurrentIndex(0);
        rebuildNav();
        reloadVersions();
    }

    /** 齿轮 -> **弹窗**挑图标（用户点名：弹窗、颜色选择器样式，不做单页）。
     *  锚点就用那颗齿轮本身，选完先关窗再重建侧栏。 */
    void showIconPicker(const QString &path) {
        const QString title = QFileInfo(path).fileName();
        QWidget *anchor = m_lastActionAnchor;
        showFolderIconPopup(anchor != nullptr ? anchor : m_nav, path, title,
                            [this, path, title](const QString &id) {
                                setFolderIconId(path, id);
                                rebuildNav();
                                InfoBar::push(InfoBar::Type::Success, QStringLiteral("图标已更新"),
                                              QStringLiteral("「%1」的图标已保存（下次打开还是它）")
                                                  .arg(title.isEmpty() ? path : title),
                                              window(), 3000);
                            });
    }

    void clearVersionRows() {
        m_rowMarks.clear(); // 卡片马上要被删:先撤掉指示条的登记,免得留下悬空指针
        while (QLayoutItem *item = m_listLay->takeAt(0)) {
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
    }

    // ── 右栏：这个文件夹下的已安装版本 ──
    /* 扫描**在工作线程里**(sxcl_instance_scan 是磁盘活:每个 <dir>/versions/<id>/ 都要看
     * JSON/jar)。以前它同步跑在这里 —— 换文件夹、点进这一页、甚至页面构造期都在界面线程上
     * 扫盘;那正是"点一下卡一下"的来源之一。现在:先把"正在读取…"画出来,结果回来再填。
     *
     * 三态(与版本页同一套口径):扫到了 -> 列出来;扫到了但一个都没有 -> 空态文案;
     * 扫不动(目录不存在/读不了) -> **错误态 + 能点的「重试」**,绝不只写一行红字。 */
    void reloadVersions() {
        clearVersionRows();
        m_rowNames.clear();
        m_rowGears.clear();
        m_retry->setVisible(false);
        m_listHint->setText(QStringLiteral("正在读取「%1」里的已安装版本…").arg(shortName()));
        if (m_scan->running())
            return; // 上一轮还在跑:结果回来填**最新**那个文件夹(切得快也不会排两次队)
        m_scan->start(QStringLiteral("installed-scan"),
                      [this] { m_scanned = scanInstalledInstances(m_gameDir, &m_scanError); },
                      [this] { fillVersions(); });
    }

    // 界面线程:把工作线程扫到的结果画成行(每行的判据/文案与从前逐字一致)
    void fillVersions() {
        clearVersionRows();
        m_rowNames.clear();
        m_rowGears.clear();
        m_rowMarks.clear();
        const QString shownName = shortName();
        const QString saved = selectedVersionName();
        const QColor secondary = pageTokenColor("textSecondary");
        int shown = 0;
        if (m_scanError.isEmpty()) {
            for (const InstalledInstance &inst : m_scanned) {
                auto *card = new CardWidget(m_listLay->parentWidget());
                card->setMinimumHeight(62);
                card->setCursor(Qt::PointingHandCursor);
                auto *rowLay = new QHBoxLayout(card);
                rowLay->setContentsMargins(20, 8, 16, 8);
                rowLay->setSpacing(12);

                auto *text = new QVBoxLayout();
                text->setSpacing(2);
                const QString name = inst.id;
                const bool isCurrent = (name == saved);
                /* 当前版本**不再加文字后缀**(用户 2026-09-26:不许再写一句"当前是什么")——
                 * 它只由下面那条左侧强调色指示条表达。 */
                auto *title = new BodyLabel(name, card);
                {
                    QFont font = title->font();
                    font.setPixelSize(15);
                    font.setWeight(QFont::DemiBold);
                    title->setFont(font);
                }
                // 长版本名**省略**而不是把卡片撑宽（自适应：横向滚动条一律不出）
                title->setToolTip(name);
                text->addWidget(title);

                QStringList bits;
                if (!inst.summary.isEmpty())
                    bits << inst.summary;
                // 只在**确信**时才说原版是哪个：以前会显示"原版 1.12.2(猜的)"，
                // 用户 2026-09-22 晚点名嫌它难看（"我真没绷住"）——猜的就别写出来。
                if (!inst.baseVersion.isEmpty() && inst.baseReliable)
                    bits << QStringLiteral("原版 %1").arg(inst.baseVersion);
                bits << (inst.hasJar ? QStringLiteral("有 jar") : QStringLiteral("无自己的 jar"));
                if (!inst.launchable)
                    bits << QStringLiteral("不能启动：%1")
                                .arg(inst.problem.isEmpty()
                                         ? QString::fromUtf8(sxcl_instance_problem_default_text(
                                               static_cast<sxcl_instance_problem>(inst.problemCode)))
                                         : inst.problem);
                auto *detail = new BodyLabel(bits.join(QStringLiteral(" · ")), card);
                detail->setWordWrap(true);
                detail->setTextColor(secondary, secondary);
                text->addWidget(detail);
                rowLay->addLayout(text, 1);

                if (!inst.launchable) {
                    // 警示三角是**自绘 svg**(用户点名:不要 emoji),颜色跟随主题
                    auto *bad = new InfoIconWidget(InfoBarIcon::Warning, card);
                    bad->setFixedSize(16, 16);
                    bad->setToolTip(QStringLiteral("这一份还不能启动（原因见左边那行小字）"));
                    rowLay->addWidget(bad, 0, Qt::AlignVCenter);
                }

                if (isCurrent) {
                    /* 当前版本:**只**留左侧那条强调色指示条(文字标签整条删掉 —— 用户原话
                     * 「用得着你告诉用户当前是什么」)。几何在 eventFilter 的 Resize 分支里给,
                     * 这里先按卡片的初始高度放一个位置,免得第一帧闪一下没有条。 */
                    auto *mark = new CurrentVersionMark(card);
                    mark->setGeometry(kMarkX, kMarkInset, mark->width(),
                                      qMax(0, card->minimumHeight() - 2 * kMarkInset));
                    mark->raise();
                    m_rowMarks.insert(card, mark);
                }
                /* 悬停出现的**齿轮**(用户 2026-09-23:「改为单击版本就选择,悬停显示齿轮,进入版本设置」)。
                 * 以前这里是常显的「用这个」按钮:一屏全是按钮,而且它左边那颗图标在深浅主题下显示异常。
                 * 现在 整行单击 = 选它;齿轮 = 进版本管理页改它的设置。 */
                auto *gear = new NavToolButton(QStringLiteral("Setting"), card);
                gear->setToolTip(QStringLiteral("版本设置(%1)").arg(name));
                gear->setObjectName(QStringLiteral("versionRowGear"));
                gear->setVisible(false);
                rowLay->addWidget(gear, 0, Qt::AlignVCenter);
                m_rowGears.insert(card, gear);
                QObject::connect(gear, &NavToolButton::clicked, this,
                                 [this, name](bool) { openVersionSettings(name); });

                card->setCursor(Qt::PointingHandCursor);
                card->setToolTip(QStringLiteral("点一下就用它启动(%1)").arg(name));
                m_rowNames.insert(card, name);
                card->installEventFilter(this);
                const QList<QWidget *> kids = card->findChildren<QWidget *>();
                for (QWidget *kid : kids) {
                    kid->installEventFilter(this);
                }

                m_listLay->addWidget(card);
                ++shown;
            }
        } else {
            /* 扫不动 = 错误态:照实说原因 + 给能点的重试(不是只写一行红字) */
            m_listHint->setText(QStringLiteral("「%1」读不出来：%2").arg(shownName, m_scanError));
            m_retry->setVisible(true);
            pushUiError(this,
                        UiErrorContext{QStringLiteral("版本选择页 / select"),
                                       QStringLiteral("读取已安装版本"),
                                       m_scanError,
                                       QStringLiteral("游戏目录：%1").arg(m_gameDir),
                                       QStringLiteral("读取已安装版本失败")},
                        6000);
        }

        if (m_scanError.isEmpty() && shown == 0) {
            m_listHint->setText(
                QStringLiteral("「%1」里还没有已安装的版本（去「下载 → Minecraft 版本」装一个）")
                    .arg(shownName));
        } else if (m_scanError.isEmpty()) {
            m_listHint->setText(
                QStringLiteral("「%1」里有 %2 个版本；点一行就用它启动，右上角齿轮进版本设置")
                    .arg(shownName)
                    .arg(shown));
        }
    }

    void choose(const QString &name) {
        setSelectedVersionName(name); // game.selected_version：关掉重开也记得
        InfoBar::push(InfoBar::Type::Success, QStringLiteral("已切换当前版本"),
                      QStringLiteral("%1（目录名就是版本名，随便改不影响启动）").arg(name), window(),
                      4000);
        if (auto *mw = qobject_cast<MainWindow *>(window()))
            mw->switchToRoute(QStringLiteral("home"));
    }

    QString m_gameDir;
    BgTask *m_scan = nullptr;   // 已安装版本扫描(工作线程)
    BgTask *m_probe = nullptr;  // 游戏文件夹探测(工作线程)
    QVector<InstalledInstance> m_scanned; // 工作线程写、界面线程读(队列投递保证先后)
    QString m_scanError;
    QVector<GameFolder> m_folders;        // 当前用于建侧栏的文件夹表
    QVector<GameFolder> m_foldersProbed;  // 工作线程探测结果
    bool m_navExpanded = true;            // 侧栏展开状态(重建时要保持住)
    PushButton *m_retry = nullptr;        // 扫不动时的「重试」
    QWidget *m_navSlot = nullptr;
    QVBoxLayout *m_navLay = nullptr;
    NavPanel *m_nav = nullptr;
    QWidget *m_lastActionAnchor = nullptr; // 弹窗要锚在用户点的那颗齿轮上
    QHash<QWidget *, QString> m_rowNames;  // 行 -> 版本名(整行单击用)
    QHash<QWidget *, NavToolButton *> m_rowGears; // 行 -> 那颗悬停齿轮
    QHash<QWidget *, CurrentVersionMark *> m_rowMarks; // 行 -> 当前版本那条强调色指示条(只有当前版本有)
    QStackedWidget *m_stack = nullptr;
    QVBoxLayout *m_listLay = nullptr;
    BodyLabel *m_listHint = nullptr;
};

} // namespace

QWidget *createVersionsSelectPage(QWidget *parent) { return new SelectPage(parent); }

} // namespace sxcl::ui
