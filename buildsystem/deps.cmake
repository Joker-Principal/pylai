# jk 优先用包管理器提供的包；没有时 FetchContent 取源码。FIND_PACKAGE_ARGS 让
# FetchContent_MakeAvailable 先试 find_package，并在 redirects 目录留下 jk-config.cmake，
# 因此随后的 find_package(jk) 在两条路径下都成立。
include(FetchContent)
FetchContent_Declare(jk
    GIT_REPOSITORY https://github.com/Joker-Principal/jk.git
    GIT_TAG v2.0.0
    GIT_SHALLOW TRUE
    FIND_PACKAGE_ARGS CONFIG)
FetchContent_MakeAvailable(jk)
find_package(jk CONFIG REQUIRED)

find_package(asio CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS asio context)
find_package(OpenSSL REQUIRED)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    set(ENV{PKG_CONFIG_PATH}
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    pkg_check_modules(liburing REQUIRED IMPORTED_TARGET liburing)
endif()
