/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

/* 新界面(M1 骨架)入口。老界面(src/ui)一行不删、照旧能跑:
 *   SXCL_UI2=1 时 main.cpp 走这一条,否则还是老的 MainWindow。
 * 方案与口径见 docs/27-UI重写方案.md(§1 令牌 / §2 外壳 / §10.2 动画 / §11 交互原则)。 */
namespace sxcl::ui2 {

/** 起新外壳。返回进程退出码(main 直接 return 它)。 */
int run(int argc, char **argv);

} // namespace sxcl::ui2
