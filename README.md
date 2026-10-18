# pylai

HTTP 服务器框架（`server/`，静态库 **`pylai`**）；底座是 `standalone/` —— 把 standalone asio 包成 **C++20 modules** 的薄封装。
（仓库原名 `asio-module`，那时它还只是 asio 的调试场。）

## 目录地图

| 目录 | 职责 | 详见 |
|---|---|---|
| `standalone/` | **唯一职责**：asio → C++20 module 封装（target `asio-module`） | `standalone/README.md` |
| `ports/` | vcpkg **overlay ports**（asio 1.38.2 由此提供） | `ports/README.md` |
| `docs/` | 调研笔记与设计草稿（**是笔记，不是决策**） | `docs/README.md` |
| `tools/` | 仓库内小工具（clice 启动器） | `tools/README.md` |
| `boost/` | boost.asio 的 module 封装草稿（**当前未参与构建**） | `boost/README.md` |
| `server/` | **HTTP 服务器框架**（静态库 `pylai`），**不含**业务路由 | `server/README.md` |
| `tests/` | 示例应用 + 端到端验证（可执行 `app`） | `tests/README.md` |

工作区**之外**但要经常查的目录（vcpkg 根等）→ `docs/vcpkg.md`。

## 平台

| 平台 | asio 后端 | 状态 |
|---|---|---|
| Linux（含 WSL2） | **io_uring** | ✅ 日常验证（构建 + 端到端） |
| Windows | **IOCP**（MSVC 14.34+ / VS 2022 17.4+） | 🔍 构建已按平台分叉，未真机验证 |
| macOS | **kqueue** | 🔍 未验证 |

平台差异**只在 CMake 与 `vcpkg.json` 里分叉**，`server/` 不写 `#ifdef`。
构建步骤与已知风险 → `docs/cross-platform.md`。

## 三类文档，分工明确

| 类型 | 去哪 | 性质 |
|---|---|---|
| **规矩**（相对稳定） | `AGENTS.md` | 必须遵守 |
| **目录职责**（该放什么） | 各目录的 `README.md` | 就近说明 |
| **笔记**（会变，标了状态） | `docs/` | 随时可能被推翻 |

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

也可以用 CMake Tools 触发配置 / 构建（状态栏按钮，或命令 `cmake.configure` / `cmake.build`）。
但**命令本身不返回可读输出**：要看编译错误请走 **Problems 面板诊断**，要原始输出请走终端。
理由与完整的"想做什么 → 走哪条路"对照表见 `AGENTS.md` §3。
