# C++ 模块在本仓库的布局与踩坑

> 状态标记：**✅ 已验证** · **🟡 倾向** · **🔍 待调研** · **❓ 未定**
>
> 这里放**机制**与**踩过的坑**；`AGENTS.md` §1 放"规矩"。**代码里只留一句"为什么"**，
> 其余都指到这里（`AGENTS.md` §5 的注释纪律）。

## 分区 vs 独立模块（✅ 已验证）

跨**独立模块**做"前向声明 + 定义"是非法的：

```
error: declaration of 'App' in module X follows declaration in module Y
```

用**模块分区**（`export module pylai:xxx;`）就能在分区之间自由前向声明。
所以本仓库的公开面是**一个模块 `pylai` + 若干分区**，不是若干个独立模块。

## 依赖图必须是 DAG（✅ 已验证）

分区之间形成导入环时，CMake 直接报：

```
Circular dependency detected in the C++ module import graph
```

因此把共享数据下沉到最底层。当前（2026-10-12）：

```
:config ← :http ← :route ← :session ← :server
```

`Settings` 放在 `:config` 这个**叶子分区**，是为了让 `:session` 与 `:server`
都能用它、又不必互相依赖。

> 📌 **修订（2026-10-12）**：本节此前写作
> `:config (:http) ← :route ← :session ← :server ← :app ← pylai`。
> **`App` 已与 `Server` 合并**，该图作废——原图当时留在 `config.cppm` 里，属于"随代码失效的注释"。

## 实现单元的 import 规则（✅ 已验证）

实现单元（`module pylai;`，**不带分区名**）里用到的每个 `import`，
都必须由**主接口单元**直接或间接导入 —— 见 `[module.import]` 对实现单元的限制。
所以 `pylai.cppm` 里显式列着 `import asio;` / `import asio.experimental;`，看着多余，其实是必需的。

⚠️ 另一条：实现单元若写成 `module pylai:part;`，CMake 会当成**内部分区**，
于是报 "module-providing source 不在 CXX_MODULES 文件集"。**实现单元写 `module pylai;` 就好。**

## 标准库头 / C 头必须放全局模块片段（✅ 已验证）

在**模块附加区**（`export module ...;` 之后）include 标准库头，会让 `<wchar.h>` 之类
被解析两次（模块内 vs 全局），产生 `conflicting types`。
所以 `module;` 与 `export module ...;` 之间集中放**所有** `#include`。

## 定义协程的 TU 必须自己看到 `<coroutine>`（✅ 已验证）

即使接口单元已经 `import asio;`，**协程的定义处**仍要 `#include <coroutine>`
（`route.cpp` / `server.cpp` / `session.cpp` 顶部都有这一行）。

## asio 封装（`standalone/`）的导出坑（✅ 已验证）

- `#include <asio/spawn.hpp>` 要按 `ASIO_HAS_BOOST_CONTEXT_FIBER` 守卫；`yield_context` 只在 spawn.hpp 里声明。
- ⚠️ 用普通 completion handler（lambda）时 asio 会实例化 op 模板，其中 `o->ec_ = asio::error::eof;` 依赖 ADL 找
  `asio::error::make_error_code` → 必须在模块里**导出**它，否则 TU 侧报 `no matching function for call to 'make_error_code'`。
  走 `use_awaitable` 的路径不触发（所以老 `main.cpp` 从没暴露）。
- ⚠️ `asio::ssl::verify_peer` 等是命名空间作用域的 `const int`（内部链接）→ **不能 re-export**；
  应用侧写 `asio::ssl::context::verify_peer`。
- 弃用名要按 `ASIO_NO_DEPRECATED` 守卫（该宏生效的头：`basic_deadline_timer.hpp`、`basic_io_object.hpp`、
  `deadline_timer.hpp`、`io_context_strand.hpp`、`time_traits.hpp`）。
- ⚠️ 要让自家类型当**协程返回类型**，必须**导出** `asio::detail::awaitable_frame`：
  `awaitable` 本身**没有** `promise_type` 成员，asio 自己也是靠 `coroutine_traits<awaitable<T,Executor>, ...>`
  指过去的（`impl/awaitable.hpp:1082`）。

## 相关

- 应用代码只 `import asio;`，不 `#include <asio/...>` —— 见 `server/README.md`。
- 第三方库**不封 module**（spdlog / llhttp 直接 include）—— 见 `server/CMakeLists.txt`。
