/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include "sxcl/net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 建一个回放后端。
 *   fixture_dir  —— 夹具目录(tests/fixtures/auth)
 *   fail_at_hop6 —— 非 0:第 6 跳(login_with_xbox)返回 403 + 实测的
 *                   "Invalid app registration" 正文;0:返回 200 的成功夹具。
 * 返回 NULL = 参数不合法。销毁用 tr->destroy(tr->ctx)(与其它后端一致)。 */
sxcl_transport *sxcl_ui_auth_replay_create(const char *fixture_dir, int fail_at_hop6);

#ifdef __cplusplus
}
#endif
