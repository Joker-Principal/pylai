file(GLOB_RECURSE ASIO_SOURCES CONFIGURE_DEPENDS *.cpp)
file(GLOB_RECURSE ASIO_MODULES CONFIGURE_DEPENDS *.cppm)

add_library(asio-module ${ASIO_SOURCES})
target_sources(asio-module PUBLIC
    FILE_SET asio_set TYPE CXX_MODULES 
    FILES ${ASIO_MODULES}
)
target_link_libraries(asio-module PUBLIC asio::asio Boost::context OpenSSL::SSL OpenSSL::Crypto PkgConfig::liburing)
target_compile_definitions(asio-module PUBLIC 
    ASIO_SEPARATE_COMPILATION=1
    ASIO_NO_DEPRECATED=1
    ASIO_HAS_IO_URING=1
    ASIO_HAS_IO_URING_AS_DEFAULT=1
)
