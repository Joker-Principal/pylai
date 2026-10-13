# 示例应用：把 server/ 的框架（静态库 `pylai`）跑成一个真服务器，供端到端验证。
# 它只 `import pylai;`，不碰任何实现细节。
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS *.cpp)

add_executable(app ${APP_SOURCES})
target_link_libraries(app PRIVATE pylai)
