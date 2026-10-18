file(GLOB_RECURSE ASIO_SOURCES CONFIGURE_DEPENDS *.cpp)
file(GLOB_RECURSE ASIO_MODULES CONFIGURE_DEPENDS *.cppm)

add_library(asio-module ${ASIO_SOURCES})
target_sources(asio-module PUBLIC
    FILE_SET asio_set TYPE CXX_MODULES 
    FILES ${ASIO_MODULES}
)
# 平台后端：asio 自带选择，不靠我们定义宏 ——
#   Linux   → io_uring（要 liburing）；Windows → IOCP（要 winsock）；macOS → kqueue（不要额外库）。
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

# io_uring 只在 Linux 存在；定义了就是"后端恒定启用"（用户定：不做运行期开关）。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(asio-module PUBLIC
        ASIO_HAS_IO_URING=1
        ASIO_HAS_IO_URING_AS_DEFAULT=1
    )
endif()
