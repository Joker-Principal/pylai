vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Joker-Principal/jk
    REF v2.0.0
    SHA512 4ee1f7bb57be45d741aab5cf01c591edc074e3906461a611a04981af7823b2204dee3b69402ebf87baa06e0ec740780c09536c429b94e445d1841d3135750a94
    HEAD_REF master
)

# jk 自己 include(CTest) 并把 BUILD_TESTING 默认成 ON ⇒ 显式关掉。
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_TESTING=OFF
        -DJK_BUILD_TESTING=OFF
)

vcpkg_cmake_install()

# jk 把包配置装在 share/jk/cmake（不是 share/jk）。
vcpkg_cmake_config_fixup(PACKAGE_NAME jk CONFIG_PATH "share/jk/cmake")

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
