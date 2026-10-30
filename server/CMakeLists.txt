# 接口单元里有 `#if` ⇒ 消费方必须看到同一个定义。
option(PYLAI_ENABLE_OBSERVATION "编译进观测钩子与每请求观测槽" ON)

file(GLOB_RECURSE PYLAI_SOURCES CONFIGURE_DEPENDS *.cpp)
file(GLOB_RECURSE PYLAI_MODULES CONFIGURE_DEPENDS *.cppm)

add_library(pylai STATIC ${PYLAI_SOURCES})
target_sources(pylai
    PUBLIC
        FILE_SET pylai_modules TYPE CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES 
            ${PYLAI_MODULES}
)
target_link_libraries(pylai
    PUBLIC
        asio-standalone
        joker::module
        fmt::fmt
        spdlog::spdlog
        llhttp::llhttp_static
)

target_compile_definitions(pylai PUBLIC 
    PYLAI_ENABLE_OBSERVATION=$<BOOL:${PYLAI_ENABLE_OBSERVATION}>
)

# 装到 <prefix>/include/pylai/，保留 server/ 下的相对布局。
install(TARGETS pylai
    EXPORT pylaiTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET pylai_modules DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
