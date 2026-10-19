file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(app ${APP_SOURCES})
target_link_libraries(app PRIVATE pylai)
