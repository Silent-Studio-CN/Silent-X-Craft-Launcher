# src/ui/CMakeLists.txt —— 阶段 6:Qt6 + libqf 图形前端(地基)
#
# 只有根 CMakeLists 打开 SXCL_BUILD_UI 时才会 add_subdirectory 到这里。
# 核心库(sxcl)是纯 C,Qt 头文件一律不进那边;这个目录是唯一的 C++/Qt 侧。

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
  theme_bridge.cpp   theme_bridge.h
  icon_registry.cpp  icon_registry.h
  nav.cpp            nav.h
  main_window.cpp    main_window.h
)
qt_add_library(sxcl_ui_core STATIC ${SXCL_UI_SOURCES})
target_include_directories(sxcl_ui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# fluent 的 Qt6::Svg 是 PRIVATE(静态库只把它带进链接行),我们直接用 QSvgRenderer,
# 所以这里显式要一份 Svg 的头文件路径
target_link_libraries(sxcl_ui_core PUBLIC fluent Qt6::Svg)

# ---- 可执行 ----
qt_add_executable(sxcl-ui main.cpp)
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

  add_test(NAME sxcl_ui_smoke_test COMMAND sxcl_ui_smoke_test -platform offscreen)
  set_tests_properties(sxcl_ui_smoke_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_sxcl_qt_bin}")
endif()
