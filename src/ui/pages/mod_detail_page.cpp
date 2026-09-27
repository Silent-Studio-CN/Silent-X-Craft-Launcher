/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 模组 / 光影详情页的实现（口径见 mod_detail_page.h）。

#include "mod_detail_page.h"

#include "game_folders.h"  // applyButtonFont(与主页/选择页同一套按钮字体)

#include "libqf.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "fluent/fluent_cards.h"
#include "fluent/fluent_controls.h"
#include "fluent/fluent_labels.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QAbstractButton>
#include <QEvent>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QStringList>
#include <QVBoxLayout>

#include <cstdio>

#include "sxcl/mods.h"

namespace sxcl::ui {
namespace {

/** 点一下就叫回调的守门人：「返回」是一个**可点的字**，不是按钮（用户讨厌按钮排）。 */
class ClickFilter : public QObject {
public:
    ClickFilter(QObject *parent, std::function<void()> fn)
        : QObject(parent), m_fn(std::move(fn)) {}
    void setHandler(std::function<void()> fn) { m_fn = std::move(fn); }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::MouseButtonRelease && m_fn) {
            m_fn();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> m_fn;
};

/** 元数据那一列的一行：左边一个小字段名，右边是真的值（值空 = 这一行整行不占版面）。 */
BodyLabel *metaLabel(const QString &text, QWidget *parent) {
    auto *label = new BodyLabel(text, parent);
    label->setWordWrap(false);
    label->setTextColor(pageTokenColor("textTertiary"), pageTokenColor("textTertiary"));
    return label;
}

} // namespace

ModDetailPage::ModDetailPage(QWidget *parent)
    : PageShell(QString(), QString(), QStringLiteral("ModDetailPage"), parent) {
    /* 空标题的空壳：名字/来源由 setItem 填（同一个页面复用，点哪条写哪条）——
     * PageShell 空标题不占版面，副标题空也是一样（见 page_shell.h）。 */
    auto *head = new QWidget(view());
    head->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(0, 0, 0, 0);
    headLay->setSpacing(16);

    m_icon = new QLabel(head);
    m_icon->setObjectName(QStringLiteral("modsDetailIcon"));
    m_icon->setFixedSize(kModDetailIconSide, kModDetailIconSide);
    m_icon->setAlignment(Qt::AlignCenter);
    m_icon->setStyleSheet(QStringLiteral("QLabel { background: transparent; }"));
    headLay->addWidget(m_icon, 0, Qt::AlignTop);

    auto *meta = new QWidget(head);
    meta->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *grid = new QGridLayout(meta);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(6);

    // 来源（合并列表里"这条是谁家的"）：详情页也照写，不靠颜色猜
    grid->addWidget(metaLabel(QStringLiteral("来源"), meta), 0, 0);
    m_source = new BodyLabel(QString(), meta);
    m_source->setObjectName(QStringLiteral("modsDetailSource"));
    m_source->setWordWrap(false);
    grid->addWidget(m_source, 0, 1);

    grid->addWidget(metaLabel(QStringLiteral("开发商"), meta), 1, 0);
    m_author = new BodyLabel(QString(), meta);
    m_author->setObjectName(QStringLiteral("modsDetailAuthor"));
    m_author->setWordWrap(false);
    grid->addWidget(m_author, 1, 1);

    grid->addWidget(metaLabel(QStringLiteral("版本"), meta), 2, 0);
    m_versions = new BodyLabel(QString(), meta);
    m_versions->setObjectName(QStringLiteral("modsDetailVersions"));
    m_versions->setWordWrap(false);
    grid->addWidget(m_versions, 2, 1);

    grid->addWidget(metaLabel(QStringLiteral("下载量"), meta), 3, 0);
    m_downloads = new BodyLabel(QString(), meta);
    m_downloads->setObjectName(QStringLiteral("modsDetailDownloads"));
    m_downloads->setWordWrap(false);
    grid->addWidget(m_downloads, 3, 1);

    grid->addWidget(metaLabel(QStringLiteral("更新"), meta), 4, 0);
    m_updated = new BodyLabel(QString(), meta);
    m_updated->setObjectName(QStringLiteral("modsDetailUpdated"));
    m_updated->setWordWrap(false);
    grid->addWidget(m_updated, 4, 1);
    grid->setColumnStretch(1, 1);

    headLay->addWidget(meta, 1);
    addContent(head);

    /* ── 动作：**一个**「安装」 + 一个可点的「返回」 ──
     * 用户 2026-09-27：「详情页里放一个明显的安装动作（不是一排按钮）」——安装就这么一个;
     * 返回是**字**不是按钮（ClickFilter），谁也不会把它当成第二个动作。 */
    auto *actions = new QWidget(view());
    actions->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *actionLay = new QHBoxLayout(actions);
    actionLay->setContentsMargins(0, 0, 0, 0);
    actionLay->setSpacing(16);

    m_installButton = new PrimaryPushButton(QStringLiteral("安装"), actions);
    m_installButton->setObjectName(QStringLiteral("modsDetailInstallButton"));
    applyButtonFont(qobject_cast<QPushButton *>(m_installButton));
    m_installButton->setCursor(Qt::PointingHandCursor);
    m_installButton->setMinimumWidth(120);
    QObject::connect(m_installButton, &QAbstractButton::clicked, this, [this] {
        if (m_install) {
            m_install();
        }
    });
    actionLay->addWidget(m_installButton, 0);

    m_backLink = new BodyLabel(QStringLiteral("返回"), actions);
    m_backLink->setObjectName(QStringLiteral("modsDetailBack"));
    m_backLink->setWordWrap(false);
    m_backLink->setCursor(Qt::PointingHandCursor);
    m_backLink->setTextColor(pageTokenColor("textSecondary"), pageTokenColor("textSecondary"));
    m_backLink->installEventFilter(new ClickFilter(m_backLink, [this] {
        if (m_back) {
            m_back();
        }
    }));
    actionLay->addWidget(m_backLink, 0, Qt::AlignVCenter);
    actionLay->addStretch(1);
    addContent(actions);

    /* ── **完整介绍**（列表那一行才是省略号；这里一个字都不截） ── */
    m_description = new BodyLabel(QString(), view());
    m_description->setObjectName(QStringLiteral("modsDetailDescription"));
    m_description->setWordWrap(true); // 详情页要的就是全文：换行显示，不省略
    m_description->setTextColor(pageTokenColor("textSecondary"), pageTokenColor("textSecondary"));
    addContent(m_description);

    /* 「好几个版本都说得过去」时的可选项：默认空着（点一项 = 我要它） */
    auto *choices = new QWidget(view());
    choices->setStyleSheet(QStringLiteral("background: transparent;"));
    m_choiceBox = new QVBoxLayout(choices);
    m_choiceBox->setContentsMargins(0, 0, 0, 0);
    m_choiceBox->setSpacing(8);
    addContent(choices);

    addStretch();
}

void ModDetailPage::setItem(const ModsHitRow &row) {
    m_id = row.id;
    setTitleText(row.title);
    setSubtitleText(modsSourceDisplayName(row.source));
    m_source->setText(modsSourceDisplayName(row.source));
    m_author->setText(row.author.isEmpty() ? QString() : row.author);
    if (row.versionsMin.isEmpty()) {
        m_versions->setText(QString());
    } else if (row.versionsMin == row.versionsMax) {
        m_versions->setText(row.versionsMin);
    } else {
        m_versions->setText(QStringLiteral("%1 – %2").arg(row.versionsMin, row.versionsMax));
    }
    m_downloads->setText(QStringLiteral("%1次下载").arg(modsDownloadText(row.downloads)));
    m_updated->setText(modsUpdatedText(row.updated, QDateTime::currentDateTimeUtc()));
    m_description->setText(row.description); // 全文，不截
    setInstalling(false);
    clearFileChoices();
    m_icon->clear(); // 图标由模组页那一栏按同一条缓存下回来（拉不到就一直空着）
}

void ModDetailPage::setInstalling(bool installing) {
    if (m_installButton == nullptr) {
        return;
    }
    m_installButton->setEnabled(!installing);
    m_installButton->setText(installing ? QStringLiteral("正在装…") : QStringLiteral("安装"));
}

void ModDetailPage::clearFileChoices() {
    if (m_choiceBox == nullptr) {
        return;
    }
    while (QLayoutItem *item = m_choiceBox->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }
}

void ModDetailPage::setFileChoices(const std::vector<sxcl_mod_file> &files, size_t count) {
    clearFileChoices();
    if (m_choiceBox == nullptr || count == 0) {
        return;
    }
    auto *hint = new BodyLabel(QStringLiteral("没有正好匹配这个实例的文件，点一项装："), view());
    hint->setWordWrap(true);
    hint->setTextColor(pageTokenColor("textTertiary"), pageTokenColor("textTertiary"));
    m_choiceBox->addWidget(hint);

    const QColor secondary = pageTokenColor("textSecondary");
    for (size_t i = 0; i < count; ++i) {
        const sxcl_mod_file copy = files[i];
        auto *card = new CardWidget(view());
        card->setObjectName(QStringLiteral("modsFileChoiceCard")); // 验收钩子按它点
        card->setCursor(Qt::PointingHandCursor);
        card->setMinimumHeight(48);
        auto *lay = new QHBoxLayout(card);
        lay->setContentsMargins(16, 8, 16, 8);
        QString meta = QString::fromUtf8(copy.game_versions);
        if (meta.size() > 72) {
            meta = meta.left(72) + QStringLiteral("…");
        }
        auto *text = new BodyLabel(QStringLiteral("%1 · %2 · %3")
                                       .arg(QString::fromUtf8(copy.filename), meta,
                                            QString::fromUtf8(copy.loaders)),
                                   card);
        text->setTextColor(secondary, secondary);
        text->setWordWrap(true);
        lay->addWidget(text, 1);
        QObject::connect(card, &CardWidget::clicked, this, [this, copy] {
            if (m_pickFile) {
                m_pickFile(copy);
            }
        });
        m_choiceBox->addWidget(card);
    }
}

} // namespace sxcl::ui
