# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

target_sources(sxcl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/util.c        # 缓冲/编码/随机数/时间/人话错误
  ${CMAKE_CURRENT_SOURCE_DIR}/crypto.c      # ChaCha20-Poly1305(RFC 8439,降级存储用)
  ${CMAKE_CURRENT_SOURCE_DIR}/pkce.c        # PKCE(RFC 7636)
  ${CMAKE_CURRENT_SOURCE_DIR}/http_auth.c   # 登录链统一 HTTP 出口(非 2xx 也读正文)
  ${CMAKE_CURRENT_SOURCE_DIR}/loopback.c    # 127.0.0.1 环回重定向服务器
  ${CMAKE_CURRENT_SOURCE_DIR}/oauth.c       # 授权码 / refresh / 设备码
  ${CMAKE_CURRENT_SOURCE_DIR}/xbox.c        # Xbox Live 用户认证 + XSTS(+ XErr 中文映射)
  ${CMAKE_CURRENT_SOURCE_DIR}/minecraft.c   # login_with_xbox / 权益 / 档案
  ${CMAKE_CURRENT_SOURCE_DIR}/bedrock.c     # 基岩链(multiplayer.minecraft.net)
  ${CMAKE_CURRENT_SOURCE_DIR}/ec384.c       # P-384 身份密钥(RFC 5903 参数)
  ${CMAKE_CURRENT_SOURCE_DIR}/store.c       # 令牌加密落盘(DPAPI/Keychain/libsecret/文件+密钥)
  ${CMAKE_CURRENT_SOURCE_DIR}/session.c     # 会话工具 + client_id/租户解析
  ${CMAKE_CURRENT_SOURCE_DIR}/auth.c        # 整链编排
)

if(WIN32)
  # ws2_32  = 环回重定向服务器的套接字
  # crypt32 = DPAPI(CryptProtectData)
  # bcrypt  = 系统 CSPRNG(BCryptGenRandom)
  # shell32 = ShellExecuteW(用默认浏览器打开授权页)
  # advapi32 = 注册表(MachineGuid,派生降级密钥用)
  target_link_libraries(sxcl PRIVATE ws2_32 crypt32 bcrypt shell32 advapi32)
elseif(APPLE)
  find_library(SXCL_SECURITY_FRAMEWORK Security REQUIRED)
  target_link_libraries(sxcl PRIVATE ${SXCL_SECURITY_FRAMEWORK})
else()
  # Linux:libsecret 走 dlopen(不引入链接期依赖),所以只需要 libdl
  target_link_libraries(sxcl PRIVATE ${CMAKE_DL_LIBS})
endif()

if(SXCL_BUILD_TESTS)
  # 根 CMakeLists 是在 add_subdirectory 之后才 enable_testing() 的,所以本目录(在它之前
  # 被加进来)必须自己再开一次,否则 add_test 进不了 CTestTestfile.cmake(实测:ctest 看不到)。
  enable_testing()
  # 测试要 include 内部头(auth_internal.h)才能对拍底层原语,所以把本目录加进搜索路径。
  set(SXCL_AUTH_TESTS
    auth_pkce_test     tests/auth_pkce_test.c      # RFC 7636 官方向量 + client_id/租户/端点 + 错误码人话
    auth_crypto_test   tests/auth_crypto_test.c    # RFC 8439 官方向量(ChaCha20/Poly1305/AEAD)
    auth_flow_test     tests/auth_flow_test.c      # 假传输 + 夹具:成功链/XSTS 错误/name 空/续期/设备码
    auth_store_test    tests/auth_store_test.c     # 加密落盘:往返、无明文、改一个字节就解不开
    auth_loopback_test tests/auth_loopback_test.c  # 真起 127.0.0.1 服务并真发一次 HTTP 回调
    auth_bedrock_test  tests/auth_bedrock_test.c   # RFC 5903 的 g^i 对拍 + 基岩链夹具
  )
  list(LENGTH SXCL_AUTH_TESTS _auth_n)
  math(EXPR _auth_last "${_auth_n} - 1")
  foreach(_i RANGE 0 ${_auth_last} 2)
    list(GET SXCL_AUTH_TESTS ${_i} _auth_name)
    math(EXPR _j "${_i} + 1")
    list(GET SXCL_AUTH_TESTS ${_j} _auth_src)
    add_executable(sxcl_${_auth_name} ${_auth_src})
    target_include_directories(sxcl_${_auth_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # 测试里要 fopen/sscanf/snprintf:必须在**任何 CRT 头之前**定义,所以走编译选项而不是写在源文件里
    target_compile_definitions(sxcl_${_auth_name} PRIVATE _CRT_SECURE_NO_WARNINGS=1)
    target_link_libraries(sxcl_${_auth_name} PRIVATE sxcl)
    sxcl_warnings(sxcl_${_auth_name})
    add_test(NAME sxcl_${_auth_name} COMMAND sxcl_${_auth_name})
    # 夹具目录:测试用 SXCL_AUTH_FIXTURES 拿到绝对路径(不依赖当前工作目录)
    set_tests_properties(sxcl_${_auth_name} PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      ENVIRONMENT "SXCL_AUTH_FIXTURES=${CMAKE_SOURCE_DIR}/tests/fixtures/auth")
  endforeach()
endif()
