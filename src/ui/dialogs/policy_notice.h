/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#if defined(_MSC_VER)
#pragma warning(push, 0) // libqf 是外部依赖,头文件在 /W4 下不干净(见 libqf.h 的说明)
#endif
#include "fluent/fluent_dialog.h" // MessageBox(MaskDialogBase + 标题/正文/按钮)
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

class QWidget;

namespace sxcl::ui {

/* 启动时的一条声明(文案由用户 2026-10-22 指定,原文照抄,别改字)。
 *
 * 只走**产品路径**:任何验收钩子(SXCL_UI_ROUTE / NAV / DUMP / SHOT / LAUNCH / WINCHECK / POPUP /
 * RAILS_TEST)在场时**自动不出现** —— 否则二十多个验收脚本会全被一个模态框挡在后面。
 *   SXCL_UI_POLICY=1  强制出现(给验收用)
 *   SXCL_UI_POLICY=0  强制不出现
 */
class PolicyNoticeDialog : public MessageBox {
    Q_OBJECT
public:
    explicit PolicyNoticeDialog(QWidget *parent = nullptr);

    /** 在启动器主窗口上弹一次(窗口级模态,遮罩盖住主窗,卡片居中)。 */
    static void showFor(QWidget *window);
};

} // namespace sxcl::ui
