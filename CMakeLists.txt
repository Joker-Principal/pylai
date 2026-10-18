cmake_minimum_required(VERSION 3.28)
project(pylai)
cmake_policy(SET CMP0167 NEW)

set(CMAKE_CXX_STANDARD 23)
# 关掉 GNU 扩展（→ `-std=c++23`），与 jk 对齐：jk 对自己的 target 设了 CXX_EXTENSIONS OFF，
# 我们若保持默认的 ON（`-std=gnu++23`），clice 加载 jk 的 PCM 会因语言选项不匹配被拒
# （"GNU extensions was disabled in precompiled file … but is currently enabled"），
# 于是 `import jk;` 的 TU 编不过、索引退化。理由详见 `docs/changelog.md`（2026-10-13）。
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# jk —— Joker's C++ Kit（https://github.com/Joker-Principal/jk）
#
# 以源码方式引入（无 release、不在 vcpkg registry），供 `import jk;` 使用。
# 提供的 target：
#   joker::kit     INTERFACE，纯头文件（#include <jk/...>）
#   joker::module  CXX_MODULES，导出模块 `jk`（import jk;）——它 PUBLIC 链接 joker::kit
#
# ⚠️ BUILD_TESTING 必须先关掉：jk 的 CMakeLists 里有
#      if(BUILD_TESTING) add_subdirectory(tests) endif()
#    而它自己 include(CTest) 会把 BUILD_TESTING 默认置为 ON，不关就会连带
#    把 jk 的测试目标一起拉进来（还会嵌套一个 project(tests)）。
# ---------------------------------------------------------------------------
set(BUILD_TESTING OFF CACHE BOOL "Build the testing tree" FORCE)

include(FetchContent)
FetchContent_Declare(
    jk
    GIT_REPOSITORY https://github.com/Joker-Principal/jk.git
    # 跟 master 走（作者自己的库，仍在开发）。要可复现就换成固定 commit：
    # c985819db0bd675bc42092c5f2340d1afe607d9b
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(jk)

find_package(asio CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS asio context)
find_package(OpenSSL REQUIRED)

# 平台差异只在构建这一层分叉，代码里不写 `#ifdef`（见 docs/cross-platform.md）。
# Linux 才有 io_uring：liburing 只发 pkg-config（没有 CMake config 包），
# 且 vcpkg 不导出 PKG_CONFIG_PATH ⇒ 自己把 triplet 的 pkgconfig 目录指过去。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    set(ENV{PKG_CONFIG_PATH}
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    pkg_check_modules(liburing REQUIRED IMPORTED_TARGET liburing)
endif()

# MSVC：源码注释是 UTF-8 且含中文 ⇒ 必须 /utf-8，否则按系统代码页解析；
# /Zc:preprocessor 是 C++20 modules 与 __VA_OPT__ 的前提（与 jk 自身 target 的要求一致）。
# 放在 FetchContent 之后，免得把这些选项塞进 jk。
if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:preprocessor)
endif()

add_subdirectory(standalone)
add_subdirectory(server)
add_subdirectory(tests)
