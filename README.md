# pylai

C++23 的 HTTP 服务器框架：**standalone asio + C++20 modules**，解析器用 `llhttp`。

- **声明式路由**：路由表是编译期类型，冲 突 / 拼错的 pattern 在编译期就拦下
- **一次分配的响应链**：`co_await res.body(...).flush()`，热点路径不额外分配
- **每请求观测挂点**：为后续的自适应调优留槽（可整体关掉）
- **同一套源码跨平台**：Linux 走 io_uring、Windows 走 IOCP、macOS 走 kqueue

```cpp
import pylai;
using namespace pylai::route;

struct App
{
    asio::awaitable<void> health(Get<"/health">, StringResponse& res)
    {
        co_await res.body("ok\n").flush();
    }
};

auto server = pylai::Server::builder().bind("127.0.0.1", 8080).route_on(&app, &App::health).build();
return server.run();
```

完整用法（路由 DSL 全表 · 请求与响应 · 观测挂点）→ **`server/README.md`**。

## 平台

| 平台 | asio 后端 | 状态 |
|---|---|---|
| Linux（含 WSL2） | **io_uring** | ✅ 日常验证（构建 + 端到端） |
| Windows | **IOCP**（MSVC 14.34+ / VS 2022 17.4+） | 🔍 已按平台分叉，未真机验证 |
| macOS | **kqueue** | 🔍 未验证 |

平台差异只在构建层分叉（CMake 与 `vcpkg.json`），`server/` 里没有 `#ifdef`。
细节与 Windows 构建步骤 → `docs/cross-platform.md`。

## 快速开始

构建（终端——**要读到输出就走这条路**）：

```
cmake --preset dev_llvm
cmake --build --preset debug_llvm
```

验证（起服务器 → 发请求断言；跨平台，Windows 上同样用它）：

```
python tests/run.py
```

构建开关（都可在 preset 后面追加 `-D<名字>=OFF/ON` 覆盖）：

| 变量 | 默认 | 作用 |
|---|---|---|
| `PYLAI_BUILD_TESTS` | `ON` | 是否构建 `tests/`（示例应用 `app` + 端到端）；只拿库时关掉 |
| `PYLAI_ENABLE_OBSERVATION` | `ON` | 是否编译观测钩子与每请求观测槽（见 `server/README.md` §5） |

> 编译器与 vcpkg 路径是**机器本地信息**，放你自己的 `CMakeUserPresets.json`（gitignored）。
> 预设的约定与 Windows 写法 → `docs/maintainers.md` §1.2、`docs/cross-platform.md` §3.2。

## 安装（给别人用）

```
cmake --install build-dev_llvm --config Debug --prefix /path/to/prefix
```

装出来是**单包 `pylai`**（命名空间 `pylai::`）：

| 内容 | 位置 |
|---|---|
| 静态库 | `lib/libpylai.a` · `lib/libasio-module.a` |
| 模块接口单元（`import pylai;` / `import asio;`） | `include/pylai/` · `include/asio-module/` |
| CMake 包配置 | `lib/cmake/pylai/pylaiConfig.cmake` |

消费方只需要两行：

```cmake
find_package(pylai 0.1 REQUIRED CONFIG)
target_link_libraries(app PRIVATE pylai::pylai)   # pylai::asio-module 会跟着来
```

⚠️ 两条要注意：

- 消费方也要 C++23 + 支持 C++20 modules 的生成器（Ninja / Ninja Multi-Config）—— 模块以**接口单元源码**交付，BMI 由消费方生成。
- **依赖不随包安装**（asio / Boost / OpenSSL / jk / llhttp / fmt / spdlog），由 `pylaiConfig.cmake` 自己 `find_dependency`。
  其他限制（只装 Debug 的多配置问题、jk 会被一并装进同一 prefix）→ `docs/maintainers.md` §1.4。

## 目录地图（想深入代码时看）

| 目录 | 职责 | 详见 |
|---|---|---|
| `server/` | **HTTP 服务器框架**（静态库 `pylai`），不含业务路由 | `server/README.md` |
| `standalone/` | asio → C++20 module 封装（target `asio-module`） | `standalone/README.md` |
| `tests/` | 示例应用 + 端到端验证 | `tests/README.md` |
| `buildsystem/` | CMake 的依赖引入与包配置模板 | `buildsystem/README.md` |
| `ports/` | vcpkg overlay ports（asio 1.38.2 由此提供） | `ports/README.md` |
| `tools/` | 仓库内小工具（clice 启动器） | `tools/README.md` |
| `boost/` | boost.asio 的 module 封装草稿（未参与构建） | `boost/README.md` |
| `docs/` | 调研笔记、设计文档、维护者手册 | `docs/README.md` |

工作区**之外**但要经常查的目录（vcpkg 根等）→ `docs/vcpkg.md`。

## 文档

| 想知道 | 去哪 |
|---|---|
| 怎么用这个框架 | `server/README.md` |
| 现状设计为什么是这样 | `docs/http-server-plan.md` |
| 怎么改这个仓库（构建 / 工具 / 内部细节） | `docs/maintainers.md` |
| 平台差异与 Windows 构建 | `docs/cross-platform.md` |
| 走到今天的过程（含被推翻的结论） | `docs/changelog.md` |
| 工作纪律（维护者与 AI agent） | `AGENTS.md` |
