# 维护者手册 —— 构建 · 工具 · 内部细节

> **给改这个仓库的人看的。** 只想用它：根 `README.md` + `server/README.md` 就够。
> 状态标记：**✅ 已验证** · **🟡 倾向** · **🔍 待调研 / 未验证** · **❓ 未定**。

## 1. 构建系统

### 1.1 依赖从哪来（`buildsystem/deps.cmake`）

| 依赖 | 来源 |
|---|---|
| **jk** | `FetchContent` 跟 `master`（浅克隆）。要可复现就钉 `45095019328b649e1e01e4d694c4e3ef5eb7c133` |
| **asio** | `find_package`；1.38.2 由 `ports/asio/` 这个 **overlay port** 提供 |
| **Boost / OpenSSL** | `find_package`（vcpkg） |
| **liburing** | 仅 Linux：`PkgConfig` + `pkg_check_modules` |

⚠️ **别手动 `git fetch` FetchContent 的浅克隆**（`build-*/_deps/<name>-src`）：它下一次更新会 `rebase`，浅克隆没有共同祖先 ⇒ 冲突卡死（2026-10-18 在 jk 上踩过）。
处置：删 `_deps/<name>-{src,subbuild,build}` 后重新 configure。

⚠️ vcpkg **不导出** `PKG_CONFIG_PATH` ⇒ 用 pkg-config 的库要自己按 triplet 补（`buildsystem/deps.cmake` 里 liburing 那段；安装后的包由 `pylaiConfig.cmake` 补）。

### 1.2 预设：tracked 只管公共部分，机器本地信息自己准备

- `CMakePresets.json`（tracked）只有 `base` / `base_debug|relwithdeb|release`。
- 编译器、工具链路径、triplet 属于机器本地信息 ⇒ **每台机器自备** `CMakeUserPresets.json`（gitignored）。
- ⚠️ CMake 的硬约束：**两个文件里的 preset 不能重名**，且 `CMakePresets.json` 里的 preset **不能继承** user 文件里的。
  ⇒ `dev_llvm` / `dev_msvc` 这类名字只能由 user 文件给。Windows 写法见 `cross-platform.md` §3.2。

### 1.3 开关

| 变量 | 默认 | 位置 / 作用 |
|---|---|---|
| `PYLAI_BUILD_TESTS` | 跟随 `BUILD_TESTING` | 根 `CMakeLists.txt`；门控 `add_subdirectory(tests)` |
| `PYLAI_ENABLE_OBSERVATION` | `ON` | `server/CMakeLists.txt`；定义是 **PUBLIC**（接口单元里有 `#if`） |
| `JK_BUILD_TESTING` | `OFF` | `buildsystem/deps.cmake`；jk 自带的测试 |

都是**缓存变量**：关过之后要显式 `-D…=ON` 才会打开（preset 不会替你改回来）。

### 1.4 安装 / 导出（✅ 已验证）

- 单包 **`pylai`**、命名空间 `pylai::`，`pylai::asio-module` 与 `pylai::pylai` 同属导出集 `pylaiTargets`。
- 模块只装**接口单元源码**（`include/asio-module/`、`include/pylai/`）；BMI 由消费方生成 —— 导出的 `IMPORTED_CXX_MODULES_*` 带上了编译定义与链接口径。
- 依赖不随包：`buildsystem/pylaiConfig.cmake.in` 逐个 `find_dependency`；`plf_hive.h`（纯头文件、无 CMake 包）靠 `find_path` 兜。
- 验证方式：装到工作区内的临时前缀，再建一个**独立工程**（`find_package(pylai 0.1 REQUIRED CONFIG)` + `import pylai;`）→ configure / build / 运行。探针在 `.vscode/tmp/consumer-check/`（不在版本库）。
- ⚠️ 只装 Debug 时，消费方用 **Ninja Multi-Config** 会报 `IMPORTED_LOCATION not set for imported target "joker::module" configuration "RelWithDebInfo"` ⇒ 用单配置 Ninja，或把各配置都装上。
- ⚠️ `cmake --install` 会把 FetchContent 的 **jk 一并装进同一个 prefix**（它的 install 规则在同一构建树里）。

## 2. 工具

### 2.1 验证入口

`python tests/run.py` —— 唯一入口（`tests/run.sh` 已删除）。它自己找 `build-*/tests/<Config>/app[.exe]`，也接受 `--exe` / `--host` / `--port`。

### 2.2 终端 vs CMake Tools

- **要看输出（编译错误、测试结果）走终端**：`cmake --preset …` / `cmake --build --preset …`。
- CMake Tools 也行（状态栏按钮，或命令 `cmake.configure` / `cmake.build`），但它**不返回可读输出** ⇒ 错误看 **Problems 面板**，原始输出走终端。
- 破坏性命令（`cmake.resetState` / `cmake.clean` / `cmake.fullCleanConfigure` / `cmake.*All`）要用户确认后再执行。

### 2.3 用 clice 查代码

路由的 pattern 层是**编译期**的（`route.cppm` 的 `pylai::route`）——"路由表"本身就是一堆类型 / 常量，正是索引擅长回答的东西：

```bash
tools/clice query --method documentSymbols --path server/route.cppm --fresh
tools/clice query --method symbolSearch    --symbol 'health'   --limit 5
tools/clice query --method references      --symbol 'catch_all'
tools/clice query --method definition      --symbol 'parse_into'
```

⚠️ **每次 `cmake --preset` 都会重写 `compile_commands.json`** ⇒ 索引过期；查询前加 `--fresh`，或先 `tools/clice index`。答案里的 `stale` 字段务必看。完整纪律见 `AGENTS.md` §3。

### 2.4 调试（VS Code + CodeLLDB）

`.vscode/launch.json` 三个配置（`preLaunchTask` 会先构建）：

| 配置 | 用途 |
|---|---|
| `Debug app` | Debug 构建，日常调试（产物 `build-dev_llvm/tests/Debug/app`） |
| `Debug app (ASan)` | AddressSanitizer 构建，查 use-after-free / 越界 |
| `Attach to app` | 附加到已在运行的实例 |

⚠️ 它是个**服务器**：按 F5 起来后必须**另开一个终端发请求**（`python tests/http_e2e.py`），才会命中断点。
排查 cancellation 的 UAF 见 `asio-notes.md` §7；建议断点：`asio/impl/cancellation_signal.ipp:44`、`asio/impl/awaitable.hpp:170`。

## 3. 内部细节（改 `server/` 前先看）

### 3.1 写死在源码顶部的可调项

| 常量 | 值 | 说明 |
|---|---|---|
| `kBindAddress` / `kPort` | `127.0.0.1:18080` | 上游 clio 的既定基地址是 `127.0.0.1:7788`；这里先用 18080 避开本机已有服务 |
| `kIoThreads` | `1` | 纯异步路径下"加线程不会让 I/O 更快"，并发应存进连接状态（`http-server-plan.md` §5.2 / §5.4） |

### 3.2 与 `standalone/` 的边界

- 应用代码**只** `import asio;`，**不** `#include <asio/...>`。
- 需要 asio 的新名字时，往 `standalone/asio.cppm` 的导出清单**加一行 `using`**，别绕过模块。

### 3.3 编译期用例在哪

路由的编译期用例（`static_assert`）在 **`server/route.checks.cpp`** —— `pylai` 的**实现单元**（`module pylai;`），
因为它要看树 / 建树器 / 匹配器这些**不导出**的内部件。它跟着每次 `cmake --build` 一起求值 ⇒ "错误的路由表编不过"是持续验证的。
代价：少了"顺带验收导出面"的护栏；补偿：`tests/main.cpp` 走公开面 + e2e 全程跑。
⚠️ "必须编不过"的失败路径不在这里 → 人工探针，见 `http-server-plan.md` §11.2。
