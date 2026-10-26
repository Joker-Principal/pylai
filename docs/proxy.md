# 反向代理 / 应用网关：框架该提供什么

> **状态：🟡 草稿（2026-10-27 讨论）**。本文只回答"归谁、缺什么、怎么判"，不写实现细节。
> 结论可被推翻；被推翻时加「修订」小节，不直接改旧结论（约定见 `docs/README.md`）。
> 实现纪律仍在 `AGENTS.md`；变更史进 `docs/changelog.md`。

## 0. 一句话

pylai 是 **HTTP Server 框架**，不是成品反代。目标是让反代**写得出来**，而不是把反代**做进来**：
框架给"用户自己写不出来、或写出来必然错"的东西，其余留给用户。

## 1. 归属判据

| 判据 | 归谁 | 例子 |
|---|---|---|
| 需要框架内部状态 / 协议细节才能做对 | 框架必须给**原语** | 下游断没断 · 请求体读没读完 · 响应怎么定界 |
| 只有一种合理实现，且每个用户都会重复写一遍 | 框架给**默认实现（可替换）** | 出站连接 · 最简连接池 |
| 有多种合理策略，或依赖业务 | **只留挂点** | LB 算法 · 健康检查 · 鉴权 · 限流 · 缓存 · 配置语言 |

配套总原则：**设施默认可关、默认最保守**（池可设空闲 0 关闭 · 重试默认 0 次 · 观测默认零开销，已有 `PYLAI_ENABLE_OBSERVATION`）。
不遵守这条，框架会逐渐长成产品。

## 2. 参考坐标（✅ 查证，来源见文末）

| | nginx | Pingora |
|---|---|---|
| 形态 | **配置驱动**的成品（指令 + 模块） | **代码驱动**的库（`ProxyHttp` + 阶段钩子） |
| 代理语义 | 默认**全缓冲**：`proxy_buffering on` / `proxy_request_buffering on`，可关 | 默认**双工直通**：上游请求头发出后即进入 duplex（响应→下游 ∥ 请求体→上游） |
| 扩展方式 | C 模块 / njs | 重写 trait 方法 |
| 不停机升级 | reload worker / 二进制升级 | upgrade socket **传 listening fd**，SIGQUIT 旧实例进 graceful shutdown |

值得抄的是**语义**，不是实现：

- **三个超时都是 idle 语义**，不是请求总时限（`proxy_connect_timeout` / `proxy_read_timeout` / `proxy_send_timeout`）
  —— 与本仓「不做请求总时限」的定案一致。
- **逐跳头部默认不转发**：nginx 默认把 `Host` 改写成 `$proxy_host`、`Connection` 改写成 `close`。
- **重试只在响应头尚未发给下游之前**；非幂等方法默认不重试。
- **回池判定**：Pingora 只复用"完全相同的 peer"（IP:port · scheme · SNI · 客户端证书 · 校验选项），
  请求中途出错即不可复用。
- **归因字段**：`$upstream_addr` / `$upstream_status` / `$upstream_connect_time` / `$upstream_header_time` /
  `$upstream_response_time` —— 用户报障时问的几乎全是这几项。

## 3. 框架欠的四件原语

| 原语 | 现状 | 非反代场景也要 |
|---|---|---|
| 请求体的流式读 | ✅ `http::BodyReader` 满足 `AsyncReadStream`；`route::Request::async_read_some` 直通 | 大上传 |
| 响应可多次 `flush` | ✅ chunked 侧本来就有：`route::UnknownLengthResponse::flush` 每次把 `pending` 作为一块发出并清空 | SSE · 大响应 |
| **定长 + 多块** | 🟡 **已落地、未编译验证**：`route::ByteResponse`（见 §4） | 大文件 · 导出流 |
| **出站流（HTTP 客户端）** | 🔍 **缺**（见 §5） | clio 调外部服务 |

**取消与超时**：机制已在，不是缺口 ——

| 层 | 现状 |
|---|---|
| 进程 / accept | ✅ `accept_signal` |
| 每会话（总取消） | ✅ `sessions`（hive）里每会话一个 `session_signal`，`co_spawn` 时绑槽；`Server::quit()` 逐个 `emit(all)` |
| 每请求 | ❌ 没有（keep-alive 下 `emit` 不可逆，只能整条连接作废） |
| 超时 | ✅ `Session::fill_buffer` / `Session::write` 用 `awaitable_operators` 的 `||` 与 `steady_timer` 竞速，落败方被取消 |

取消**随协程帧传播**：`co_await asio::co_spawn(…, asio::deferred)` 与 `use_awaitable` 等价
（`awaitable_thread::get_cancellation_slot()` 取的就是当前帧的 `cancellation_state_` 的槽），
所以"读请求体 / 写响应"这两道门面不会被取消卡住。

## 4. `ByteResponse` 的语义（🟡 已落地、未编译验证）

定长、可分块：长度**由调用方声明**（运行期值），体可反复追加、反复 `flush()`。
与 `StringResponse` 的分工：那个是文本、一次成型；这个是字节、长度先知道、内容边产边发。

| 边界 | 行为 |
|---|---|
| 多写（累计超过声明长度） | `body()` 立刻抛 `std::logic_error` |
| 没声明长度就 `body()` | 抛 `std::logic_error` |
| 少写（`finish()` 时不足） | 抛 `std::logic_error` —— `Content-Length` 已发出，补不回来，只能让连接层断开 |
| `HEAD` | 照发头（长度用声明值），正文抑制，不校验已写字节数 |

**长度由调用方保证，框架只做校验** —— 与 `UnknownLengthResponse`（把定界交给 chunked）正好互补。

## 5. 出站流 = HTTP 客户端

需求形态：出站连接也满足 `AsyncReadStream` / `AsyncWriteStream`，与进站**同构**，带 per-operation cancellation。
这样"转发"就是两个方向的 `async_read_some` → `write`，不需要新执行模型。

**先定接口，再选实现**（接口定了，用户的代码就不随后续选型变化）：

| 候选 | 待核风险 |
|---|---|
| 自建（llhttp 的 `HTTP_RESPONSE` 模式 + asio 协程） | llhttp 9.3.1 确有 `HTTP_RESPONSE = 2` ✅；代价是重定向 / cookie / 压缩 / TLS 校验 / 连接池都要自己来 |
| libcurl（`curl_multi_socket_action`） | C API + 拷贝语义 + 自带 TLS/H2 栈，与流的抽象对不齐 |
| Boost.Beast | 🔍 绑 Boost.Asio，本仓用 standalone asio，需先确认是否可用 |
| cpp-httplib | 阻塞式，装不进协程 |

## 6. 不属于框架的部分

LB 算法 · 主动健康检查 · 重试策略 · 限流 · 缓存 · `Location` / `Set-Cookie` 改写 · 配置语言 · 多机协调。
这些落到用户的"反代框架"里。框架只保证：**不挡路**（流与控制都由用户驱动）＋**语义诚实**（`eof` / 错误 / 取消说准）。

## 7. 与路线图的关系

- **不打断 MU**：按 §1.3 的"功能覆盖不亏欠 = 缺口有名字 + 有挂点"，本轮只把它们记成缺口。
- 属**形态级**、越晚越贵的只有两件：`ByteResponse`（定长可分块）· 请求级取消（若要做）。
- 其余（出站客户端、池、LB…）排在 Stage 2「有界资源」之后 —— 代理的缓冲上限与在途上限就是 Stage 2 的内容。

## 8. 未定

1. **代理在架构里的定位**：❓ "普通用例（只是一个特殊 handler）" vs "独立层"。已查清的一点是：形态上**不需要新执行路径**
   （`ResponseCore::send_head` 的长度与 chunked 都是运行期参数，`IsResponse` 只要求 `attach` + `finish`）。
2. 是否需要**请求级** `cancellation_signal`（代价：keep-alive 语义要重想）。
3. 背压默认值：全缓冲（保护上游、慢客户端友好）vs 直通（省内存）—— 倾向"有界缓冲 + 背压"，与 Stage 2 合并做。

## 来源

- nginx `ngx_http_proxy_module` / `ngx_http_upstream_module` 官方文档（默认值、超时语义、`keepalive`、`max_fails`、`next_upstream`）
- Pingora README + `docs/user_guide/{phase,pooling,failover,graceful}.md`
- 本仓代码：`server/http.cppm` · `server/route.cpp` · `server/route.cppm` · `server/session.cpp` · `server/server.cpp`
