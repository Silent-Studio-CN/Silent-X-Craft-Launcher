# 本目录的源文件由这里追加到 sxcl 目标(见根 CMakeLists 的说明)。
# 子代理请只改这个文件,不要动根 CMakeLists。

target_sources(sxcl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/io_internal.c
  ${CMAKE_CURRENT_SOURCE_DIR}/paths.c
  ${CMAKE_CURRENT_SOURCE_DIR}/http.c
  ${CMAKE_CURRENT_SOURCE_DIR}/instance.c
  ${CMAKE_CURRENT_SOURCE_DIR}/sysinfo.c
  # 国际化(i18n)查表:与 http.c / sysinfo.c 一样是本目录里的"通用核心工具"。
  # 内置语言表 lang_table.inc 由 Python 版 config/lang/*.lang 生成(见文件头)。
  ${CMAKE_CURRENT_SOURCE_DIR}/lang.c
)

if(SXCL_BUILD_TESTS)
  # 根 CMakeLists 是在 add_subdirectory 之后才 enable_testing() 的,所以本目录(在它之前
  # 被加进来)必须自己再开一次,否则 add_test 进不了 CTestTestfile.cmake(实测:ctest 看不到)。
  enable_testing()
  # 夹具写在构建目录下(WORKING_DIRECTORY 就是本目录的构建目录,已被 .gitignore 忽略:
  # 仓库里不留任何二进制/临时夹具)。
  add_executable(sxcl_instance_test tests/instance_test.c)
  target_include_directories(sxcl_instance_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(sxcl_instance_test PRIVATE sxcl)
  sxcl_warnings(sxcl_instance_test)
  add_test(NAME sxcl_instance_test COMMAND sxcl_instance_test)
  set_tests_properties(sxcl_instance_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  # Android 适配层:挂载表解析 + 路径体检 + 安卓候选表/诊断
  # 夹具是一棵假的"共享存储"树,夹具用的是设备 /proc/self/mounts 的**原文**。
  add_executable(sxcl_android_test tests/android_test.c)
  target_include_directories(sxcl_android_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(sxcl_android_test PRIVATE sxcl)
  sxcl_warnings(sxcl_android_test)
  add_test(NAME sxcl_android_test COMMAND sxcl_android_test)
  set_tests_properties(sxcl_android_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  # 游戏目录候选表 / 判定 / 排序:不联网。候选表是**纯字符串 + 可注入 env**,
  # 所以一台 Windows 机器上也能验 Linux/macOS/安卓三套规则;实例家族的 instances/ 展开
  # 与"像不像 MC 目录"的判据用夹具目录实测(夹具建在构建目录下,仓库里不留东西)。
  add_executable(sxcl_paths_roots_test tests/paths_roots_test.c)
  target_include_directories(sxcl_paths_roots_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(sxcl_paths_roots_test PRIVATE sxcl)
  sxcl_warnings(sxcl_paths_roots_test)
  add_test(NAME sxcl_paths_roots_test COMMAND sxcl_paths_roots_test)
  set_tests_properties(sxcl_paths_roots_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  # 国际化:不联网。夹具直接用 Python 版那份真实语言包(config/lang/*.lang)逐条对拍,
  # 证明"格式逐字段兼容";那个目录不存在(只 checkout 了 C 版)时测试自动跳过那一节。
  add_executable(sxcl_lang_test ${CMAKE_SOURCE_DIR}/tests/lang_test.c)
  target_include_directories(sxcl_lang_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(sxcl_lang_test PRIVATE sxcl)
  sxcl_warnings(sxcl_lang_test)
  add_test(NAME sxcl_lang_test COMMAND sxcl_lang_test)
  set_tests_properties(sxcl_lang_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set(_sxcl_pylang_dir "${CMAKE_SOURCE_DIR}/../Silent-X-Craft-Launcher/config/lang")
  if(EXISTS "${_sxcl_pylang_dir}/en-us.lang")
    target_compile_definitions(sxcl_lang_test PRIVATE
      SXCL_LANG_FIXTURE_DIR="${_sxcl_pylang_dir}")
  endif()
endif()
