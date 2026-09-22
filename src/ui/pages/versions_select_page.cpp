/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 版本选择页 —— **2026-09-22 界面重构**(用户口述,规格见 docs/25 §3)。
//
// 用户原话:「原来的所有安装版本都堆在主页整体砍掉。改成版本选择页。用户可以更改已安装的版本。
// 选择页布局：左侧为文件夹列表"当前文件夹"，如果有用户导入历史，也提供切其他文件夹。我们不强制
// 根目录一定是 .minecraft，如果有别的名字，"当前文件夹"就显示这个文件夹的名字。」
//
// 布局:左栏 = 文件夹列表(当前文件夹 + 探测到的其它 + 导入的历史 + "导入文件夹…"),
//      右栏 = 该文件夹下**已安装的版本**(一行一个,显示**文件夹名** + 加载器/问题,点一下即设为当前)。
//
// "版本 = versions/ 下的文件夹名"这条 PCL 概念由核心库保证:
//   启动靠目录名(实例名)+ 该目录里任意一份能解析出版本信息的 JSON + JSON 里的库/主类/资源索引,
//   与"MC 版本号"无关 —— 所以文件夹叫 114514、JSON 里的 id 也叫 114514 照样能跑。
//   右栏的元数据(加载器/能不能启动/为什么不能)全部来自 sxcl_instance_scan,与"版本页"同一份实现。

#include "page_factory.h"
#include "game_folders.h"
#include "page_shell.h"

#include "fluent_theme.h"
#include "libqf.h"
#include "main_window.h"

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
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <cstring>

#include "sxcl/instance.h"

namespace sxcl::ui {
namespace {

class SelectPage : public PageShell {
public:
    explicit SelectPage(QWidget *parent)
        : PageShell(QStringLiteral("版本选择"),
                    QStringLiteral("一个原版可以装无数个实例 · 认的是文件夹名，不是 MC 版本号"),
                    QStringLiteral("sxclPage_select"), parent) {
        m_gameDir = resolveGameDirectory();
        buildBody();
        reload();
    }

private:
    void buildBody() {
        auto *body = new QWidget(view());
        auto *lay = new QHBoxLayout(body);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(16);

        // ── 左栏:文件夹列表 ──
        auto *left = new CardWidget(body);
        left->setFixedWidth(280);
        auto *leftLay = new QVBoxLayout(left);
        leftLay->setContentsMargins(16, 16, 16, 16);
        leftLay->setSpacing(8);
        leftLay->addWidget(new StrongBodyLabel(QStringLiteral("文件夹"), left));
        m_folderHint = new BodyLabel(QString(), left);
        m_folderHint->setWordWrap(true);
        const QColor secondary = pageTokenColor("textSecondary");
        m_folderHint->setTextColor(secondary, secondary);
        leftLay->addWidget(m_folderHint);

        m_folderBox = new QWidget(left);
        m_folderLay = new QVBoxLayout(m_folderBox);
        m_folderLay->setContentsMargins(0, 0, 0, 0);
        m_folderLay->setSpacing(4);
        leftLay->addWidget(m_folderBox);
        m_folderGroup = new QButtonGroup(left);
        m_folderGroup->setExclusive(true);
        connect(m_folderGroup, &QButtonGroup::idClicked, this, [this](int id) {
            const QVector<GameFolder> folders = detectGameFolders(m_gameDir);
            if (id >= 0 && id < folders.size()) {
                m_gameDir = folders[id].path;
                setGameDirectory(m_gameDir); // 状态保留:选的文件夹要记住
                reload();
            }
        });

        leftLay->addStretch(1);
        auto *importBtn = new PushButton(QStringLiteral("导入文件夹…"), left);
        applyButtonFont(importBtn);
        connect(importBtn, &QAbstractButton::clicked, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("选择游戏文件夹（不一定要叫 .minecraft）"), m_gameDir);
            if (dir.isEmpty())
                return;
            m_gameDir = QDir::fromNativeSeparators(dir);
            setGameDirectory(m_gameDir); // 顺带进 game.known_dirs 历史
            reload();
        });
        leftLay->addWidget(importBtn);
        lay->addWidget(left, 0);

        // ── 右栏:该文件夹下的已安装版本 ──
        auto *right = new QWidget(body);
        auto *rightLay = new QVBoxLayout(right);
        rightLay->setContentsMargins(0, 0, 0, 0);
        rightLay->setSpacing(8);
        m_listHint = new BodyLabel(QString(), right);
        m_listHint->setWordWrap(true);
        m_listHint->setTextColor(secondary, secondary);
        rightLay->addWidget(m_listHint);

        auto *scroll = new ScrollArea(right);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *holder = new QWidget(scroll);
        holder->setStyleSheet(QStringLiteral("background: transparent;"));
        scroll->setWidget(holder);
        m_listLay = new QVBoxLayout(holder);
        m_listLay->setContentsMargins(0, 0, 0, 0);
        m_listLay->setSpacing(8);
        m_listLay->setAlignment(Qt::AlignTop);
        rightLay->addWidget(scroll, 1);
        lay->addWidget(right, 1);

        addContent(body);
    }

    void clearFolderRows() {
        while (QLayoutItem *item = m_folderLay->takeAt(0)) {
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
    }

    void clearVersionRows() {
        while (QLayoutItem *item = m_listLay->takeAt(0)) {
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
    }

    void reload() {
        const QVector<GameFolder> folders = detectGameFolders(m_gameDir);
        const QString currentName = QDir(m_gameDir).dirName();
        m_folderHint->setText(QStringLiteral("当前文件夹：%1")
                                  .arg(currentName.isEmpty() ? m_gameDir : currentName));
        clearFolderRows();
        // 重新建按钮组(旧的随 deleteLater 走)
        m_folderGroup->deleteLater();
        m_folderGroup = new QButtonGroup(m_folderBox);
        m_folderGroup->setExclusive(true);
        connect(m_folderGroup, &QButtonGroup::idClicked, this, [this](int id) {
            const QVector<GameFolder> list = detectGameFolders(m_gameDir);
            if (id >= 0 && id < list.size()) {
                m_gameDir = list[id].path;
                setGameDirectory(m_gameDir);
                reload();
            }
        });
        for (int i = 0; i < folders.size(); ++i) {
            const GameFolder &folder = folders[i];
            auto *row = new RadioButton(
                QStringLiteral("%1（%2 个版本）").arg(folder.name()).arg(folder.versions),
                m_folderBox);
            row->setToolTip(QStringLiteral("%1\n来源：%2").arg(QDir::toNativeSeparators(folder.path),
                                                            folder.label));
            const bool isCurrent =
                QDir(folder.path).absolutePath().compare(QDir(m_gameDir).absolutePath(),
                                                         Qt::CaseInsensitive) == 0;
            row->setChecked(isCurrent);
            m_folderGroup->addButton(row, i);
            m_folderLay->addWidget(row);
        }

        // 右栏:这个文件夹下的已安装版本(核心库扫描:加载器/能不能启动/为什么不能)
        clearVersionRows();
        sxcl_instance_list list;
        memset(&list, 0, sizeof(list));
        char err[256];
        err[0] = '\0';
        const int rc =
            sxcl_instance_scan(m_gameDir.toUtf8().constData(), nullptr, &list, err, sizeof(err));
        const QString saved = selectedVersionName();
        int shown = 0;
        if (rc == 0) {
            for (size_t i = 0; i < list.count; ++i) {
                const sxcl_instance &inst = list.items[i];
                auto *card = new CardWidget(m_listLay->parentWidget());
                card->setFixedHeight(64);
                card->setCursor(Qt::PointingHandCursor);
                auto *rowLay = new QHBoxLayout(card);
                rowLay->setContentsMargins(20, 8, 16, 8);
                rowLay->setSpacing(16);

                auto *text = new QVBoxLayout();
                text->setSpacing(2);
                const QString name = QString::fromUtf8(inst.id);
                auto *title = new BodyLabel(
                    QStringLiteral("%1%2").arg(name, name == saved ? QStringLiteral("　← 当前") : QString()),
                    card);
                {
                    QFont font = title->font();
                    font.setPixelSize(15);
                    font.setWeight(QFont::DemiBold);
                    title->setFont(font);
                }
                text->addWidget(title);
                QStringList bits;
                if (inst.summary[0] != '\0')
                    bits << QString::fromUtf8(inst.summary);
                if (inst.base_version[0] != '\0')
                    bits << QStringLiteral("原版 %1%2").arg(QString::fromUtf8(inst.base_version),
                                                          inst.base_reliable ? QString()
                                                                             : QStringLiteral("(猜的)"));
                bits << (inst.has_jar ? QStringLiteral("有 jar") : QStringLiteral("无自己的 jar"));
                if (!inst.launchable)
                    bits << QStringLiteral("不能启动：%1")
                                .arg(QString::fromUtf8(inst.problem[0] != '\0'
                                                           ? inst.problem
                                                           : sxcl_instance_problem_default_text(
                                                                 inst.problem_code)));
                auto *detail = new BodyLabel(bits.join(QStringLiteral(" · ")), card);
                const QColor secondary = pageTokenColor("textSecondary");
                detail->setTextColor(secondary, secondary);
                text->addWidget(detail);
                rowLay->addLayout(text, 1);

                if (!inst.launchable) {
                    auto *bad = new BodyLabel(QStringLiteral("⚠"), card);
                    bad->setStyleSheet(QStringLiteral("color: %1;").arg(pageTokenText("warning")));
                    rowLay->addWidget(bad, 0, Qt::AlignVCenter);
                }

                QObject::connect(card, &QWidget::customContextMenuRequested, card, [] {});
                auto *pick = new PushButton(QStringLiteral("用这个"), card);
                applyButtonFont(pick);
                const QString picked = name;
                connect(pick, &QAbstractButton::clicked, this, [this, picked] { choose(picked); });
                rowLay->addWidget(pick, 0, Qt::AlignVCenter);
                m_listLay->addWidget(card);
                ++shown;
            }
        }
        sxcl_instance_list_free(&list);

        if (shown == 0) {
            m_listHint->setText(QStringLiteral("「%1」里还没有已安装的版本（去「下载 → Minecraft 版本」装一个）")
                                    .arg(currentName.isEmpty() ? m_gameDir : currentName));
        } else {
            m_listHint->setText(QStringLiteral("这个文件夹里有 %1 个版本；点「用这个」把它设为当前版本")
                                    .arg(shown));
        }
    }

    void choose(const QString &name) {
        setSelectedVersionName(name); // game.selected_version:关掉重开也记得
        InfoBar::push(InfoBar::Type::Success, QStringLiteral("已切换当前版本"),
                      QStringLiteral("%1（目录名就是版本名，随便改不影响启动）").arg(name), window(),
                      4000);
        if (auto *mw = qobject_cast<MainWindow *>(window()))
            mw->switchToRoute(QStringLiteral("home"));
    }

    QString m_gameDir;
    QWidget *m_folderBox = nullptr;
    QVBoxLayout *m_folderLay = nullptr;
    QButtonGroup *m_folderGroup = nullptr;
    QVBoxLayout *m_listLay = nullptr;
    BodyLabel *m_folderHint = nullptr;
    BodyLabel *m_listHint = nullptr;
};

} // namespace

QWidget *createVersionsSelectPage(QWidget *parent) { return new SelectPage(parent); }

} // namespace sxcl::ui
