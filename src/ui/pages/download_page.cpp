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
// 基岩版切换开关:**用户要求预留、位置待他确认**,先放这一页顶部右侧(语义最贴近),
// 只写状态键 game.edition,点了如实说"还没接入"(不假装有功能)。

#include "page_factory.h"
#include "page_shell.h"

#include "../fluent_theme.h"
#include "../icon_registry.h"
#include "../nav.h"
#include "../sxcl_icons.h"
#include "../ui_icons.h"
#include "libqf.h"          // InfoBar(与其他页同一个入口)
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
#include <QLabel>
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

    // ── 侧栏页脚:Java 版 / 基岩版 滑块(用户点名:滑块 + 咖啡杯 + 基岩 LOGO) ──
    {
        auto *foot = new CardWidget(leftCol);
        auto *fl = new QVBoxLayout(foot);
        fl->setContentsMargins(16, 12, 16, 12);
        fl->setSpacing(6);
        auto *row = new QHBoxLayout();
        row->setSpacing(8);
        auto *javaIcon = new QLabel(foot);
        javaIcon->setPixmap(fluent::icon(QStringLiteral("Cafe"), FluentTheme::instance().isDark())
                                .pixmap(18, 18));
        javaIcon->setToolTip(QStringLiteral("Java 版"));
        row->addWidget(javaIcon, 0, Qt::AlignVCenter);
        auto *slider = new SwitchButton(QStringLiteral("Java 版"), foot);
        slider->setToolTip(QStringLiteral("Java 版 / 基岩版：二选一（切换会记住）"));
        row->addWidget(slider, 0, Qt::AlignVCenter);
        auto *bedrockIcon = new QLabel(foot);
        const QPixmap bedrockLogo = uiBedrockLogoPixmap(14);
        if (!bedrockLogo.isNull())
            bedrockIcon->setPixmap(bedrockLogo);
        else
            bedrockIcon->setText(QStringLiteral("基岩版"));
        bedrockIcon->setToolTip(QStringLiteral("基岩版"));
        row->addWidget(bedrockIcon, 0, Qt::AlignVCenter);
        row->addStretch(1);
        fl->addLayout(row);

        const QString edition = currentEdition();
        slider->setChecked(edition == QLatin1String("bedrock"));
        slider->setText(slider->isChecked() ? QStringLiteral("基岩版") : QStringLiteral("Java 版"));
        QObject::connect(slider, &SwitchButton::checkedChanged, foot, [slider, foot](bool on) {
            // 状态键落盘(界面能点到的开关必须存得住),功能之后再接 —— 基岩版走 Rust+C 那条线。
            const QByteArray path = uiSettingsFilePath().toUtf8();
            if (sxcl_settings *st = sxcl_settings_open(path.constData())) {
                sxcl_settings_set(st, "game.edition", on ? "bedrock" : "java");
                sxcl_settings_save(st, path.constData()); // 必须存盘,否则"记住"是假的
                sxcl_settings_free(st);
            }
            slider->setText(on ? QStringLiteral("基岩版") : QStringLiteral("Java 版"));
            if (on) {
                InfoBar::push(InfoBar::Type::Info, QStringLiteral("基岩版还没接入"),
                              QStringLiteral("开关已经记住（game.edition=bedrock）。基岩版要走 Rust+C "
                                             "那条线，现在的下载与启动都只有 Java 版。"),
                              foot, 4500);
            }
        });
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
