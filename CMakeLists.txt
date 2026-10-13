cmake_minimum_required(VERSION 3.28)
project(asio-module)
cmake_policy(SET CMP0167 NEW)

set(CMAKE_CXX_STANDARD 23)

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

# liburing ships only a pkg-config file (no CMake config package), so consume it
# through PkgConfig. vcpkg does not export PKG_CONFIG_PATH, so point pkg-config at
# the installed triplet's pkgconfig directory (prepended to any existing value).
find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH}
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
pkg_check_modules(liburing REQUIRED IMPORTED_TARGET liburing)

add_subdirectory(standalone)
add_subdirectory(server)
add_subdirectory(tests)
