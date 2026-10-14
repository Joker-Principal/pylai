# 框架层：HTTP 服务器（静态库 `pylai`）。
# 只依赖模块层 `asio-module`；asio 的名字一律经 `import asio;` 取得。
# llhttp 是 C 库，只在 server/http.cpp 的全局模块片段里被 include。
#
# 这里**没有 main、没有业务路由** —— 只用框架起服务器的示例应用在 tests/。

file(GLOB_RECURSE PYLAI_SOURCES CONFIGURE_DEPENDS *.cpp)
file(GLOB_RECURSE PYLAI_MODULES CONFIGURE_DEPENDS *.cppm)

# 框架层自己的依赖（基础依赖在根 CMakeLists 里集中 find_package）。
find_package(llhttp CONFIG REQUIRED)
# fmt/spdlog 不做模块封装（按约定：第三方库不封 module），直接 include 头文件。
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# 与 standalone/ 同样的写法：
#   - 接口单元（含主接口与分区，`export module pylai[...]`）进 CXX_MODULES 文件集；
#   - 实现单元写的是 `module pylai;`（**不带分区名**，否则 CMake 会把
#     `module M:part;` 当成“内部分区”并报 module-providing source 不在
#     CXX_MODULES 文件集 —— 见 cmake-cxxmodules(7) 的 Internal Module
#     Partition Extension 一节），因此它们可以当普通源。
add_library(pylai STATIC ${PYLAI_SOURCES})
# 文件集是 PUBLIC：消费方（tests/）要能 `import pylai;`。
target_sources(pylai
    PUBLIC
        FILE_SET pylai_modules TYPE CXX_MODULES
        FILES ${PYLAI_MODULES}
)
# asio-module / joker::module 被接口单元（乃至 `export import :session`）带到消费方
# → PUBLIC；llhttp / fmt / spdlog 只在实现单元的全局模块片段里出现 → PRIVATE。
target_link_libraries(pylai
    PUBLIC
        asio-module
        joker::module
    PRIVATE
        llhttp::llhttp_static
        fmt::fmt
        spdlog::spdlog
)
