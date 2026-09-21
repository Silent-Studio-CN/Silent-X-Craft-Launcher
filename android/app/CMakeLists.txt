# SXCL C - Android packaging layer (SilentStudio JQt pipeline).
#
# This project lives OUTSIDE the shared source tree. It only decides WHICH
# translation units go into the Android shared library and how they link;
# every file under src/** is used verbatim (no Android forks of UI code).
#
# Layout expected next to this file (assembled by the staging script):
#   sxcl/    copy of the SXCL C source tree (root CMakeLists + src/)
#   pytoc/   copy of the libqf source tree ("PyQf to C")
#   include/ copy of the SXCL C include tree (repo root CMakeLists uses
#            ${CMAKE_SOURCE_DIR}/include; CMAKE_SOURCE_DIR is this directory)
#
cmake_minimum_required(VERSION 3.24)

project(sxcl_android VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT ANDROID)
  message(FATAL_ERROR "Android-only project: configure with the Qt android toolchain file.")
endif()

set(SXCL_SRC_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/sxcl"  CACHE PATH "SXCL C source tree copy")
set(SXCL_PYQFTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pytoc" CACHE PATH "libqf source tree copy")

# Options the two sub-projects read. Plain variables win over option() (CMP0077).
set(SXCL_BUILD_UI OFF)      # src/ui is added explicitly below (same sources, our entry)
set(SXCL_BUILD_TESTS OFF)
set(SXCL_BUILD_CLI OFF)
set(SXCL_WERROR OFF)        # keep -Wall/-Wextra, drop -Werror on the Android toolchain
set(PYQFTO_BUILD_DEMO OFF)
set(PYQFTO_MULTIMEDIA OFF)
set(PYQFTO_BUILD_TESTS OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network)

# ---- core library (pure C) -------------------------------------------------
add_subdirectory(${SXCL_SRC_DIR} ${CMAKE_BINARY_DIR}/sxcl)

# The desktop build gets AUTOMOC from the root CMakeLists (SXCL_BUILD_QT_TRANSPORT
# sets CMAKE_AUTOMOC ON before src/ui is added). We add src/ui from THIS scope, so
# we must turn it on here or tasks_page.cpp's "#include \"tasks_page.moc\"" fails.
set(CMAKE_AUTOMOC ON)

# ---- UI layer: the very same CMakeLists the desktop build uses -------------
add_subdirectory(${SXCL_SRC_DIR}/src/ui ${CMAKE_BINARY_DIR}/sxcl_ui)
# the desktop executable target is not part of the APK; it is not built
set_target_properties(sxcl-ui PROPERTIES EXCLUDE_FROM_ALL TRUE)
# ...and force AUTOMOC on the library itself (src/ui/CMakeLists.txt relies on the
# variable being set by the root CMakeLists, which only happens in its own scope).
set_target_properties(sxcl_ui_core PROPERTIES AUTOMOC ON)

# ---- Android shared library ------------------------------------------------
# sxcl_android_main.cpp #includes src/ui/main.cpp under a renamed symbol, so the
# desktop entry point is reused unchanged instead of being copied.
qt_add_executable(sxclui sxcl_android_main.cpp)
target_include_directories(sxclui PRIVATE ${SXCL_SRC_DIR}/src/ui)
target_link_libraries(sxclui PRIVATE sxcl_ui_core)
set_target_properties(sxclui PROPERTIES OUTPUT_NAME "sxclui_${ANDROID_ABI}")

message(STATUS "sxclui -> libsxclui_${ANDROID_ABI}.so (Qt ${Qt6Widgets_VERSION})")
