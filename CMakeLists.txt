cmake_minimum_required(VERSION 3.28)
project(pylai VERSION 0.2.0)
cmake_policy(SET CMP0167 NEW)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CTest)
include("buildsystem/deps.cmake")

option(PYLAI_BUILD_TESTS "构建 tests/ 示例应用与端到端验证" ${BUILD_TESTING})

add_subdirectory(standalone)
add_subdirectory(server)
if(PYLAI_BUILD_TESTS)
    add_subdirectory(tests)
endif()

include(CMakePackageConfigHelpers)

install(EXPORT pylaiTargets
    FILE pylaiTargets.cmake
    NAMESPACE pylai::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/pylai"
    EXPORT_PACKAGE_DEPENDENCIES
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/pylaiConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "buildsystem/pylaiConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pylaiConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/pylai"
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/pylaiConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/pylaiConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/pylai"
)
