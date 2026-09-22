# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

target_sources(sxcl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/catalog.c
  ${CMAKE_CURRENT_SOURCE_DIR}/processors.c
  ${CMAKE_CURRENT_SOURCE_DIR}/keymap.c
  ${CMAKE_CURRENT_SOURCE_DIR}/keymap_presets.c
  ${CMAKE_CURRENT_SOURCE_DIR}/keymap_fcl.c
  ${CMAKE_CURRENT_SOURCE_DIR}/keymap_store.c
)

if(SXCL_BUILD_TESTS)
  # 根 CMakeLists 是在 add_subdirectory 之后才 enable_testing() 的,所以本目录(在它之前
  # 被加进来)必须自己再开一次,否则 add_test 进不了 CTestTestfile.cmake(ctest 看不到)。
  enable_testing()

  # 加载器版本目录:不联网。真实响应片段在 tests/catalog_fragments.inc(纯文本,注释里有来源与抓取日期)
  add_executable(sxcl_loader_catalog_test tests/loader_catalog_test.c)
  target_include_directories(sxcl_loader_catalog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(sxcl_loader_catalog_test PRIVATE sxcl)
  sxcl_warnings(sxcl_loader_catalog_test)
  add_test(NAME sxcl_loader_catalog_test COMMAND sxcl_loader_catalog_test)
  set_tests_properties(sxcl_loader_catalog_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  # 键位映射核心:9 套安卓端资产逐字节嵌在 tests/keymap_assets.inc 里,所以同样不联网;
  # 测试自己往当前构建目录写临时布局(仓库里不留夹具)。
  add_executable(sxcl_keymap_test tests/keymap_test.c)
  target_include_directories(sxcl_keymap_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(sxcl_keymap_test PRIVATE sxcl)
  sxcl_warnings(sxcl_keymap_test)
  add_test(NAME sxcl_keymap_test COMMAND sxcl_keymap_test)
  set_tests_properties(sxcl_keymap_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  # 安卓端资产目录就在隔壁仓库里(存在才定义):测试会把内嵌片段与磁盘上的真实文件逐字节比对,
  # 防止两边跑偏;换台机器没有这个目录也不影响编译与运行。
  # 布局存取(store.py)+ FCL 一致性(fcl.py):临时产物写在构建目录,不碰用户真实配置;
  # FCL 夹具与"Python 生成的期望文件"在 tests/ 下(tools/keymap_parity.py --write-fixtures 生成),
  # 路径用 SXCL_KEYMAP_FIXTURE_DIR 编进去,ctest 才能与 Python 版逐字段比对。
  add_executable(sxcl_keymap_store_test tests/keymap_store_test.c)
  target_include_directories(sxcl_keymap_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(sxcl_keymap_store_test PRIVATE sxcl)
  sxcl_warnings(sxcl_keymap_store_test)
  target_compile_definitions(sxcl_keymap_store_test PRIVATE
    SXCL_KEYMAP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME sxcl_keymap_store_test COMMAND sxcl_keymap_store_test)
  set_tests_properties(sxcl_keymap_store_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  set(_sxcl_android_keymaps "${CMAKE_SOURCE_DIR}/../Silent-X-Craft-Launcher/android/app/src/main/assets/keymaps")
  if(EXISTS "${_sxcl_android_keymaps}")
    target_compile_definitions(sxcl_keymap_test PRIVATE
      SXCL_ANDROID_KEYMAPS_DIR="${_sxcl_android_keymaps}")
  endif()
endif()
