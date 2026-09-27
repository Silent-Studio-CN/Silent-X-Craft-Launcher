# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)

# ---- libqf(qfluentwidgets 的 C/C++ 重写,静态库目标名 fluent)----
set(SXCL_PYQFTO_DIR "D:/SilentStudio/PyQf to C" CACHE PATH "libqf 源码目录")
if(NOT EXISTS "${SXCL_PYQFTO_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "找不到 libqf 源码:${SXCL_PYQFTO_DIR}(用 -DSXCL_PYQFTO_DIR=<dir> 指定)")
endif()

# 只编它的库,不编它的 demo / 多媒体 / 自带测试:
#   PYQFTO_BUILD_DEMO   demo 含媒体页,开着就要求多媒体模块
#   PYQFTO_MULTIMEDIA   媒体模块(ffmpeg 后端)体积大,启动器阶段 6 用不到
#   PYQFTO_BUILD_TESTS  那是 libqf 自己的黄金文件测试(工作目录在它源码树里),
#                       不该混进 SXCL 的 ctest —— 我们的 ctest 只跑自己的用例
# (CMP0077 下普通变量优先于 option(),所以这里 set 就够,不用改缓存)
set(PYQFTO_BUILD_DEMO OFF)
set(PYQFTO_MULTIMEDIA OFF)
set(PYQFTO_BUILD_TESTS OFF)
add_subdirectory("${SXCL_PYQFTO_DIR}" "${CMAKE_BINARY_DIR}/_libqf" EXCLUDE_FROM_ALL)

# ---- 界面核心层(主程序与 UI 冒烟测试共用同一份实现)----
set(SXCL_UI_SOURCES
  pages/page_factory.cpp  pages/page_factory.h
  pages/home_page.cpp     pages/versions_page.cpp
  pages/tasks_page.cpp    pages/keymap_page.cpp
  pages/multiplayer_page.cpp pages/settings_page.cpp
  # 三个临时页(会话页;不进导航,由 main_window 的会话机制按需创建)
  pages/download_config_page.cpp pages/download_progress_page.cpp pages/launch_page.cpp
  # 2026-09-22 界面重构(docs/25):下载 / 团队 / 更多 + 新页面共用的外壳
  pages/page_shell.h
  pages/game_folders.cpp    pages/game_folders.h
  pages/download_page.cpp   pages/team_page.cpp   pages/more_page.cpp
  pages/versions_select_page.cpp
  #   mods_page.cpp  下载 → 拼图/太阳(MOD/光影,docs/22 的 A1;网络全在工作线程)
  pages/mods_page.cpp
  # 启动器**自己**崩了要留现场(用户报"切主题色直接崩"时我们手上什么都没有,见 crash_handler.h)
  crash_handler.cpp        crash_handler.h
  #   tokens.*  令牌表(颜色唯一来源;外壳与每个控件都从这里**现取**)
  #   icons.*   assets/icons/ui/* 的取图(单色线稿按令牌现染;官方四色原样保留)
  #   task.*    任务原语:阻塞活进工作线程,结果由框架回到界面线程;析构先取消再回收
  #   sources.* 主页的阻塞数据源(扫版本 / 读账户 / 写设置)—— **只许 Task 的工作线程调**
  #   pages/home.*       主页:页面里只有数据与槽(不碰样式、不碰磁盘)
  #   widgets/*          当前版本大卡 / 玩家卡(悬停图标 + 版本形态点选) / 手风琴 / 公告卡
  # 动画基座(docs/27 §10.2 / §10.2.1 / §13.1):时间线编排 + 自家曲线表 + 错峰模板 + 动效开关。
  # 纯 Qt,不依赖 libqf;全局只有一个心跳 QTimer(见 anim.cpp)。
  # 会"冒出来"的自绘行:只暴露一个 reveal 属性,动画写它、paintEvent 读它(不碰布局)。
  # 工作线程层(阶段 7:下载->安装->启动 的界面接线)
  #   ui_paths.*       界面层共用的运行期取值(游戏目录/设置文件/内存/进度追踪)
  #   install_worker.* 把阻塞的 sxcl_install_run 放工作线程,阶段/进度/速度/取消回填 UI
  #   launch_worker.*  把阻塞的 sxcl_launch_run 放工作线程,Java/命令行/日志归类/退出码回填 UI
  workers/ui_paths.cpp        workers/ui_paths.h
  workers/install_worker.cpp  workers/install_worker.h
  workers/launch_worker.cpp   workers/launch_worker.h
  #   ui_error.*       统一的错误出口:完整上下文 + 自动复制剪贴板(同一错误只复制一次)
  workers/ui_error.cpp        workers/ui_error.h
  #   mods_worker.* 模组页的取文本/下文件(工作线程;模组下载不碰安装编排)
  workers/mods_worker.cpp     workers/mods_worker.h
  #   mods_merge.*  模组页"来源"那一层的纯逻辑(勾选列表的落盘/读回 + 两源结果合并/去重;
  #                 结果不跨源合并,每条自带真实来源)。纯字符串与纯数据,单测直接调它:
  #                 tests/mods_sources_test.cpp —— 不需要窗口、不需要网络
  workers/mods_merge.cpp      workers/mods_merge.h
  #   bg_task.*     通用"一次性后台任务"(阻塞代码进工作线程,结果回界面线程;界面线程零等待)
  workers/bg_task.cpp         workers/bg_task.h
  #   instance_scan.* 已安装版本扫描(sxcl_instance_scan)的界面层唯一实现;**只许工作线程调**
  workers/instance_scan.cpp   workers/instance_scan.h
  # 账户(正版登录)—— Python 版没有这个功能,新增:
  #   account.*     账户快照 + 工作线程任务(登录/续期/注销/正版启动;主线程绝不阻塞)
  #   auth_dialog.* 设备码登录对话框(qf 风格的 MessageBoxBase)
  #   auth_replay.* 夹具回放传输(取证通路,默认不启用;见头文件说明)
  dialogs/account.cpp     dialogs/account.h
  dialogs/auth_dialog.cpp dialogs/auth_dialog.h
  dialogs/auth_replay.c   dialogs/auth_replay.h
  fluent_theme.cpp   fluent_theme.h
  sxcl_icons.cpp     sxcl_icons.h
  theme_bridge.cpp   theme_bridge.h
  icon_registry.cpp  icon_registry.h
  #   ui_icons.*     界面自绘小图标（警示三角等；用户点名"emoji 换成 svg"）
  ui_icons.cpp       ui_icons.h
  #   icon_select_button.* 图标点选钮（透明底 + 悬停提亮 + 选中态走主题令牌 accent）：
  #   下载页侧2 页脚的"Java 版(咖啡杯) / 基岩版(基岩方块)"就是它（docs/27 §11 按钮最少化）
  icon_select_button.cpp icon_select_button.h
  #   folder_icons.* 文件夹自定义图标（用户点名：悬停文件夹出设置按钮，从应用内挑图标）
  folder_icons.cpp   folder_icons.h
  #   flow_layout.* 流式布局（docs/25 总则：控件必须完整显示自己，放不下**换行**、不许压扁/裁切）
  flow_layout.cpp    flow_layout.h
  nav.cpp            nav.h
  main_window.cpp    main_window.h
  #   tray.*       任务栏托盘图标:「最小化 = 隐藏窗口」后的恢复入口(新增设计,
  #                Python 版没有这套语义;三个动作只发信号,窗口状态仍由 main_window 独占)
  tray.cpp           tray.h
)
qt_add_library(sxcl_ui_core STATIC ${SXCL_UI_SOURCES})
target_include_directories(sxcl_ui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# 主题资产（qf 原版 QSS）在源码树里，编译期把目录写死，运行时按需回退到 exe 旁
target_compile_definitions(sxcl_ui_core PUBLIC SXCL_UI_THEME_DIR="${CMAKE_SOURCE_DIR}/assets/theme")
# 方块图标资产(assets/icons/blocks/*.png)
target_compile_definitions(sxcl_ui_core PUBLIC SXCL_UI_BLOCK_DIR="${CMAKE_SOURCE_DIR}/assets/icons/blocks")
# 版本形态图标(assets/icons/edition/*):官方 Java 咖啡杯 svg + 官方基岩方块 png
# (出处见 assets/icons/edition/NOTICE.md;IconSelectButton 按它取图)
target_compile_definitions(sxcl_ui_core PUBLIC SXCL_UI_EDITION_DIR="${CMAKE_SOURCE_DIR}/assets/icons/edition")
# 新界面自己的图标(assets/icons/ui/*):微软 LOGO + 断线图标 + 换皮肤/切换账号/更改名字/齿轮/箭头
# (全部自绘,出处见 assets/icons/ui/NOTICE.md;单色线稿由 IconButton 按主题令牌现染)
# fluent 的 Qt6::Svg 是 PRIVATE(静态库只把它带进链接行),我们直接用 QSvgRenderer,
# 所以这里显式要一份 Svg 的头文件路径
# sxcl = 纯 C 核心库。界面层必须能直接调它(设置读写/路径解析/实例列表…),
# 否则页面只能用 Qt 去读 Python 版的旧配置文件(主页代理踩过这个坑)。
# 核心库不依赖 Qt,不存在循环依赖。
target_link_libraries(sxcl_ui_core PUBLIC fluent Qt6::Svg sxcl)
# 登录/续期要走 HTTPS:核心库的 Qt Network 传输后端(根 CMakeLists 里按 SXCL_BUILD_QT_TRANSPORT 建)。
# 没有它时 account.cpp 会如实报"没有可用的网络后端",绝不假装登录成功。
if(TARGET sxcl_net_qt)
  target_link_libraries(sxcl_ui_core PUBLIC sxcl_net_qt)
  target_compile_definitions(sxcl_ui_core PUBLIC SXCL_UI_HAVE_QT_TRANSPORT=1)
endif()
# 核心库头文件目录:显式挂上,免得"链了库却看不到 include/sxcl/*.h"(页面代理踩过)
target_include_directories(sxcl_ui_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

# CurseForge 的官方 API key 是**编译期内置**的(用户 2026-09-26:「不要让用户自己填写」)。
#   * 默认 = include/sxcl/mods_key.h 里的空串,空串就是"这个构建里没有这一源";
#   * 拿到 key 后配置时给一个缓存变量即可,不用改源码:
#         cmake -S . -B build-ui -DSXCL_CURSEFORGE_API_KEY=<key>
#   * 这里**只挂到读这个头的那一个源文件**上:换 key 只重编 pages/mods_page.cpp 一份
#     (验收脚本 tools/ui_mods_sources.ps1 的第二档就是靠这一点:临时编一个假 key 进去,
#      验完再编回空串 —— 整个目标重编的话这一档就没法在脚本里跑)。
if(DEFINED SXCL_CURSEFORGE_API_KEY AND NOT SXCL_CURSEFORGE_API_KEY STREQUAL "")
  set_source_files_properties(pages/mods_page.cpp PROPERTIES
    COMPILE_DEFINITIONS "SXCL_CURSEFORGE_API_KEY=\"${SXCL_CURSEFORGE_API_KEY}\"")
  string(LENGTH "${SXCL_CURSEFORGE_API_KEY}" _sxcl_cf_len)
  message(STATUS "CurseForge: 内置了官方 API key(长度 ${_sxcl_cf_len},只编进 pages/mods_page.cpp)")
endif()

# ---- 可执行 ----
qt_add_executable(sxcl-ui main.cpp)
# 编进可执行文件而不是静态库:静态库里没有被引用的目标文件会被链接器丢掉,启动钩子就永远不跑。
# 成品观感:控制台子系统会在启动器旁边弹一个黑窗口(实测 PE 子系统 = 3)。
# 开发/验收阶段保留控制台 —— 工具链与各页自检都靠 stderr 看诊断;
# **打包时**用 -DSXCL_UI_WINDOWS_GUI=ON 变成 GUI 子系统(PE 子系统 = 2)。
option(SXCL_UI_WINDOWS_GUI "Windows: 把 sxcl-ui 编译为 GUI 子系统(不弹控制台窗口)" OFF)
if(WIN32 AND SXCL_UI_WINDOWS_GUI)
  set_target_properties(sxcl-ui PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_link_libraries(sxcl-ui PRIVATE sxcl_ui_core sxcl)
# 界面程序需要 platforms/tls 插件才能起窗:用 windeployqt 把 Qt 运行时铺到 exe 同目录
# (用户反馈"总是提示找不到 Qt*.dll" —— 不能靠 PATH,产物必须自带)
sxcl_deploy_qt_full(sxcl-ui)

# 警告口径与库/测试一致(工程既有的函数)。Qt/libqf 的头文件都能过 /W4 /WX,
# 过不了就把具体文件降级,不要整个目标去掉警告。
sxcl_warnings(sxcl_ui_core)
sxcl_warnings(sxcl-ui)

# ---- UI 冒烟测试(没开 UI 的构建里根本不会进这个文件,ctest 不受影响)----
if(SXCL_BUILD_TESTS)
  add_executable(sxcl_ui_smoke_test "${CMAKE_SOURCE_DIR}/tests/ui_smoke_test.cpp")
  target_link_libraries(sxcl_ui_smoke_test PRIVATE sxcl_ui_core)
  sxcl_warnings(sxcl_ui_smoke_test)
  sxcl_deploy_qt(sxcl_ui_smoke_test) # 同样:测试的 exe 旁边也要有 Qt DLL

  # Qt 的 DLL 不在 PATH 的机器上也要能跑:把 Qt 的 bin 目录并进测试环境
  get_target_property(_sxcl_qt_core Qt6::Core IMPORTED_LOCATION_RELEASE)
  if(NOT _sxcl_qt_core)
    get_target_property(_sxcl_qt_core Qt6::Core IMPORTED_LOCATION)
  endif()
  get_filename_component(_sxcl_qt_bin "${_sxcl_qt_core}" DIRECTORY)

  # 事故夹具(界面侧):一唤醒就自动续装 -> 默认只登记可点击的"上次未完成",用户点了才恢复。
  # 设置文件/游戏目录/待恢复任务文件全钉在临时目录(SXCL_UI_SETTINGS/SXCL_UI_GAME_DIR),
  # 不碰用户真实配置与游戏目录。SXCL_UI_RESUME_TASKS=1 是唯一的自动恢复钉子(用例里自己设)。
  add_executable(sxcl_ui_resume_test "${CMAKE_SOURCE_DIR}/tests/ui_resume_test.cpp")
  target_link_libraries(sxcl_ui_resume_test PRIVATE sxcl_ui_core)
  sxcl_warnings(sxcl_ui_resume_test)
  sxcl_deploy_qt(sxcl_ui_resume_test)
  add_test(NAME sxcl_ui_resume_test COMMAND sxcl_ui_resume_test -platform offscreen)
  set_tests_properties(sxcl_ui_resume_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_sxcl_qt_bin}")

  # Fabric 安装器地址验收(**要联网**):没开 SXCL_ACCEPT_NETWORK=1 时自己跳过,ctest 不受影响。
  # 手动跑:
  #   $env:SXCL_ACCEPT_NETWORK=1; .\build-ui\src\ui\Release\sxcl_fabric_installer_test.exe
  add_executable(sxcl_fabric_installer_test "${CMAKE_SOURCE_DIR}/tests/fabric_installer_test.cpp")
  target_link_libraries(sxcl_fabric_installer_test PRIVATE sxcl_ui_core)
  sxcl_warnings(sxcl_fabric_installer_test)
  sxcl_deploy_qt(sxcl_fabric_installer_test)
  add_test(NAME sxcl_fabric_installer_test COMMAND sxcl_fabric_installer_test)
  set_tests_properties(sxcl_fabric_installer_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_sxcl_qt_bin}")

  # 文件夹自定义图标（目录 / 持久化 / 回落 / 页面内选择器）—— 纯逻辑 + 控件计数，不用截图
  add_executable(sxcl_ui_folder_icons_test "${CMAKE_SOURCE_DIR}/tests/ui_folder_icons_test.cpp")
  target_link_libraries(sxcl_ui_folder_icons_test PRIVATE sxcl_ui_core)
  sxcl_warnings(sxcl_ui_folder_icons_test)
  sxcl_deploy_qt(sxcl_ui_folder_icons_test)
  add_test(NAME sxcl_ui_folder_icons_test COMMAND sxcl_ui_folder_icons_test -platform offscreen)
  set_tests_properties(sxcl_ui_folder_icons_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_sxcl_qt_bin}")

  # 模组页"来源"那一层(用户点名:两个源是**可同时勾选**的选项,结果合并成一份;
  # CF 的 key 编译期内置,不让用户填)。纯逻辑 + 纯数据,不用截图、不联网。
  add_executable(sxcl_mods_sources_test "${CMAKE_SOURCE_DIR}/tests/mods_sources_test.cpp")
  target_link_libraries(sxcl_mods_sources_test PRIVATE sxcl_ui_core)
  sxcl_warnings(sxcl_mods_sources_test)
  sxcl_deploy_qt(sxcl_mods_sources_test)
  add_test(NAME sxcl_mods_sources_test COMMAND sxcl_mods_sources_test -platform offscreen)
  set_tests_properties(sxcl_mods_sources_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_sxcl_qt_bin}")


  add_test(NAME sxcl_ui_smoke_test COMMAND sxcl_ui_smoke_test -platform offscreen)
  set_tests_properties(sxcl_ui_smoke_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_sxcl_qt_bin}")
endif()
