/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 账户管理页。
//
// 用户 2026-09-27 点名:「正版账户登录完的玩家,拉取他的模型头像……放到顶栏……单击之后
// 进到账户管理页。账户管理页先留空,做到跳转那一步就可以。」
// 所以这一页现在**就是空的**:只有页名(不然用户不知道自己到了哪),没有任何卡片/文字/按钮。
// 真正的内容(切账户 / 换皮肤 / 退出登录)等用户点名再做。

#include "page_factory.h"
#include "page_shell.h"

namespace sxcl::ui {

QWidget *createAccountPage(QWidget *parent) {
    return new PageShell(QStringLiteral("账户管理"), QString(),
                         QStringLiteral("sxclPage_account"), parent);
}

} // namespace sxcl::ui
