# buildsystem/ —— 构建脚本与包配置模板

## 这个目录的职责

放 **CMake 侧、但不属于任何源码目标**的文件：依赖怎么引入、装出去的包长什么样。
根 `CMakeLists.txt` 只留项目高层决策（标准、开关、子目录、安装导出），构建细节集中在这里。

| 文件 | 作用 |
|---|---|
| `deps.cmake` | 集中引入第三方依赖（`jk` 与其他库都走 `find_package`，由 vcpkg 提供 —— `jk` 来自 `joker-registry` 的端口 `jk`；Linux 另加 liburing） |
| `pylaiConfig.cmake.in` | `cmake --install` 后生成 `pylaiConfig.cmake` 的模板：`find_dependency` 依赖链 + 引入 `pylaiTargets.cmake` |

## 不该放这里

源码、模块接口单元、测试。那些属于 `server/` / `standalone/` / `tests/`。

构建系统的取舍与坑 → `docs/maintainers.md` §1。
