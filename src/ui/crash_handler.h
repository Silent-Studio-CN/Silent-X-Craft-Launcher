/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

namespace sxcl::ui {

/* 启动器**自己**崩了的时候，得留一份能看的东西（docs/20 只管游戏那边的崩溃取证）。
 *
 * 为什么必须有（2026-09-23 用户报障）：用户说"切主题色窗口直接崩溃"，
 * 而我们去查的时候手上什么都没有 —— 没有 dump、没有栈、事件日志里也没有应用错误，
 * 因为进程是**直接死掉**的（未处理异常默认就是静默终止）。这种"只能靠猜"的状态
 * 在竞品对照里是最不能接受的：用户抓不到把柄，我们自己也没法修。
 *
 * 装上去之后：未处理异常 / C++ 异常逃逸 / QtFatalMsg / abort，一律先落两样东西到
 * <配置目录>/logs/crashes/：
 *   * sxcl-ui-crash-<时间>.txt —— 异常码 + 出错模块与偏移 + 调用栈（符号化）+
 *     本次运行的日志文件路径（时间线在那边）；
 *   * sxcl-ui-crash-<时间>.dmp —— 同一现场的 minidump（要更细再用调试器开）。
 * 报告里**不写任何凭据**：只用系统给的地址与模块名。
 *
 * 只在 Windows 上装（安卓/macOS/Linux 上这个函数是空操作，各自平台该用各自的手段）。
 */
void installCrashHandler();

} // namespace sxcl::ui
