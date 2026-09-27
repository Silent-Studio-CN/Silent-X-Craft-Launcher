/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// CurseForge 官方 API 的内置密钥（**编译期常量**）。
//
// 口径（用户 2026-09-26 点名）：「关于 CF 源的密钥配置，我后续给你，不要让用户自己填写。」
//   * 界面上**没有任何**让用户填 key 的入口（以前模组页顶部有一栏 key 输入框，已删）；
//   * 默认空串：空串 = 这一源在这个构建里不可用 —— 界面把 CurseForge 的勾选框**置灰**
//     （可见但点不动，tooltip 一句话说清），**不发注定失败的请求**，也绝不拿 Modrinth 的结果冒充它；
//   * 拿到官方 key 后**不用改这个文件**，配置时给 CMake 一个缓存变量即可：
//         cmake -S . -B build-ui -DSXCL_CURSEFORGE_API_KEY=<key>
//     src/ui/CMakeLists.txt 只把它加到**读这个头的那一个源文件**上（改 key 只重编那一份，
//     不是整个目标重编）；
//   * 密钥只进**请求头**（x-api-key），绝不进 URL —— URL 会被写进日志/错误消息/历史。
#pragma once

#ifndef SXCL_CURSEFORGE_API_KEY
#define SXCL_CURSEFORGE_API_KEY ""
#endif
