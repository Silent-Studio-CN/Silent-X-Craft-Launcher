#pragma once
/* auth_replay —— 登录对话框的**夹具回放传输**(取证通路,不联网)。
 *
 * 为什么需要它:docs/09 §9.2 的第 6 跳是 HTTP 403 Invalid app registration —— 那是
 * **应用资质(Mojang 户口)**问题,不是账号问题,所以"界面上原样展示这条错误"这件事
 * 没法靠真人登录复现(真人登录走到第 6 跳就停在那里,而申请资质不由我们控制)。
 *
 * 这个后端把 tests/fixtures/auth/*.json(**实测响应的脱敏副本**)当响应正文返回,
 * 走的是**核心库原封不动的登录链代码**(sxcl_auth_login → … → sxcl_auth_minecraft_login):
 * 所以对话框里显示的 403 正文,是核心库从真实记录的响应里解析出来的原文,
 * 不是界面自己编的字符串。
 *
 * 只有环境变量 SXCL_UI_AUTH_REPLAY=<夹具目录> 显式设了才会启用(见 account.cpp),
 * 默认路径完全不受影响。
 *
 * 线程:与 transport_qt 一样,**实例只属于创建它的线程**(登录在 work 线程里跑)。
 */
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
