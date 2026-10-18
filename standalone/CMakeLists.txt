file(GLOB_RECURSE ASIO_SOURCES CONFIGURE_DEPENDS *.cpp)
file(GLOB_RECURSE ASIO_MODULES CONFIGURE_DEPENDS *.cppm)

add_library(asio-module ${ASIO_SOURCES})
target_sources(asio-module PUBLIC
    FILE_SET asio_set TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${ASIO_MODULES}
)
set(ASIO_PLATFORM_LIBS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND ASIO_PLATFORM_LIBS PkgConfig::liburing)
elseif(WIN32)
    list(APPEND ASIO_PLATFORM_LIBS ws2_32 mswsock)
endif()

target_link_libraries(asio-module PUBLIC asio::asio Boost::context OpenSSL::SSL OpenSSL::Crypto ${ASIO_PLATFORM_LIBS})
target_compile_definitions(asio-module PUBLIC
    ASIO_SEPARATE_COMPILATION=1
    ASIO_NO_DEPRECATED=1
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(asio-module PUBLIC
        ASIO_HAS_IO_URING=1
        ASIO_HAS_IO_URING_AS_DEFAULT=1
    )
endif()

# 装到 <prefix>/include/asio-module/，保留 asio/ 子目录。
install(TARGETS asio-module
    EXPORT pylaiTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET asio_set DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/asio-module"
)
