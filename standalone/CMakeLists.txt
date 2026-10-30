option(PYLAI_ASIO_MODULE "Build the standalone ASIO module" OFF)

file(GLOB_RECURSE ASIO_SOURCES CONFIGURE_DEPENDS *.cpp)

add_library(asio-standalone STATIC ${ASIO_SOURCES})
target_link_libraries(asio-standalone PUBLIC 
    asio::asio
    OpenSSL::SSL 
    OpenSSL::Crypto 
    ${ASIO_PLATFORM_LIBS}
)
target_compile_definitions(asio-standalone PUBLIC
    ASIO_SEPARATE_COMPILATION=1
    ASIO_NO_DEPRECATED=1
    ASIO_HAS_THREADS=1
    ASIO_DISABLE_BOOST_CONTEXT_FIBER=1
    $<$<PLATFORM_ID:Linux>:ASIO_HAS_IO_URING=1>
    $<$<PLATFORM_ID:Linux>:ASIO_HAS_IO_URING_AS_DEFAULT=1>
    $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>
)

if(PYLAI_ASIO_MODULE)
    file(GLOB_RECURSE ASIO_MODULES CONFIGURE_DEPENDS *.cppm)
    target_sources(asio-standalone PUBLIC
        FILE_SET asio_set TYPE CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${ASIO_MODULES}
    )
    install(TARGETS asio-standalone
        EXPORT pylaiTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        FILE_SET asio_set DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/asio-standalone"
    )
else()
    install(TARGETS asio-standalone
        EXPORT pylaiTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif()
