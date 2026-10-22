/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "policy_notice.h"

#if defined(_MSC_VER)
#pragma warning(push, 0) // libqf 头文件在 /W4 下不干净(与 auth_dialog.h 同一口径)
#endif
#include "fluent/fluent_controls.h" // PrimaryPushButton(yesButton 的实体类型)
#include "fluent/fluent_labels.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QSize>
#include <QTimer>

#include <cstdio>

namespace sxcl::ui {
namespace {

/* 文案(用户 2026-10-22 原话,一个字都别改) */
const char *const kNoticeText =
    "为遵守相关法律法规，SXCL将继续提供MC下载，启动，MOD下载等功能。";

/* 验收钩子在场吗(who 是"这条脚本自己的"钩子,不该被声明挡住)。 */
bool acceptanceHookActive() {
    static const char *const kHooks[] = {
        "SXCL_UI_ROUTE",     "SXCL_UI_NAV",      "SXCL_UI_DUMP",     "SXCL_UI_SHOT",
        "SXCL_UI_LAUNCH",    "SXCL_UI_WINCHECK", "SXCL_UI_POPUP",    "SXCL_UI_RAILS_TEST",
        "SXCL_UI_SETTINGS",  "SXCL_UI_MODS_QUERY", "SXCL_UI_MODS_ROW", "SXCL_UI_MODS_INSTALL",
        "SXCL_UI_CATEGORY",  "SXCL_UI_LIST",     "SXCL_UI_AUTH_DIALOG",
    };
    for (const char *name : kHooks) {
        if (!qEnvironmentVariableIsEmpty(name))
            return true;
    }
    return false;
}

} // namespace

PolicyNoticeDialog::PolicyNoticeDialog(QWidget *parent)
    : MessageBox(QStringLiteral("SXCL"), QString::fromUtf8(kNoticeText), parent) {
    setObjectName(QStringLiteral("sxclPolicyNotice"));
    hideCancelButton();
    if (auto *ok = yesButton(); ok != nullptr) {
        ok->setText(QStringLiteral("我知道了"));
        ok->setMinimumWidth(104);
        ok->setFixedHeight(32);
    }
    setClosableOnMaskClicked(false);
    setDraggable(false);
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);
    connect(this, &MessageBox::yesSignal, this, [this] { accept(); });

    /* 卡片按文案给足宽度、文字按自己需要给足高度(libqf 的 MessageBox 只按"折成几行"算,
     * 折行后高度不会自己长 —— 实测 23px 高 vs 需要 38px 就是被裁)。 */
    if (QWidget *card = widget()) {
        /* 固定宽(不是 minimum):libqf 的 MessageBox 用 AlignCenter 摆卡片,只认 sizeHint,
         * 给 minimum 会被忽略(实测设了 520 还是 287) —— 文案会挤成两行、卡片窄得像条。 */
        card->setFixedWidth(460);
    }
    for (QLabel *label : findChildren<QLabel *>()) {
        if (label == nullptr)
            continue;
        label->setWordWrap(true);
        /* 正文那一行要**一行放得下**(用户要的是"一句话声明",断成两行看着像被挤了):
         * 直接给它一个够宽的宽度下限,卡片会跟着长(qlayout 的 SetMinimumSize 约束)。 */
        if (label->objectName() == QLatin1String("contentLabel"))
            label->setMinimumWidth(560);
        label->setMinimumHeight(label->sizeHint().height());
    }
}

void PolicyNoticeDialog::showFor(QWidget *window) {
    if (window == nullptr || !window->isVisible())
        return;
    const QString forced = qEnvironmentVariable("SXCL_UI_POLICY").trimmed();
    if (forced == QLatin1String("0")) {
        std::fprintf(stderr, "[sxcl-ui] policy-notice: suppressed (SXCL_UI_POLICY=0)\n");
        return;
    }
    if (forced != QLatin1String("1") && acceptanceHookActive()) {
        std::fprintf(stderr, "[sxcl-ui] policy-notice: skipped (acceptance hook active)\n");
        return;
    }
    auto *dialog = new PolicyNoticeDialog(window);
    /* 摆在**启动器自己窗口**的正中间(与登录窗同一条口径:遮罩正好盖满主窗,卡片居中),
     * 不是"飘到屏幕中间"。 */
    const QRect frame(window->mapToGlobal(QPoint(0, 0)), window->size());
    dialog->setGeometry(frame);
    dialog->show();
    /* 布局完成后的客观读数(不是推测):卡片尺寸、遮罩是否盖满主窗、每个文字标签的
     * (实际尺寸 vs sizeHint) —— 用来证明文案没有被裁。 */
    QTimer::singleShot(250, dialog, [dialog, window] {
        QWidget *mask = dialog->windowMask();
        const QRect host(window->mapToGlobal(QPoint(0, 0)), window->size());
        std::fprintf(stderr,
                     "[sxcl-ui] policy-notice: pos=%d,%d host=%d,%d center=%d,%d card=%dx%d mask=%dx%d "
                     "parent=%dx%d\n",
                     dialog->x(), dialog->y(), host.x(), host.y(),
                     dialog->x() + dialog->width() / 2, dialog->y() + dialog->height() / 2,
                     dialog->widget() != nullptr ? dialog->widget()->width() : 0,
                     dialog->widget() != nullptr ? dialog->widget()->height() : 0,
                     mask != nullptr ? mask->width() : 0, mask != nullptr ? mask->height() : 0,
                     window->width(), window->height());
        std::fprintf(stderr,
                     "[sxcl-ui] policy-notice: layout card=%dx%d mask=%dx%d parent=%dx%d\n",
                     dialog->widget() != nullptr ? dialog->widget()->width() : 0,
                     dialog->widget() != nullptr ? dialog->widget()->height() : 0,
                     mask != nullptr ? mask->width() : 0, mask != nullptr ? mask->height() : 0,
                     window->width(), window->height());
        /* 取证通路(SXCL_UI_SHOT):截图取完就把它关掉,否则窗口级模态框会让"关主窗即退出"
         * 走不下去,验收脚本会一直等到超时。产品路径不受影响。 */
        if (!qEnvironmentVariableIsEmpty("SXCL_UI_SHOT")) {
            const QString shot = qEnvironmentVariable("SXCL_UI_SHOT") + QStringLiteral(".policy.png");
            const bool ok = dialog->grab().save(shot);
            std::fprintf(stderr, "[sxcl-ui] policy-notice: grab %s %s\n",
                         shot.toUtf8().constData(), ok ? "OK" : "FAILED");
            QTimer::singleShot(200, dialog, [dialog] { dialog->close(); });
        }
        const QList<QLabel *> labels = dialog->findChildren<QLabel *>();
        for (QLabel *label : labels) {
            if (label == nullptr || label->text().isEmpty() || !label->isVisible())
                continue;
            const QSize hint = label->sizeHint();
            std::fprintf(stderr,
                         "[sxcl-ui] policy-notice label: %dx%d hint=%dx%d cut=%d text=%s\n",
                         label->width(), label->height(), hint.width(), hint.height(),
                         (label->width() < hint.width() || label->height() < hint.height()) ? 1
                                                                                          : 0,
                         label->text().toUtf8().constData());
        }
    });
}

} // namespace sxcl::ui
