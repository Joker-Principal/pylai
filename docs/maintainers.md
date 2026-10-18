# 维护者手册 —— 构建 · 工具 · 内部细节

> **给改这个仓库的人看的。** 只想用它：根 `README.md` + `server/README.md` 就够。
> 状态标记：**✅ 已验证** · **🟡 倾向** · **🔍 待调研 / 未验证** · **❓ 未定**。

## 1. 构建系统

### 1.1 依赖从哪来（`buildsystem/deps.cmake`）

| 依赖 | 来源 |
|---|---|
| **jk** | `find_package(jk)`；2.0.0 来自 git registry `joker-registry` 的 `ports/jk`（钉 tag `v2.0.0`） |
| **asio** | `find_package`；1.38.2 来自同一个 registry 的 `ports/asio`（内置端口停在 1.32.0） |
| **Boost / OpenSSL** | `find_package`（vcpkg） |
| **liburing** | 仅 Linux：`PkgConfig` + `pkg_check_modules` |

改 jk 的版本 = 在 `joker-registry` 仓里改三处（`ports/jk/portfile.cmake` 的 `REF` / `SHA512`、`ports/jk/vcpkg.json` 的 `version`），发布后把新 commit 填回本仓 `vcpkg-configuration.json` 的 `baseline`。**本仓库不再保存 `ports/` 副本**，端口只在 registry 仓维护。
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

都是**缓存变量**：关过之后要显式 `-D…=ON` 才会打开（preset 不会替你改回来）。
> `JK_BUILD_TESTING` 在我们这边已经无关 —— jk 由 vcpkg 构建，端口里直接传 `-DBUILD_TESTING=OFF -DJK_BUILD_TESTING=OFF`。

### 1.4 安装 / 导出（✅ 已验证）

- 单包 **`pylai`**、命名空间 `pylai::`，`pylai::asio-module` 与 `pylai::pylai` 同属导出集 `pylaiTargets`。
- 模块只装**接口单元源码**（`include/asio-module/`、`include/pylai/`）；BMI 由消费方生成 —— 导出的 `IMPORTED_CXX_MODULES_*` 带上了编译定义与链接口径。
- 依赖不随包：`buildsystem/pylaiConfig.cmake.in` 逐个 `find_dependency`；`plf_hive.h`（纯头文件、无 CMake 包）靠 `find_path` 兜。
- 验证方式：装到工作区内的临时前缀，再建一个**独立工程**（`find_package(pylai 0.1 REQUIRED CONFIG)` + `import pylai;`）→ configure / build / 运行。探针在 `.vscode/tmp/consumer-check/`（不在版本库）。
- ⚠️ 只装 Debug 时，消费方用 **Ninja Multi-Config** 会报 `IMPORTED_LOCATION not set for imported target "joker::module" configuration "RelWithDebInfo"` ⇒ 用单配置 Ninja，或把各配置都装上。

### 1.5 历史：把项目当 vcpkg 端口用时的坑（overlay 方案，🔄 已被 §1.6 取代）

> 早期只想到 overlay ports；下表是当时逐条分析出的坑。现已改用 git registry（§1.6），其中大部分不再成立。

要这么干，消费方得自己写一个 `ports/pylai/portfile.cmake`（指向本仓库）并加进**他们项目**的 `overlay-ports`。会遇到：

| 问题 | 为什么 |
|---|---|
| **overlay 不传递** | vcpkg 只读**根项目**的 `vcpkg-configuration.json`，依赖包自带的 overlay 声明一律忽略 ⇒ 我们的 `ports/asio`（1.38.2）与 `ports/jk` 不会被看见。没有 `jk` 端口 = `find_package(jk)` 直接失败；asio 退回 registry 的 1.32 = 用到的新 API 编不过 |
| **C++20 modules 不由 vcpkg 管** | 端口只把 `.cppm` 装进 triplet，BMI 由**消费方**生成 ⇒ 必须 CMake ≥ 3.28 + Ninja；Visual Studio 生成器不支持导入 target 的 modules（CMake 文档写明） |
| **标准不随 target 导出** | 我们只设了 `CMAKE_CXX_STANDARD 23`，没给 target 加 `cxx_std_23` ⇒ 消费方漏设标准时 BMI 不兼容。要么消费方自觉，要么以后给 target 补 `target_compile_features(… cxx_std_23)` |
| **重复的模块** | 消费方若既装了端口又 `add_subdirectory(本仓库)`，同一个程序里会有两份 `pylai` 模块 —— 强模块所有权下直接错 |
| **版本只能钉 commit** | 仓库没有 release / tag ⇒ 消费方拿不到语义版本；改 `REF` 必须同步 `version-date`，否则 vcpkg 命旧缓存 |
| **依赖必须全** | asio[spawn,ssl] · boost-asio[spawn,ssl] · openssl · llhttp · fmt · spdlog · plf-hive · liburing（仅 linux）· jk —— 少一个端口就 configure 失败 |
| **triplet / CRT** | Windows 上静态 / 动态 CRT 混用会 LNK2038；消费方必须与端口用同一个 triplet |
| **overlay 名字冲突** | 把我们的 `ports/` 整目录加进他们的 overlay，会连带把我们的 `asio` 版本覆盖过去 |

已经做对的部分：导出用 `_IMPORT_PREFIX` 相对路径（可 relocate）· `liburing` 用 `"platform": "linux"` 门控 · `pylaiConfig.cmake` 用 `find_dependency` 把依赖接力出去。

**结论（✅ 2026-10-18 现况）**：三种方式并存 —— `add_subdirectory`、安装后的 `find_package(pylai)`、以及**已落地的 git registry `joker-registry`**（消费方声明一次，`pylai` / `jk` / `asio` 就都从那里解析，见 §1.6）。无论哪种，消费方都必须是 Ninja + C++23。

### 1.6 注册表方案：`joker-registry`（✅ 已验证，已是唯一来源）

`jk` / `asio` / `pylai` 的端口都放在 [`joker-registry`](https://github.com/Joker-Principal/joker-registry) 仓；
本仓库**自己也只用**这个 registry（`vcpkg-configuration.json` 里 `packages: [asio, jk]` + `baseline`），不再保留 `ports/` 副本。
相对 §1.5 的 overlay 方案，它解决最要命的两条：

- **传递性**：registry 由**消费方**声明，但**对所有端口名解析生效**（包括依赖的依赖）⇒ 他们声明一次，`pylai` / `jk` / `asio` 都从我们的 registry 取，不用复制 overlay 目录。
- **版本可复现**：每个版本写进 `versions/<前缀>/<port>.json` + `versions/baseline.json`；消费方钉一个 baseline commit，可显式升级 / 回退。

registry 仓的结构（它只认**仓库根**的 `ports` 与 `versions`，git registry 没有“子目录”选项）：

```text
ports/pylai/{portfile.cmake,vcpkg.json}
ports/jk/...
ports/asio/...
versions/p-/pylai.json
versions/j-/jk.json
versions/a-/asio.json
versions/baseline.json
```

生成版本库（会同时更新 baseline）：

```bash
vcpkg x-add-version --x-builtin-ports-root=./ports --x-builtin-registry-versions-dir=./versions pylai
```

⚠️ 顺序与不可变性：**先提交 `ports/`（或打 tag）再跑 `x-add-version`** —— 版本文件里的 `git-tree` 必须对应仓库里真实存在的 tree；已发布的版本**不许**改写 `git-tree`，改内容就 `port-version` +1。

消费方配置：

```json
{
  "registries": [{
    "kind": "git",
    "repository": "https://github.com/Joker-Principal/joker-registry",
    "baseline": "<registry 仓 master 的 commit>",
    "packages": [ "pylai", "jk", "asio" ]
  }]
}
```

⚠️ `packages` 必须**三个都列**：漏 `jk` 就找不到端口，漏 `asio` 会退回内置的 1.32。
⚠️ 用了 `registries` 就还得给默认 registry 一个 baseline：推荐写在 `vcpkg-configuration.json` 的 `"default-registry": { "kind": "builtin", "baseline": "<vcpkg 根 commit>" }`（配置归配置），等价写法是 `vcpkg.json` 的 `builtin-baseline`；两个都没有时 configure 报 `Using registries requires that a baseline is set for the default registry…`。本仓库就是写在 configuration 里的。

**仍然解决不了的**：§1.5 里 modules 那三条（BMI 由消费方生成 ⇒ Ninja + C++23）· 端口与 `add_subdirectory` 并存撞模块 · CRT / triplet 必须一致。
其中“标准不随 target 导出”可以顺手修：给两个 target 加 `target_compile_features(... PUBLIC cxx_std_23)`。

**成本**：多一个 `ports/pylai` 端口 + `versions/` 目录；每次发版要 `x-add-version` 并推送新的 baseline commit。

**✅ 2026-10-18 实测（registry 已上传 GitHub）**：registry 仓 = `git@github.com:Joker-Principal/joker-registry.git`（同仓的 https 形式亦可），`master` 当前为 `964e6ce`，baseline 就指向它；其中 `pylai` 端口钉 `52ec2b0`、`port-version` 为 1（之前内容变而不改 `version` 会导致 `x-add-version` 报 `checked-in files have changed but the version was not updated`）。独立工程只写 `vcpkg.json`（`dependencies: ["pylai"]` + `builtin-baseline`）与 `vcpkg-configuration.json`（`kind: git` + `packages: [pylai, jk, asio]`），vcpkg 就从 GitHub 装出 `pylai 0.1.0#1` + `jk 2.0.0` + `asio 1.38.2`，消费方 `import pylai;` 编译 / 链接 / 运行全绿（GCC 16.2.1 验过；更早一次是 clang 23）。

⚠️ **GCC 也能编（2026-10-18 修订）**：先前记的“端口构建必须用 clang，因为 GCC 16 在 asio `co_composed.hpp` 编不过、属上游”是**错的**。真实原因是两条，都已修：

1. **三个 TU 缺头**：`co_composed.hpp` 靠 `std::tuple_size` 判断结构化绑定，缺 `<tuple>` 时它退化成“按基类解构”并报 `cannot decompose class type`（`server/http.cppm`、`server/route.checks.cpp`、`tests/stream_concepts.cpp` 分别补了 `<tuple>` / `<typeinfo>` 等）。
2. **`standalone/asio.cpp` 曾被写成模块实现单元**（`module asio;`）：`execution_context_service_base<config_service>::id` 同时在接口单元 `asio.cppm` 里被实例化 ⇒ GCC 视其为**模块附属实体**，在实现单元里发**非 COMDAT 强定义**，与消费方 TU 的 GNU unique（`nm` 显示 `u`）冲突。`src.hpp` 只要求 `ASIO_SEPARATE_COMPILATION`，写成普通 TU 即可 —— 10 个 `id` 全回到 `u`。

复核：`nm -C build-*/standalone/*/libasio-module.a | grep 'execution_context_service_base<asio::config_service>::id'` 应是 `u`（或加 `-fno-gnu-unique` 后的 `V`），**不应出现 `B`**。

⚠️ 这一步暴露了两个真 bug（已修）：`include(GNUInstallDirs)` 原本在 `add_subdirectory` 之后（全新构建时 `${CMAKE_INSTALL_INCLUDEDIR}` 为空 ⇒ 装到 `/asio-module`）；`http.cppm` 缺 `<coroutine>`（GCC 才报）。

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
clice_launcher query --method documentSymbols --path server/route.cppm --fresh
clice_launcher query --method symbolSearch    --symbol 'health'   --limit 5
clice_launcher query --method references      --symbol 'catch_all'
clice_launcher query --method definition      --symbol 'parse_into'
```

⚠️ **每次 `cmake --preset` 都会重写 `compile_commands.json`** ⇒ 索引过期；查询前加 `--fresh`，或先 `clice_launcher index`。答案里的 `stale` 字段务必看。完整纪律与其余方法见全局 skill `~/.agents/skills/clice-cli/` 与 `AGENTS.md` §3。

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
