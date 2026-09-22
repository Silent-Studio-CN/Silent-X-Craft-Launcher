/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 下载页(用户 2026-09-22 口述)。布局:左侧再分一栏(左 2 栏),三个入口 ——
//   草方块 · Minecraft 版本   (现有 versions_page 整页搬进来,标题改 "Minecraft 版本")
//   拼图   · MOD             (本轮只做壳;按 docs/22 的 PCL 线路做)
//   太阳   · 光影            (同上)
// 图标全部来自 PCL 自己的资源,出处照 assets/icons/pcl/index.tsv:
//   草方块 = assets/icons/blocks/Grass.png(Python BLOCK_FILES["vanilla"])
//   拼图   = assets/icons/pcl/mod.svg      (index.tsv:15 原名称「Mod」,来源 PageDownloadLeft.xaml)
//   太阳   = assets/icons/pcl/shader.svg   (index.tsv:19 原名称「光影包」,同一 XAML)
//
// 基岩版切换开关:**用户要求预留、位置待他确认**,本文先放这一页顶部右侧(语义最贴近),
// 只写状态键 game.edition,点了如实说"还没接入"(不假装有功能)。

#include "page_factory.h"
#include "page_shell.h"

#include "../icon_registry.h"
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

#include <QButtonGroup>
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

// 左栏的一个入口:图标 + 文字,互斥可选中(QRadioButton 就是 qf 的选中语义)。
RadioButton *buildRailEntry(const QIcon &icon, const QString &text, QWidget *parent) {
    auto *btn = new RadioButton(text, parent);
    btn->setIcon(icon);
    btn->setIconSize(QSize(20, 20));
    btn->setMinimumHeight(40);
    return btn;
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
        bedrock->setEnabled(true);
        QObject::connect(bedrock, &QRadioButton::clicked, row, [bedrock] {
            // 状态键先落盘(状态保留的规矩:界面能点到的开关必须存得住),功能之后再接。
            if (sxcl_settings *st = sxcl_settings_open(uiSettingsFilePath().toUtf8().constData())) {
                sxcl_settings_set(st, "game.edition", "bedrock");
                sxcl_settings_free(st);
            }
            InfoBar::push(InfoBar::Type::Info, QStringLiteral("基岩版还没接入"),
                          QStringLiteral("这里先把开关占住（状态会记住）。基岩版要用 Rust+C 那条线，"
                                         "现在的下载与启动都只有 Java 版。"),
                          bedrock, 4000);
        });
        lay->addWidget(bedrock, 0, Qt::AlignVCenter);
        page->addContent(row);
    }

    // ── 左 2 栏 + 右内容区 ──
    auto *body = new QWidget(page->view());
    auto *bodyLay = new QHBoxLayout(body);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(16);

    auto *rail = new QWidget(body);
    rail->setFixedWidth(210);
    auto *railLay = new QVBoxLayout(rail);
    railLay->setContentsMargins(0, 0, 0, 0);
    railLay->setSpacing(8);

    auto *group = new QButtonGroup(rail);
    group->setExclusive(true);

    auto *stack = new QStackedWidget(body);

    // 1) 草方块 · Minecraft 版本 —— 现有 versions_page 整页
    auto *mcEntry = buildRailEntry(SxclIcons::instance().blockIcon(QStringLiteral("vanilla"), 20),
                                   QStringLiteral("Minecraft 版本"), rail);
    railLay->addWidget(mcEntry);
    group->addButton(mcEntry, 0);
    stack->addWidget(createVersionsPage(stack));

    // 2) 拼图 · MOD
    auto *modEntry = buildRailEntry(IconRegistry::instance().themedIcon(IconRegistry::Mod),
                                    QStringLiteral("MOD"), rail);
    railLay->addWidget(modEntry);
    group->addButton(modEntry, 1);
    {
        auto *holder = new QWidget(stack);
        auto *lay = new QVBoxLayout(holder);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(12);
        lay->addWidget(buildPlaceholderCard(
            QStringLiteral("模组（MOD）"),
            QStringLiteral("这一栏会做：按 MC 版本 + 加载器筛选、一键装进 versions/<实例>/mods、"
                           "同名先拦下让你改名、依赖只列不自动装（口径见 docs/22：走 PCL 线路）。\n"
                           "现在还没开工 —— 前置是「版本隔离」（docs/22 的 A2，现在是死开关）。"),
            holder));
        lay->addStretch(1);
        stack->addWidget(holder);
    }

    // 3) 太阳 · 光影
    auto *shaderEntry = buildRailEntry(IconRegistry::instance().themedIcon(IconRegistry::Shader),
                                       QStringLiteral("光影"), rail);
    railLay->addWidget(shaderEntry);
    group->addButton(shaderEntry, 2);
    {
        auto *holder = new QWidget(stack);
        auto *lay = new QVBoxLayout(holder);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(12);
        lay->addWidget(buildPlaceholderCard(
            QStringLiteral("光影（Shader）"),
            QStringLiteral("这一栏会做：装进 versions/<实例>/shaderpacks、与模组共用同一套筛选与"
                           "同名处理。现在还没开工。"),
            holder));
        lay->addStretch(1);
        stack->addWidget(holder);
    }

    railLay->addStretch(1);
    bodyLay->addWidget(rail, 0);
    bodyLay->addWidget(stack, 1);

    mcEntry->setChecked(true);
    stack->setCurrentIndex(0);
    QObject::connect(group, &QButtonGroup::idClicked, stack, &QStackedWidget::setCurrentIndex);

    page->addContent(body);
    return page;
}

} // namespace sxcl::ui
