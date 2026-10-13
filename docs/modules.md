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

✅ **2026-10-13 新增一条外部边**：`:route` 现在 `import jk;`（用 jk 的 `Callable` 做 `pmf` 类型萃取）。
jk 是 `FetchContent` 拉的独立模块（target `joker::module`），不参与分区环，所以不影响上面的 DAG。

```
jk  ──→  :route
```

`Settings` 放在 `:config` 这个**叶子分区**，是为了让 `:session` 与 `:server`
都能用它、又不必互相依赖。

> 📌 **修订（2026-10-12）**：本节此前写作
> `:config (:http) ← :route ← :session ← :server ← :app ← pylai`。
> **`App` 已与 `Server` 合并**，该图作废——原图当时留在 `config.cppm` 里，属于"随代码失效的注释"。

## 跨 target 的 PCM 语言选项必须一致（✅ 已验证）

jk（`FetchContent` 拉的那个）对自己的 target 设了 `CXX_EXTENSIONS OFF` ⇒ 它的编译命令是 `-std=c++23`。
我们若保持 CMake 默认的 `CXX_EXTENSIONS ON`（`-std=gnu++23`），**clice 会拒绝加载 jk 的 PCM**：

```
BuildPCM failed for module pylai:route: GNU extensions was disabled in precompiled file '…/jk-….pcm'
but is currently enabled; … cannot be loaded due to a configuration mismatch
```

⇒ 根 `CMakeLists.txt` 设 `set(CMAKE_CXX_EXTENSIONS OFF)` 对齐。ninja 直接构建能容忍这种混用，
**只有 clice 的 PCM 复用会硬失败** —— 而后果不是把报错拍在脸上，是**索引静默退化**（该文件只剩扫描级符号，导航缺一半）。

## 实现单元的 import 规则（✅ 已验证）

实现单元（`module pylai;`，**不带分区名**）里用到的每个 `import`，
都必须由**主接口单元**直接或间接导入 —— 见 `[module.import]` 对实现单元的限制。
所以 `pylai.cppm` 里显式列着 `import asio;` / `import asio.experimental;`，看着多余，其实是必需的。

⚠️ 另一条：实现单元若写成 `module pylai:part;`，CMake 会当成**内部分区**，
于是报 "module-providing source 不在 CXX_MODULES 文件集"。**实现单元写 `module pylai;` 就好。**

⚠️ **消费方要自己 `import asio;`** —— handler 的返回类型是 `asio::awaitable<void>`，而 `pylai` 没有 `export import asio`
（框架不替消费方决定 asio 的可见性）。示例见 `tests/main.cpp` 顶部。

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
- 🔄 **2026-10-13 修订**：要让自家类型当**协程返回类型**时，确实需要 `awaitable_frame` 可见（`awaitable` 本身**没有** `promise_type`，
  asio 自己也是靠 `coroutine_traits<awaitable<T,Executor>, ...>` 指过去的，`impl/awaitable.hpp:1082`），
  而那种写法要靠特化 `std::coroutine_traits` **按名字**引用帧类型 ⇒ 当时导出了它。
  ⚠️ 但**声明搬到第一个形参后不再需要**（`route.cppm` 的特化已删）：handler 返回 `awaitable<void>` 是天然的，
  没有任何代码再按名字点到帧类型。✅ 实验：删掉 `using asio::detail::awaitable_frame;` 后全量重编 18 步通过（含 asio PCM 与全部下游）。

## 相关

- 应用代码只 `import asio;`，不 `#include <asio/...>` —— 见 `server/README.md`。
- 第三方库**不封 module**（spdlog / llhttp 直接 include）—— 见 `server/CMakeLists.txt`。
