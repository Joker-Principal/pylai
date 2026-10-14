# 示例应用：把 server/ 的框架（静态库 `pylai`）跑成一个真服务器，供端到端验证。
# 它只 `import pylai;`，不碰任何实现细节。
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS *.cpp)

# handler 里拼字符串用 fmt（与库内部同一个包；框架不封 module ⇒ 消费方自己 include）。
find_package(fmt CONFIG REQUIRED)

add_executable(app ${APP_SOURCES})
target_link_libraries(app PRIVATE pylai fmt::fmt)
