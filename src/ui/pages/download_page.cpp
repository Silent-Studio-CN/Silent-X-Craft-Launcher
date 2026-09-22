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

#include "../icon_registry.h"
#include "../nav.h"
#include "../sxcl_icons.h"
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
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace sxcl::ui {
namespace {

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

    // ── 顶部右侧:基岩版开关(预留;位置待用户确认)──
    {
        auto *row = new QWidget(page->view());
        auto *lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addStretch(1);
        auto *bedrock = new RadioButton(QStringLiteral("基岩版（未接入）"), row);
        QObject::connect(bedrock, &QRadioButton::clicked, row, [bedrock] {
            // 状态键先落盘(界面能点到的开关必须存得住),功能之后再接。
            const QByteArray path = uiSettingsFilePath().toUtf8();
            if (sxcl_settings *st = sxcl_settings_open(path.constData())) {
                sxcl_settings_set(st, "game.edition", "bedrock");
                sxcl_settings_free(st);
            }
            InfoBar::push(InfoBar::Type::Info, QStringLiteral("基岩版还没接入"),
                          QStringLiteral("这里先把开关占住（状态会记住）。基岩版要走 Rust+C 那条线，"
                                         "现在的下载与启动都只有 Java 版。"),
                          bedrock, 4000);
        });
        lay->addWidget(bedrock, 0, Qt::AlignVCenter);
        page->addContent(row);
    }

    // ── 双层侧边栏 + 右内容区 ──
    auto *body = new QWidget(page->view());
    auto *bodyLay = new QHBoxLayout(body);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(12);

    // 第二层:与窗口左边那条**同一个组件**,所以动画/几何/选中态天然一致。
    auto *inner = new NavPanel(body);
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
    bodyLay->addWidget(inner, 0);

    auto *stack = new QStackedWidget(body);
    stack->addWidget(createVersionsPage(stack));
    stack->addWidget(buildPlaceholderTab(
        QStringLiteral("模组（MOD）"),
        QStringLiteral("这一栏会做：按 MC 版本 + 加载器筛选、一键装进 versions/<实例>/mods、"
                       "同名先拦下让你改名、依赖只列不自动装（口径见 docs/22：走 PCL 线路）。\n"
                       "现在还没开工 —— 前置是「版本隔离」（docs/22 的 A2，现在是死开关）。"),
        stack));
    stack->addWidget(buildPlaceholderTab(
        QStringLiteral("光影（Shader）"),
        QStringLiteral("这一栏会做：装进 versions/<实例>/shaderpacks、与模组共用同一套筛选与同名处理。"
                       "现在还没开工。"),
        stack));
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

    bodyLay->addWidget(stack, 1);
    page->addContent(body);
    return page;
}

} // namespace sxcl::ui
