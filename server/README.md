# server/ —— 应用层：HTTP 服务器实现（可执行 `pylai`）

## 这个目录的职责

**唯一职责：实现 HTTP 服务器**，产出可执行 target **`pylai`**。

- **名字来源**：**πύλαι（pylai）**，"门 / 关口"（如 Thermopylae 温泉关，字面即"热之门"）。
  命名顺着上游 Herodotus → Clio 的古典脉络：HTTP server 正是系统的**唯一门户**。
- **上游去向**（🟡 2026-10-12 用户拍板）：本实现将成为
  [herodotus](https://github.com/Joker-Principal/herodotus) 的 `clio` 组件的 **HTTP server 实现**，
  目标是**取代 hical**。
  本仓库刻意与 hical 的差异：**standalone asio + C++ module**（hical 用 Boost.Asio）、
  **`llhttp`** 解析（hical 用 `picohttpparser`）。
- **设计路线**见 `docs/http-server-plan.md`（L0 协议 → L1 连接/流 → L2 执行与并发 → L3 观测 → L4 参数）。

## 当前状态（🟡 Stage 0 骨架，未完成）

**只是 TCP 字节回显** —— 唯一目的：把「asio 模块 + io_uring + 固定线程数 `run()`」这条链路跑通。

刻意**尚无**（按计划分层顺序逐步加）：

| 缺什么 | 计划位置 |
|---|---|
| HTTP 解析（`llhttp`） | L0，下一步 |
| 有界队列 / 在途上限 / 超时 | Stage 0 的验收项（§9） |
| 指标 · 自适应 · TLS | L3 / L4 / Stage 2 |

## 构建与运行

```
cmake --preset dev_llvm                # 仅当依赖 / 目录结构变化时需要
cmake --build --preset debug_llvm
./build-dev_llvm/server/Debug/pylai    # 监听 127.0.0.1:18080
                                       # 注：子目录 target 的产物带一层目录名
```

冒烟（发一个 HTTP 请求，响应体是解析结果摘要）：

```bash
python3 -c "import socket;s=socket.create_connection(('127.0.0.1',18080));s.sendall(b'GET /hello HTTP/1.1\r\nHost: a\r\n\r\n');print(s.recv(400).decode())"
```

## 用 clice 查路由设施

路由的 pattern 层是**编译期**的（`route.cppm` 的 `pylai::route`）——"路由表"本身就是一堆
类型 / 常量，正是 clice 索引擅长回答的东西：

```bash
tools/clice query --method documentSymbols --path server/route.cppm --fresh
tools/clice query --method symbolSearch    --query 'health'  --limit 5
tools/clice query --method references      --name 'catch_all'
tools/clice query --method definition      --name 'parse_into'
```

⚠️ **每次 `cmake --preset` 会重写 `compile_commands.json`** → 索引过期；
查询前加 `--fresh`，或先 `tools/clice index`。答案里的 `stale` 字段务必看。

路由的**编译期用例**在 `pylai::route::selftest`（`route.cppm`）—— 每次 `cmake --build` 都会跑一遍，
所以"错误的路由表编不过"这件事是**持续验证**的，不靠额外测试命令。

## 调试（VS Code + CodeLLDB）

`.vscode/launch.json` 提供三个配置（`preLaunchTask` 会自动先构建）：

| 配置 | 用途 |
|---|---|
| `Debug pylai` | Debug 构建，日常调试 |
| `Debug pylai (ASan)` | AddressSanitizer 构建，查 use-after-free / 越界 |
| `Attach to pylai` | 附加到已在运行的实例 |

⚠️ pylai 是**服务器**：按 F5 起来后必须**另开一个终端发请求**（比如上面那条冒烟命令），才会命中断点。

排查 cancellation 的 UAF：见 `docs/asio-notes.md` §7；建议先在这些位置下断点 ——
`asio/impl/cancellation_signal.ipp:44`（`cancellation_slot::clear()`）与
`asio/impl/awaitable.hpp:170`（`clear_cancellation_slot()`）。

## 与 `standalone/` 的边界

- 应用代码**只** `import asio;`，**不** `#include <asio/...>`。
- 需要 asio 的新名字时，往 `standalone/asio.cppm` 的导出清单**加一行 `using`**，别绕过模块。

## 当前写死在源码顶部的可调项

| 常量 | 值 | 说明 |
|---|---|---|
| `kBindAddress` / `kPort` | `127.0.0.1:18080` | 上游 clio 的既定基地址是 `127.0.0.1:7788`；这里先用 18080，避免与本机已有服务冲突 |
| `kIoThreads` | `1` | 计划 §5.2：纯异步路径下"加线程不会让 I/O 更快"，并发应存进连接状态；等 L3 观测层就位再谈调参（§5.4 / §5.6） |
