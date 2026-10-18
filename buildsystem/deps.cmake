include(FetchContent)

set(JK_BUILD_TESTING OFF CACHE BOOL "Build jk tests")
FetchContent_Declare(
    jk
    GIT_REPOSITORY https://github.com/Joker-Principal/jk.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(jk)

find_package(asio CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS asio context)
find_package(OpenSSL REQUIRED)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    set(ENV{PKG_CONFIG_PATH}
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    pkg_check_modules(liburing REQUIRED IMPORTED_TARGET liburing)
endif()
