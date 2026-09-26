# (C) Silent X Craft Launcher —— 侧边栏叠层实验台(docs/27 §2 的实验台)
#
# 用户 2026-09-26:「给我一个测试,有 LOG 的 DEMO,点一下按钮侧边栏就加一层,看 BUG/特性」。
# 它**只依赖 Qt Widgets**(不挂 libqf、不挂 sxcl),所以谁都能编、编得快,
# 用来把"严丝合缝 + 同一时刻只伸一条"这两件事先证明出来,再照它改老界面。
add_executable(sxcl_rails_demo main.cpp)
# 挂 sxcl_ui_core:它 PUBLIC 带着 fluent 与 Qt6 的 include/链接(只写 Qt6::Widgets 时
# 编译期拿不到 Qt 头,实测 C1083 打不开 QApplication)。demo 只用 Qt 的控件,不碰 libqf 的类。
target_link_libraries(sxcl_rails_demo PRIVATE sxcl_ui_core)
set_target_properties(sxcl_rails_demo PROPERTIES WIN32_EXECUTABLE FALSE)

# **必须部署 Qt 运行时**:不然双击就是"缺少 Qt6Widgets.dll"闪退(用户 2026-09-26 直接骂了:
# 「一个运行连 dll 都缺的 exe 我怎么看」)。用工程现成的部署函数,别手工拷。
sxcl_deploy_qt(sxcl_rails_demo)
