/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 下载页(用户 2026-09-22 口述 + 同日的澄清)。
//
// 布局 = **双层侧边栏**:窗口左边那条主侧边栏之外,这一页左边再来一条**同类侧边栏**
// (左 2),三个入口:
//   草方块 · Minecraft 版本   (现有 versions_page 整页搬进来,标题改 "Minecraft 版本")
//   拼图   · MOD             (本轮只做壳;按 docs/22 的 PCL 线路做)
//   太阳   · 光影            (同上)
//
// 用户原话:「我指的是按照左边栏做个双层侧边栏。要包含完整的动画,三横菜单缩放一系列」——
// 所以这一层**不是自画的按钮列**,而是**直接复用主侧边栏那个 NavPanel 组件**:
// 汉堡键(Menu 三横)、折叠/展开 48 <-> 322、150ms OutQuad、条目 40x36 <-> 312x36 的
// compacted 切换、选中指示条 [0,10,3,16] —— 全都是同一份实现,不存在"两套动画对不上"。
//
// 图标(全部来自 PCL 自己的资源,出处照 assets/icons/pcl/index.tsv):
//   草方块 = assets/icons/blocks/Grass.png(Python BLOCK_FILES["vanilla"])
//   拼图   = assets/icons/pcl/mod.svg      (index.tsv:15 原名称「Mod」,来源 PageDownloadLeft.xaml)
//   太阳   = assets/icons/pcl/shader.svg   (index.tsv:19 原名称「光影包」,同一 XAML)
//
// 侧2 页脚的**版本形态**(Java 版 / 基岩版):**两个图标点选钮**,不是一个滑块。
//   用户 2026-09-26 原话:「我想要的是咖啡杯代表 Java,然后基岩版再找一个代表,
//   点这两个所代表的图标,而不是一个按钮,非常诡异啊」。
//   版面是 [图标 A] | [图标 B]:中间一条**竖直分隔线**,两颗图标各自可点;
//   咖啡杯     = assets/icons/edition/java_logo.svg(官方 Java 咖啡杯:devicon 的 java-original,
//                128x128 矢量,fill #0074BD —— 不用任何自绘替代品);
//   基岩版 LOGO = assets/icons/edition/bedrock_logo.png(用户本机基岩版 APK 里
//                assets/assets/resource_packs/vanilla/textures/ui/title.png 的**官方 MINECRAFT
//                标题 LOGO**;原图 1937x333 RGBA,**只裁画布**(裁到字母墨迹包围盒 ->
//                1898x273,像素逐字节未改、没有抠底改色),这样"画布高 = 字母墨迹高";
//                同一 APK 里扒出来的那枚 16x16 基岩方块留作备用文件 bedrock_block.png,界面不用)。
//   两枚素材的解包/下载出处与 sha256 逐条写在 assets/icons/edition/NOTICE.md。
//   选中态 = 图标**下方**一条 2 逻辑像素的 accent 指示条(长度 = 图标宽度,距图标下缘 4 像素;
//   用户点名不要"蓝色方框");整组在页脚里**水平居中**(前后各一个伸缩项);分隔线 = 容器
//   paintEvent + QPen(令牌 separator, 0)
//   —— 恒定 1 个**设备**像素,不是控件(docs/27 §12 第 1 条)。
//   点选只写状态键 game.edition,不弹任何解释性弹窗(docs/27 §11.5 文字纪律);"还没接入"这件事
//   只留在基岩版那枚图标的 tooltip 里(悬停才出现),折叠区/日志区也没有多余文案。

#include "page_factory.h"
#include "page_shell.h"

#include "../fluent_theme.h"
#include "../icon_registry.h"
#include "../icon_select_button.h" // 页脚的"版本形态"图标点选钮(docs/27 §11 按钮最少化)
#include "../nav.h"
#include "workers/ui_paths.h"

#include "sxcl/settings.h"  // 基岩版开关的状态键(game.edition)要落盘

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "fluent/fluent_cards.h"
#include "fluent/fluent_controls.h"
#include "fluent/fluent_labels.h"
#include "fluent/fluent_selection.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QHBoxLayout>
#include <QPainter> // 页脚那条竖直分隔线:容器 paintEvent 里画(docs/27 §12)
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace sxcl::ui {
namespace {

/** 当前"版本形态"：game.edition（java / bedrock）。读不出来就按 java —— 用户点名默认是 Java。 */
QString currentEdition() {
    const QByteArray path = uiSettingsFilePath().toUtf8();
    sxcl_settings *st = sxcl_settings_open(path.constData());
    if (!st)
        return QStringLiteral("java");
    const char *raw = sxcl_settings_get(st, "game.edition", "java");
    const QString edition = QString::fromUtf8(raw ? raw : "java");
    sxcl_settings_free(st);
    return edition.isEmpty() ? QStringLiteral("java") : edition;
}

// 占位卡:MOD / 光影这两栏现在没内容,如实说清楚"这一栏会做什么、什么时候做"。
CardWidget *buildPlaceholderCard(const QString &title, const QString &body, QWidget *parent) {
    auto *card = new CardWidget(parent);
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(6);
    auto *head = new StrongBodyLabel(title, card);
    head->setStyleSheet(QStringLiteral("color: %1; font-size: 15px;").arg(pageTokenText("accent")));
    lay->addWidget(head);
    auto *text = new BodyLabel(body, card);
    text->setWordWrap(true);
    const QColor secondary = pageTokenColor("textSecondary");
    text->setTextColor(secondary, secondary);
    lay->addWidget(text);
    return card;
}

/* 页脚那两个图标钮的**容器**:两颗图标之间那条竖直分隔线由它自己画。
 *
 * docs/27 §12 第 1 条(线不能是一个控件):拿 1 逻辑像素的控件当线,在 dpr=1.5 的屏幕上会占
 * 1~2 个物理像素(真机截图扫列实测"一粗一细"),QFrame::VLine 更糟 —— Fusion 的默认调色板是
 * 浅色的,深色界面里直接画成**白线**。正解就是这里:**栏与栏零间距相邻,线由容器在 paintEvent
 * 里用 QPen(颜色令牌, 0) 画**(宽度 0 = cosmetic = 恒定 **1 个设备像素**),颜色取主题令牌
 * (本仓库里这支令牌叫 separator —— 就是方案里写的 line 那一支)。
 *
 * 线**在两颗图标之间的中点**、竖直居中:高度取两个钮的并集(不探出、不缩进),
 * 位置每次绘制时按子控件几何现算,所以折叠/展开/换主题都不用额外接线。 */
class EditionPickRow : public QWidget {
public:
    explicit EditionPickRow(QWidget *parent) : QWidget(parent) {}

    void setButtons(QWidget *left, QWidget *right) {
        m_left = left;
        m_right = right;
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (m_left == nullptr || m_right == nullptr)
            return;
        const QRect a = m_left->geometry();
        const QRect b = m_right->geometry();
        if (a.isEmpty() || b.isEmpty())
            return;
        const int x = (a.right() + 1 + b.left()) / 2; // 间隙中点
        const int top = qMin(a.top(), b.top());
        const int bottom = qMax(a.bottom(), b.bottom());
        QPainter p(this);
        p.setPen(QPen(ThemeBridge::instance().token(QStringLiteral("separator")), 0));
        p.drawLine(x, top, x, bottom);
    }

private:
    QWidget *m_left = nullptr;
    QWidget *m_right = nullptr;
};

QWidget *buildPlaceholderTab(const QString &title, const QString &body, QWidget *parent) {
    auto *holder = new QWidget(parent);
    auto *lay = new QVBoxLayout(holder);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(12);
    lay->addWidget(buildPlaceholderCard(title, body, holder));
    lay->addStretch(1);
    return holder;
}

} // namespace

QWidget *createDownloadPage(QWidget *parent) {
    auto *page = new PageShell(QStringLiteral("下载"), QStringLiteral("官方 / 镜像双路 · 静默安装"),
                               QStringLiteral("sxclPage_download"), parent);

    // ── 双层侧边栏 + 右内容区 ──
    // 用 PageShell 的**两栏版式**:左边这一列(侧 2 栏 + 页脚)**从内容区顶部开始**,
    // 标题/副标题挪到右列上方 —— 用户 2026-09-22 晚两次点名:「侧2 还是被上方文字顶的向下移动了」。
    // 左列贴页面左边缘(左外边距 0),与窗口主侧边栏(侧1)连成一条。
    auto *leftCol = new QWidget(page->view());
    auto *leftLay = new QVBoxLayout(leftCol);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(8);

    // 第二层:与窗口左边那条**同一个组件**,所以动画/几何/选中态天然一致。
    auto *inner = new NavPanel(leftCol);
    // 与版本选择页那条同名口径:给这层侧栏一个**能找到的名字**(dump / 验收按它认这一层)。
    inner->setObjectName(QStringLiteral("sxclDownloadNav"));
    const NavItem kDownloadNav[] = {
        {QStringLiteral("download_mc"), QString(), QStringLiteral("vanilla"),
         QStringLiteral("Minecraft 版本"), false, -1},
        {QStringLiteral("download_mod"), QString(), QString(),
         QStringLiteral("MOD"), false, int(IconRegistry::Mod)},
        {QStringLiteral("download_shader"), QString(), QString(),
         QStringLiteral("光影"), false, int(IconRegistry::Shader)},
    };
    for (const NavItem &item : kDownloadNav) {
        inner->addItem(item);
    }
    inner->setCurrent(QStringLiteral("download_mc"));
    // 进页面就把那套展开动画演一遍(用户点名"要包含完整的动画"):48 -> 322 / 150ms / OutQuad。
    // 之后三横菜单照常折叠/展开,与主侧边栏一模一样。
    inner->setCollapsed(false);
    leftLay->addWidget(inner, 1); // 侧栏吃掉竖直方向的余量

    // ── 侧栏页脚:版本形态(Java 版 / 基岩版)**两个图标点选** ──
    //
    // 改前:一个 SwitchButton 滑块 + 旁边两个纯文字标签,滑块还把标签文字在"Java 版 / 基岩版"
    // 之间来回改,点了基岩版再弹一条"基岩版还没接入"的 InfoBar —— 用户 2026-09-26 点名这套
    // 形态「非常诡异啊」:要的是**咖啡杯 / 基岩方块两个图标本身可点**。
    // 改后照 docs/27 §11「按钮最少化」+ §11.5「文字纪律」:
    //   * 动作给图标**(透明底,悬停一档提亮)**,文字只进 tooltip,不占版面、不写废话;
    //   * 当前态用**主题令牌**表达(选中 = 图标下方一条 2 逻辑像素的 accent 指示条);
    //   * 点选只做一件事:写 game.edition 并立即存盘(重启后记得住),不弹任何解释性弹窗。
    {
        auto *foot = new CardWidget(leftCol);
        auto *fl = new QVBoxLayout(foot);
        fl->setContentsMargins(12, 8, 12, 8);
        fl->setSpacing(6);
        auto *rowHost = new EditionPickRow(foot);
        rowHost->setObjectName(QStringLiteral("sxclEditionPickRow"));
        auto *row = new QHBoxLayout(rowHost);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(12); // 两颗图标之间留 12:竖直分隔线就画在这段的中点

        auto *javaBtn = new IconSelectButton(rowHost);
        javaBtn->setObjectName(QStringLiteral("sxclEditionJavaButton")); // dump/验收按它认这个钮
        javaBtn->setIconFile(QStringLiteral("java_logo.svg")); // 官方咖啡杯 = Java 版
        // 视觉高度:绘制盒高 20 逻辑像素(杯子墨迹正好铺满这个盒 = 墨迹 20 逻辑像素高、
        // 14.67 逻辑像素宽,实测见 tools/ui_edition_icons.ps1 的读数)
        javaBtn->setIconHeight(20);
        javaBtn->setToolTip(QStringLiteral("Java 版"));

        auto *bedrockBtn = new IconSelectButton(rowHost);
        bedrockBtn->setObjectName(QStringLiteral("sxclEditionBedrockButton"));
        bedrockBtn->setIconFile(QStringLiteral("bedrock_logo.png")); // 官方 MINECRAFT 标题 LOGO
        // 视觉高度:LOGO 裁完画布后是 6.95:1 的宽幅图(1898x273,画布高就是字母墨迹高),
        // 这里给的是**绘制盒高度**,宽度由控件按素材横纵比算(20 -> 139 逻辑像素)。
        // 给 20 = 与咖啡杯那枚的墨迹同高:两侧墨迹实测都是 30 物理像素高(20 逻辑像素 @dpr1.5),
        // 见 tools/ui_edition_icons.ps1 的"ink height"读数。
        bedrockBtn->setIconHeight(20);
        // 未接入这件事**只在这里说**(tooltip = 悬停才出现,不占版面);
        // 以前那条"基岩版还没接入"的 InfoBar 是弹在脸上的废话,已删。
        bedrockBtn->setToolTip(QStringLiteral("基岩版\n还没接入：下载与启动目前只有 Java 版"));

        // 二选一:同一个父控件上的 autoExclusive —— 点一个,另一个自己弹起来(单选语义)
        javaBtn->setAutoExclusive(true);
        bedrockBtn->setAutoExclusive(true);
        row->addWidget(javaBtn, 0, Qt::AlignVCenter);
        row->addWidget(bedrockBtn, 0, Qt::AlignVCenter);
        rowHost->setButtons(javaBtn, bedrockBtn);

        // 页脚里**整组水平居中**(用户 2026-09-26 最终口径):前后各一个伸缩项,组里的两颗图标
        // 之间有那条 1 设备像素的竖线。容器 #sxclEditionPickRow 的中心 = 页脚内容区的中心,
        // 验收按 dump 里"组中心 x - 容器中心 x <= 1 逻辑像素"核对。
        auto *outer = new QHBoxLayout();
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addStretch(1);
        outer->addWidget(rowHost, 0, Qt::AlignVCenter);
        outer->addStretch(1);
        fl->addLayout(outer);

        const QString edition = currentEdition();
        javaBtn->setChecked(edition != QLatin1String("bedrock"));
        bedrockBtn->setChecked(edition == QLatin1String("bedrock"));

        // 点选 = 落盘 + 立刻刷新两个钮的选中态。状态只有一处真相(设置文件),两个钮都从它派生;
        // QAbstractButton 的 clicked 不区分"用户点的"与"程序触发的",所以统一走这一个入口。
        auto selectEdition = [javaBtn, bedrockBtn](const QString &next) {
            const QByteArray path = uiSettingsFilePath().toUtf8();
            if (sxcl_settings *st = sxcl_settings_open(path.constData())) {
                sxcl_settings_set(st, "game.edition", next.toUtf8().constData());
                sxcl_settings_save(st, path.constData()); // 必须存盘,否则"记住"是假的
                sxcl_settings_free(st);
            }
            const bool bedrock = next == QLatin1String("bedrock");
            javaBtn->setChecked(!bedrock);
            bedrockBtn->setChecked(bedrock);
        };
        QObject::connect(javaBtn, &QAbstractButton::clicked, foot,
                         [selectEdition]() { selectEdition(QStringLiteral("java")); });
        QObject::connect(bedrockBtn, &QAbstractButton::clicked, foot,
                         [selectEdition]() { selectEdition(QStringLiteral("bedrock")); });
        leftLay->addWidget(foot, 0);

        /* 折叠时**把页脚藏起来**:它比折叠后的轨道(48)宽得多,留着就会把这一列撑到 247 宽,
         * 折起来以后右边空一大片(用户 2026-09-22 晚:「下载页侧2栏收回来就不要靠[右]了,
         * 跟原生一样靠回最左边」)。展开时再放回来 —— 与 qf 的折叠语义一致:折叠只留图标轨道。 */
        QObject::connect(inner, &NavPanel::collapsedChanged, foot,
                         [foot](bool collapsed) { foot->setVisible(!collapsed); });
    }
    QVBoxLayout *rightLay = page->beginSideLayout(leftCol); // 右列:标题 + 副标题 + 内容

    auto *stack = new QStackedWidget(page->view());
    stack->addWidget(createVersionsPage(stack));
    // MOD 那一栏:接上 sxcl/mods.h 的资源来源层(搜索/挑文件/装进隔离后的 mods)
    stack->addWidget(createModsPage(stack, false));
    // 光影那一栏:同一个组件(kind=shaderpacks;搜索还只在模组那一类上,下一轮补 project_type)
    stack->addWidget(createModsPage(stack, true));
    stack->setCurrentIndex(0);

    QObject::connect(inner, &NavPanel::routeChanged, stack, [stack](const QString &key) {
        if (key == QLatin1String("download_mod")) {
            stack->setCurrentIndex(1);
        } else if (key == QLatin1String("download_shader")) {
            stack->setCurrentIndex(2);
        } else {
            stack->setCurrentIndex(0);
        }
    });

    rightLay->addWidget(stack, 1);
    return page;
}

} // namespace sxcl::ui
