# HTTP 服务器 —— 设计思路（自顶向下）

> ⚠️ **这是笔记，不是决策记录。** 用户明确表示：很多选型**还不了解或不太接受**，随时可能推翻；
> 细节会另开会话专门研究。本文件的作用是**让新会话能接上上下文**，而不是锁定方案。
>
> 状态标记：**✅ 已落地**（仓库里已实现/已验证） · **🟡 当前倾向**（用户倾向但可推翻） ·
> **🔍 待调研** · **❓ 未定**
>
> 📐 **阅读顺序**：本文件刻意**从"要做一个 HTTP 服务器"这个目标出发、逐层向下展开**。
> **线程池不是出发点**，它是 L2 里"有些工作没法异步化"推导出来的结果。

---

## 1. 目标与约束

### 1.1 为什么做这个（🟡 2026-10-13 用户澄清）

**一个服务器、两种极端场景、同一份代码自适应。**

| 消费者 | 场景 | 负载画像（推测） |
|---|---|---|
| **clio**（herodotus 的 MCP 组件，本地） | `127.0.0.1`，MCP 工具调用 | 小请求 · 低延迟 · 连接少 |
| **公网内容供应**（另一个项目） | 对外，真正的生产环境 | 大响应 · 高并发 · 长连接 |

**需求差异极大** ⇒ 这正是**动态调优**存在的理由（大多数开源通用架构不做这件事，见 §7）。
它是本项目的**差异化**，不是收尾工作。⚠️ 但它与“先达到最小可用”有直接张力 —— 解法见 §9 的 **MU**：
**把“挂点”（观测点 / 可调参数 / 指纹）在形态定形时一起留出来，控制器本身晚做。**

最终形态目标：**HTTP/1.1 + HTTP/2 + HTTP/3**（QUIC 那部分“也值得为之建模参数”）。

### 1.2 约束

| 约束 | 内容 |
|---|---|
| **可停机维护** | 服务器**允许停机维护与调参**。这条直接决定了 §7 的"离线 / 在线"分工（见 §7.2） |
| 模块层边界 | `standalone/` **只**做 asio → module 封装；应用另建目录（`AGENTS.md` §1） |
| 环境 | io_uring 恒定启用（用户倾向不做运行期开关）；构建与工具纪律见 `AGENTS.md` |

### 1.3 质量底线：三个“不亏欠”（✅ 2026-10-13 用户表态）

用户不接受“在很多框架已有的地方亏欠”，但这**不能理解成“现在就追上”** —— 否则就是无底洞。翻译成三条可验收的底线：

| 维度 | 什么算不亏欠 | 第一阶段（MU）只需 |
|---|---|---|
| 性能 | **无结构性劣势**：路径零拷贝 · 每请求分配次数可数 · 热路径无 `std::function` 二次间接 | 达到并**记录数字**（不追平 hical） |
| 易用性 | 声明式注册 + **编译期**拦错（含 `param<T,"名字">()` 的名字校对）；形式不满意要**能改**（这正是“形态自由”的用法） | 形态定形（§4.1.1 / §11.2 C） |
| 功能覆盖 | 不是“现在都有”，而是**每个缺口有名字 + 一个明确挂点**（见 §1.5） | 留白清单成文 |

### 1.4 非目标（✅ 2026-10-13 定）

- **不做多机 / 分布式协调** —— 单机多核内做深。留白清单因此**不给** `SO_REUSEPORT` / 分片留位置。
- **但要生产级健壮性**（不是“研究用”）—— 因为公网内容供应要真的对外。
  ⚠️ 这与**有界资源**（现排在 Stage 2）是同一条，不能因为“先到 MU”就丢掉。

### 1.5 留白清单：“不亏欠”的可核查形态

现在不做、但**有名字 + 有挂点**的缺口：

| 缺口 | 挂点 | 为何现在不做 |
|---|---|---|
| TLS | 连接层包装（`Session` 之外） | MU 只服务 clio（本地） |
| WebSocket | `req` 的升级语义（§4.1.1） | 与形态无关，随时可加 |
| 中间件洋葱 | `route_on` 的链式包装 | 等第二个真实用例 |
| 静态 / 大文件响应 | 响应类型（§4.1.1 的 `FileResponse`） | 公网那一轮 |
| 响应压缩 | 响应类型的包装 | 同上 |
| H2 / H3 | §4 已定“单位 = stream / 请求” | 大工程，独立轨道 |
| 指标 / 控制器 | **形态定形时就要留观测点与可调参数挂点** | 留位不实现 |
| 多机 | ❌ 明确不留（§1.4） | — |

### 1.6 上游关系（🟡 2026-10-13 修订）

**clio 只是第一个消费者，而且它尚未施工 ⇒ 框架形态自由。**

> 🔄 修订：本节此前把 clio 的部署约定写成【定】。那些是 **clio 侧**的约定，不是本仓库的约束 ——
> 本仓库只保证“可配置”（`Settings` / `Builder`）。

下表是 clio 侧的用法（供参照，本仓库不写死）：

| 项 | 约定 | 来源 |
|---|---|---|
| 监听 | **只监听 `127.0.0.1`**【定】 | `herodotus/docs/ARCHITECTURE.md` §2.2 |
| 基地址 | `127.0.0.1:7788`【定】（本地开发先用 18080） | `herodotus/docs/PLAN.md` §8.1 |
| 命名 | 库 `herodotus` + 可执行 `clio`；模块 `herodotus.clio.<domain>` | `clio/README.md` |
| HTTP 底座现状 | `clio/cmake/deps.cmake`：**hical 取代 cpp-httplib** | 代码为准（PLAN 表未同步） |

**hical**（[Hical61/Hical](https://github.com/Hical61/Hical)，MIT，v2.7.0，活跃）是**要取代的对象**，
也是灵感来源：Boost.Asio + `asio::awaitable` 协程 + `picohttpparser` 零拷贝 + PMR 三层池
（请求路径零堆分配）+ 中间件洋葱模型 + WebSocket + SSL + SO_REUSEPORT 多 acceptor。
但**现阶段不对标其架构**（2026-10-12 用户决定）——按本文件 L0→L4 分层走。
刻意差异：**standalone asio + C++ module**（hical 用 Boost.Asio）、**`llhttp`**（hical 用 `picohttpparser`）。

**命名脉络**：项目 Herodotus（史家）→ 服务 Clio（卷一缪斯）→ 本实现 **pylai**
（πύλαι，"门/关口"，如 Thermopylae 温泉关字面即"热之门"）——HTTP server 正是系统的**唯一门户**。

---

## 2. 由目标推出的层次地图

要跑一个 HTTP 服务器，必然要依次回答 5 个问题——它们就是 5 层：

| 层 | 要回答的问题 | 在哪 |
|---|---|---|
| **L0 协议语义** | 字节怎么变成"请求 / 响应"？ | §3 |
| **L1 连接与流** | 请求住在哪里？生命周期归谁管？ | §4 |
| **L2 执行与并发** | 请求怎么被执行？需要几个线程？ | §5 |
| **L3 观测** | 我们怎么知道现在好不好？ | §6 |
| **L4 参数与自学习** | 怎么把测出来的东西沉淀下来？ | §7 |
| （横切）内存分配 | 每一层都在分配，边界画在哪？ | §8 |

> 之后是 §9 路线图、§10 未决问题、§11 研究方法。

**起点（✅ 仓库现状）**：`standalone/` 是纯 module 封装层（细节见 `docs/asio-notes.md`）；
`ports/asio/` + `vcpkg-configuration.json` 让 asio 1.38.2 走 overlay port；
根 `CMakeLists.txt` 目前是 `add_subdirectory(standalone)` + `add_executable(main main.cpp)`。

---

## 3. L0 协议语义层：字节 ⇄ 消息

**要解决的**：从连接上读到的字节流，解析成 method / path / header / body；以及把响应序列化回去。

**候选（🔍 全部待评估，用户对选型有保留意见）**

已核实的 vcpkg 可用性（2026-10-12 修订；本地 vcpkg 根已更新至最新 master）：

| 库 | 在 vcpkg | 版本 | 备注 |
|---|---|---|---|
| `picohttpparser` | ✅ | 2026-04-06 | **零分配**：README 原话 *"stateless and does not allocate memory by itself"*，只在调用方 buffer 上设指针；含**原地** chunked 解码。**修订**：官方 registry 已收录（PR #51743，约 2026-05 合入；target `unofficial::picohttpparser`），不再需要自建 overlay port——旧结论“❌”是当时本地根（2026-04-23）落后所致 |
| `llhttp` | ✅ | 9.3.1 | **✅ 已选定（2026-10-12）**。零分配（状态机在调用方内存），回调式；**默认严格校验**（防走私），9 个 `lenient` 开关默认关；内置 `llhttp_should_keep_alive()` / `llhttp_finish()`；vcpkg target `llhttp::llhttp_static` |
| `http-parser` | ✅ | 2.9.4 | 上游**已弃用** |
| `nghttp2` | ✅ | 1.69.0 | HTTP/2；**支持自定义分配器**（`nghttp2_mem`，已核实） |
| `ngtcp2` / `nghttp3` | ✅ | 1.22.1 / 1.15.0 | HTTP/3；传输层与帧层分离，喂字节出回调 |
| `msquic` / `liblsquic` | ✅ | 2.4.8 / 3.3.2 | 各有自己的事件 / 内存模型 |
| `quiche` / `picotls` / `h2o` | ❌ | — | |
| QUIC 的 TLS | — | — | 可复用已有 OpenSSL（本地已装 **3.6.5**，≥3.2），不需要新依赖 |

**候选方向**

- ✅ **HTTP/1.1 解析：`llhttp`（2026-10-12 用户拍板，可推翻）。** 理由：本地 vcpkg 根现成可用；
  默认严格校验（走私防护）+ `lenient` 开关显式可选；keep-alive 判定与 EOF 定界内置；
  回调模型与 `async_read_some` 循环天然契合。`picohttpparser` 留作后路：其"零分配"优势 llhttp 同样具备，
  实际只剩 API 极简（4 函数）与原地 chunked 两点。⚠️ 它自述 *"tiny, primitive"*、**校验宽松**，
  不适合直接面向公网
- 🔍 HTTP/2：`nghttp2`？还是别的？
- 🔍 HTTP/3：`ngtcp2` + `nghttp3`？还是 `msquic`？—— 用户想去新会话专门研究

**为什么这层先做**：它是"请求"这个概念的诞生地；下游所有层（观测单位、参数模型）都以它产出的东西为基准。

#### 3.1 请求体（✅ 2026-10-13 落地）

| 项 | 决定 | 状态 |
|---|---|---|
| 收集 | llhttp `on_body` 追加到 `Request::body`（`std::string`）；**chunked 的块边界由 llhttp 剥掉**，回调拿到的就是载荷 | ✅ |
| 上限 | `Settings::max_body_bytes`（默认 1 MiB，`Builder::max_body_bytes()` 可改）。超限时 `on_body` 返回 `HPE_USER` **就地中止解析** → `ParseStatus::too_large` → **413 + 关连接** | ✅ |
| 为何必须有上限 | llhttp 边收边喂回调，`Request::body` 会跟着客户端一直涨 —— **生产级**明确要"有界资源"（现在排在 Stage 2），没上限就是内存 DoS | ✅ |
| `Expect: 100-continue` | **未做 🔍** —— 不发 `100 Continue`，客户端会先等超时再发体（能用，但多一次往返） | 🔍 |

> 定界由 llhttp 负责（`Content-Length` / chunked / EOF），`consumed` 一直覆盖到**整条消息含体**，
> 所以带体请求与 pipeline 共存不需要额外处理（`tests/http_e2e.py` 有这条断言）。

> ⚠️ **本节将被 §4.1 取代一半**：`on_body` / chunked 剥壳 / 上限 / `ParseStatus` 这些**留下来**，
> 但“框架把整条消息读完再派发”要改成“读到 headers 就交棒”（见 §4.1）。

---

## 4. L1 连接与流层：请求住在哪里

**要解决的**：连接 / stream 的生命周期、读缓冲、写入序列化、超时、背压（flow control）。

**关键：协议变了，"并发"的语义就变了。**（这条比较硬，会影响下游所有设计）

| 协议 | $L$（在途并发）是什么 | 影响 |
|---|---|---|
| HTTP/1.1 | ≈ 连接数（keep-alive 下每连接 1 个在途请求） | 最直观，先做 |
| HTTP/2 | ≈ **stream 数**（一连接内多路复用） | $\lambda$ 变成 streams/s，$W$ 变成 stream 生命周期 |
| HTTP/3 | 同上 + 连接级 / 流级流控 + 拥塞控制 | 多一层"每连接在途字节"的约束；QUIC 加解密 = CPU 密集 → 这时 §5 的 worker 池才真正有意义 |

**由此推出的结论（会影响早期抽象，所以现在就定）**：

> **指标与建模的单位必须建在 "stream / 请求" 层，而不是 "连接" 层**，
> 否则 H2 / H3 接入时整套统计系统要重写。

### 4.1 请求生命周期：框架读到 headers 就交棒（🟡 2026-10-13 定，待落地）

**变更**（用户 2026-10-13）：**handler 自己驱动读体**；写响应改为**框架按响应类型发**；框架只负责“读到 headers + 路由 + 收尾”。

| 阶段 | 谁 | 做什么 |
|---|---|---|
| 读 head | 框架 | llhttp 在 `on_headers_complete` 处 PAUSED（**现停在 `on_message_complete`**，`http.cpp:110`）→ method / target / headers |
| 路由 | 框架 | 只看 method + path，**体不参与**；404 / 405(+`Allow`) / OPTIONS 自动 204 仍由框架答 |
| 交棒 | 框架 → handler | 构造两个形参：typed 请求 `req` + 响应对象 `res`，然后调用 handler |
| 读体 | handler | `co_await req.body()` / `co_await req.read_some(buf)`；chunked 的块边界仍由 llhttp 剥（不变） |
| 写响应 | **框架** | handler 只**填** `res`；返回后由框架发 —— **怎么发由响应类型决定**（见 §4.1.1） |
| 收尾 | 框架 | **未读的体 → `Connection: close` 并关连接**（用户 2026-10-13 定）；读完则保持 keep-alive |

**为何“读写都得交出去”**：先路由再读体，本身就意味着体不能再由框架预先读完（§3.1 的“读完再派发”因此失效）。
读体必须走**同一套**带超时/取消的原语（现有包装在 `session.cpp` 的 `fill_buffer` / `write`），否则 handler 能绕过超时。

#### 4.1.1 handler 签名：声明搬到第一个形参（✅ 2026-10-13 定形，待落地）

```cpp
asio::awaitable<void> handle(Get<"/health"> req, FileResponse& res);
```

| 变更 | 后果 |
|---|---|
| 动词 + pattern 从**返回类型**位置挪到**第一个形参** | `Get<P>` / `Post<P>` 变成“这条路由的**请求类型**”，不再是协程返回对象 |
| ✅ **随带的大简化** | 返回类型自由后，`Endpoint` 不再是协程返回类型 → `route.cppm` 尾部那个 `std::coroutine_traits` 特化、`release()`、以及绕 `await_transform` 模板推导的注释**全部可以删掉**（这是整个 DSL 里最拐的一处） |
| **响应类型多种**（beast 式） | handler 用形参**声明它产出哪种响应**；“流式”不再是一个 `stream()` 方法，而是**一种响应类型**（`FileResponse` 自带长度 → 仍可算 `Content-Length`；只有真正未知长度的才需要响应侧 chunked 或 `Connection: close`） |
| `Param<>` 位置注入 | 🔄 **2026-10-13 已删** —— 换成 `req.param<T, "名字">()`（声明搬到形参后，请求对象本身就能带捕获）⇒ `Param` / `ParamCount` / `SlotOf` / 名字校对一并删掉；名字改为在**调用点**对照 pattern 校对 | ✅ |
| `RouteOf` | 改从 `Ps...[0]` 取 `::method` / `::literal`（原先从返回类型 `R` 取） |
| **`req` 的语义** | ✅ **请求本身**，不是标签 → 读体接口就挂在它上面（`req.body()` / `req.read_some(buf)`），**不需要额外的 exchange 形参** |
| **写响应** | ✅ **框架发**：handler 只**填** `res`，返回后框架发；**发法由响应类型决定**（下一格） |
| 返回类型 | ✅ 定为 `asio::awaitable<void>`（响应走形参；**不做**“两种应答风格并存”）| ✅ |
| `res` 的所有权 | ✅ **按引用传**（用户 2026-10-13）—— 对象由框架持有，handler 往里填；于是“框架发”才成立（按值传的话副本随协程帧一起销毁） | ✅ 定 |

**第一阶段要立的响应类型**（用户 2026-10-13 定，累积读）：

| 类型 | 体 | 长度来源 | 怎么发 |
|---|---|---|---|
| `StringResponse` | 内存里的 `std::string` | `body.size()` | 常规：`Content-Length` |
| `EmptyResponse` | 无 | 0 | 204 / 304 这类 |
| `FileResponse` | 文件 | 文件大小（`stat`） | 常规：`Content-Length`，可分块读发 |
| `UnknownLengthResponse` | 由 handler 分段喂 | **未知** | **响应侧 chunked** |

> ⚠️ 响应侧 chunked 的坑：**HEAD 不带体但 `Content-Length` 照 GET**、**204/304 既不得带体也不得 chunked** ——
> 这两种都不能走 chunked 分支，最好在“选响应类型”这一层就**编译期**拦下。

> ⚠️ 本节**取代**先前“一个 `Exchange&`”的答案：读侧在 `req`、写侧在 `res`（**按引用**）；也取代“响应要不要流式”那个设问——
> **流式是一种响应类型**，不是一个方法。尚存一个 🟡：返回类型。

**由此产生的硬约束**：

1. **未读体** → 不替客户端 drain，直接 `Connection: close`（已定）。
2. **响应定界**：**由响应类型决定**（表见 §4.1.1）；`Connection` 头仍由框架写；204/304/HEAD 不得走 chunked。
3. **handler 没填响应怎么办**：框架发什么？（空 `200` / `500`）—— 因为“发”归框架了，“忘了 send”这个失败模式消失，但要给“什么都没填”一个默认 🟡。
4. **multipart**：不是 llhttp 的活，是 body 之上的**增量**解析器。先只给全量 `co_await ex.body()`（受 `max_body_bytes` 约束），流式与 multipart 留到有真实需求。

> 已验证可沿用：`Parser` 的暂停机制（`http.cpp:170` 把 `HPE_PAUSED` 翻成 `ParseStatus::complete`）——
> 只需把暂停点从 `on_message_complete` 换到 `on_headers_complete`，并让“继续喂字节”变成 handler 可驱动的入口。

**本层还要决定的**（❓ 尚未展开，可在新会话补）：

- 读写缓冲的归属与大小（与 §8 的分配器边界耦合）
- 超时策略（空闲超时 / 请求超时 / 写超时）——它同时是 §5.5 的准入手段
- 背压：什么时候停止从 socket 读（H2/H3 有显式流控，H1 只能靠停读）

---

## 5. L2 执行与并发层

**要解决的**：请求拿到之后，怎么被执行、用几个线程、排队怎么办。

### 5.1 先问：真的需要线程吗

asio 的模型是 **Proactor**（官方章节标题就是 *"Concurrency Without Threads"*）。官方原话：

- *"all threads that have joined an io_context's pool are considered equivalent, and the io_context may
  distribute work across them in an **arbitrary** fashion."*
- *"applications do **not** need to spawn many threads in order to increase concurrency."*
- *"…avoid the cost of context switching by **minimising the number of operating system threads —
  typically a limited resource**…"*

→ **纯异步路径下，1 个（或每核 1 个）事件循环线程就够**，并发应该存进廉价的状态（连接 / stream 状态机）。

### 5.2 事件循环（io_uring）

机制细节（scheduler + `scheduler_task`、全局 `op_queue_`、handler 在**锁外**执行、
`interrupt()` = nop SQE、ring 配置项等）记录在 **`docs/asio-notes.md` §3-§4**，不复述。

这里只需记住两条：
- **只有一个全局队列**，多线程"抢队首、解锁、执行"，**没有亲和性、没有 work stealing**；
- 所以"加线程"并不会让 I/O 更快，只会增加争用。

### 5.3 那什么时候才需要 worker 池

只有当某类工作**没法异步化**时：

- **CPU 密集**：TLS 握手、QUIC 加解密与包处理、压缩、JSON / 业务计算；
- **真阻塞**：无法改造的同步依赖（同步 DB 驱动、阻塞文件调用）。

→ **worker 池是"异步化失败的兜底"，不是默认选项。** 这也解释了为什么 §4 说
"HTTP/3 的 CPU 工作让 worker 池终于有意义"。

### 5.4 池尺寸：用公式，不用倍数

**Little's Law 的正确用法**

$L = \lambda W$ 是**恒等式**，只说"系统里同时有多少请求"，**不含"该开几个线程"的结论**。
必须分开三件事：**并发度 $L$**（存进廉价状态）、**并行度 $c$**（线程，贵）、**排队**（$W_q$，危险）。

**上下界**

- 下界（吞吐可行性）：$n \ge \lambda \cdot T_{occupy} / \rho$
  —— 用**占用 worker 的时间**，**不是**端到端时延（后者含排队，会把自己排队的时间也算成工作量）。
- 上界（并行度天花板）：$n \le \text{cores} \times U \times (1 + W_{block}/C_{compute})$
  —— **超订只被阻塞时间授权**；纯 CPU 时退化为 cores。

**$\rho$ 是尾延迟旋钮（Kingman 近似）**

$W_q \approx \frac{\rho}{1-\rho}\cdot\frac{c_a^2+c_s^2}{2}\cdot S$：
$\rho=0.8$ 时排队已是 $4S$，$\rho=0.9$ 时 $9S$。
→ **尾延迟不是靠加线程买的，是靠主动留空 $\rho$ 买的**；服务时间方差越大越要留空。

**用可扩展性模型（USL）代替拍脑袋**

$X(N) = \lambda N / [1 + \sigma(N-1) + \kappa N(N-1)]$：
$\sigma$ = 争用项，$\kappa$ = 一致性项（**拐点后吞吐反而下降**）。
这解释了为什么 `2N` 有时是负优化——**拐点位置才是一切**；拟合 $\sigma,\kappa$ 可**解析**给出最优并发。

> 背景：`asio::thread_pool` 默认 = `hardware_concurrency() * 2`，**官方从未解释过理由**
> （详见 `docs/asio-notes.md` §5）；社区口径 `N` / `N+1` / `2N` 都建立在"线程会阻塞"的前提上。

### 5.5 排队与准入控制（比"动态加线程"更重要）

- **有界队列 + 显式拒绝**（429 / 503）。$W$ 无界 → 队列无界 → 内存无界。
- **按在途并发限流**（信号量），而不只是按速率——这是把 Little's Law **当限流器**用。
- **端到端超时**：对 $W$ 设上界 → 给 $L$ 上界。**最便宜、最有效的一道闸。**
- 顺序：**先限流 / 超时，再谈动态扩缩。**

### 5.6 动态性（控制器）

- 输入：$\lambda$、$T_{occupy}$ 直方图、队列深度、CPU%、$W_{block}/C_{compute}$（全部 EWMA 平滑）。
- 规则要点：**队列涨 + CPU 未饱和 → 加线程；CPU 已饱和 → 不加（改了更糟）；队列空 + 线程闲 → 慢减。**
- 工程约束：**迟滞 + 死区 + 冷却 + 硬边界**（$n_{min}$、$n_{max}$）。
- ⚠️ 危险：CPU 密集时加线程是**正反馈**（上下文切换 → $S$ 变长 → 队列更长 → 再加），
  所以 §5.5 的闸门必须先有。

---

## 6. L3 观测层：我们怎么知道现在好不好

**观测点必须落在应用层**——`asio-module` 不知道"请求"是什么（`AGENTS.md` §1）。

| 能观测 | 只能从 OS 取 | **观测不到** |
|---|---|---|
| $\lambda$、$T_{occupy}$、队列深度、拒绝率、错误率、按请求类别的量 | `/proc/self/stat`、`getrusage`、cgroup `cpu.stat`、线程数 | io_uring ring 压力等内部量 → 只能**间接推断** |

**采什么**

- **计数器 + 直方图**（不是随机时刻采样）：Little's Law 只要均值（计数器够），
  尾延迟控制要分位数（必须有直方图，对数分桶）。
- 热路径开销：分片计数器 + 松弛原子，周期合并；**并且要测观测层自己的开销**。
- 归因：指标必须带**请求类别**维度（聚合值会骗人）。

**产出**：周期性摘要报告 + 冻结模式（参数全显式、结果可复现）。**这一步之前不许有任何自适应逻辑。**

---

## 7. L4 参数与自学习层：把测出来的东西沉淀下来

### 7.1 参数分层（骨架）

| 层 | 例子 | 生效时机 | 是否跨运行持久化 |
|---|---|---|---|
| 冷 | `io_uring_ring_count` / `ring_size` / `submit_batch_size`、预分配量、`heap_reserve` | 仅启动 | ✅ 这是真正的 "PGO 层" |
| 温 | worker 池 `n_min/n_max`、$\rho$、队列容量、超时 | 分钟级在线 | ✅ 仅作**先验** |
| 热 | 瞬时 $n(t)$、spin 时长、提交批量化时机 | 秒级 | ❌ **绝不持久化**（重新测很便宜，持久化反而带回坏状态） |

> ⚠️ 这张表的**分层依据已在 §7.2 修订**（"允许停机维护"改变了判断）。

### 7.2 修订：允许停机维护后，PGO 类比要重新评估

> 背景：我最初是在"**不能停机**"的假设下给结论的，所以偏保守
> （原话：*"只有冷层值得跨运行记忆"*、*"只能在线闭环"*）。
> 用户指出：**我们的服务器可以停机维护和调参**。于是判断要改。

#### 变化点

1. **"能否在线改"不再限制"能否实验"。**
   重启才生效的参数（`io_uring_ring_count` / `ring_size` / `submit_batch_size`、预分配量、分配器参数），
   现在**可以被主动搜索**——用真实重启来试，而不是只能当"先验"。

2. **拿到了 PGO 最大的好处：干净归因。**
   重启 = 状态归零 → 每次实验从同一初始状态出发 → 参数差异可归因。
   这正好解掉我之前提的"混杂效应 / 一次只能动一个维度"的顾虑。
   （仅当**每次实验都重启**时成立；在线调参仍然混杂。）

3. **⚠️ 但 PGO 真正的难点没被解决：代表性负载。**
   停机维护窗口里**通常没有真实流量**。
   - 若窗口内能**重放生产流量**（录制回放）或用**与生产校准过的生成器** → 真 PGO 流程可行。
   - 若不能 → 窗口内测出的 $\lambda / W / S$ **不代表生产**，真实负载画像仍得靠在线测量。
   → **这一条比"能不能重启"更决定成败。**

4. **⚠️ 新引入的方法学陷阱：短窗口 ≠ 稳态。**
   重启后缓存冷、连接在爬坡、分配器池 / 定时器堆 / ring 都没填满。
   - 必须丢弃预热期；
   - 更要紧：**长期累积效应**（内存碎片、池增长、哈希冲突、fd 增长）在几分钟窗口里根本显现不出来
     → **离线窗口会系统性低估 USL 的 $\sigma$ / $\kappa$**。
   → 离线扫参**适合快速、无状态、效果立刻可见的参数**；对"随运行时长累积"的参数**不可靠**。

#### 修订后的分工（新结论）

> **离线（PGO 形态）负责"找起点 / 结构"；在线（控制形态）负责"治窗口之间的漂移"。**
> **两者是分工，不是二选一。**

| | 离线（维护窗口 / PGO 形态） | 在线（运行期 / 控制形态） |
|---|---|---|
| 目标 | 找到**结构参数**与"负载档位 → 配置"表 | 吸收**窗口之间**的负载漂移 |
| 手段 | 真实重启 + 扫参 / 拟合模型 | 慢速闭环 + 迟滞 + 护栏 |
| 归属参数 | 需重启才生效、效果短期可见的 | 可在线改的（池上下界、队列容量、超时） |
| 产出 | **固化到部署配置** | 跨运行只存**先验** |

#### 另外三点

- **窗口预算是硬约束**：一次实验的固定成本 = 停机 + 启动 + 加载 + 跑够时长 + 丢预热。
  → 实验次数有限 → **更该"拟合模型 + 少量点"，而不是穷举网格**
  （由 §5.4 的 USL，3~5 个点就能定 $\sigma$、$\kappa$）。
- **跨窗口累积**：每个维护窗口贡献几个测点，模型跨窗口 / 跨月改进——这正是 PGO "profile 随构建累积"的形态。
  窗口之间负载档位不同 → **指纹**在这里才真正发挥作用。
- **版本失效**：跨窗口若升级了 asio / 编译器 / 内核，老测点可能作废（等价于 PGO profile 跨版本失效）。

#### 对原结论的修正（诚实标注）

| 原结论 | 修正为 |
|---|---|
| "只有冷层值得跨运行记忆" | 冷层**不仅该记忆，还可以被主动搜索**；分层依据从"生效时机"改为 **"改它是否需重启" × "效果是否需要真实/长期负载才显现"** |
| "PGO 类比只有一半成立" | 在**允许停机 + 能重放流量**的前提下，类比**大部分成立**；剩下不成立的两点：**(a) 负载非平稳**（→ 需要在线那一半）、**(b) 最坏情况是宕机**（→ 需要护栏） |

### 7.3 安全与目标函数

- 存 **"指纹 → 参数"**，不是单一答案；指纹 = 负载画像（类别构成 / 大小分布 / keep-alive 比 /
  **负载档位** / 核数与 cgroup 配额）。
- 档案带 **schema 版本 + 构建 id + 硬件指纹**；不匹配即失效。
- 必须消费 **预测误差**（模型说"该提速 20%"却只提了 2% → 指纹失配 / 负载漂移）。
- **目标函数交给用户**：机器出 Pareto 前沿（吞吐 vs p99 vs CPU-秒/请求），人选点。
- 安全：**护栏 + 限步 + 灰度 + 劣化自动回滚 + 冻结开关**。没有冻结模式就无法做可复现压测。
- 同时自适应的参数**不超过 3~4 个**。
- 纪律（一次只动一个维度、丢弃过载窗口数据等）见 `AGENTS.md` §5；
  注意 §7.2 给出了"重启即可干净归因"的例外。

---

## 8. 横切：内存分配

**用户计划**：`mimalloc` + `pmr`（🔍 具体接入方式与"分配器边界"待定）。

为什么单独成节：**L0 的解析器选型、L1 的缓冲、L2 的任务对象、L3 的直方图桶，全都在分配。**
选 `llhttp` 的取舍之一正是它**不分配**（状态机在调用方内存，分配权完全留给我们的 pmr / mimalloc）。

需要回答的问题（❓）：
- 分配器边界画在哪一层/哪个对象？
- 每连接 / 每 stream 用 `pmr::monotonic_buffer_resource`（arena）还是全局堆？
- 与 `asio` 内部的 `recycling_allocator` 如何共存（asio 有自己的小对象回收）？

---

## 9. 路线图（🟡 草稿）

| 阶段 | 内容 | 验证 |
|---|---|---|
| **0** | **协议基线**：单 io_context + io_uring，固定线程数，keep-alive，超时（空闲 / 写）。**无指标、无自适应、无 TLS**。<br>🟡 **进行中**<br>✅ 已跑通：llhttp 解析（pipeline / 半包 / 400 / 431）、keep-alive、空闲与写超时、**取消传播**（每连接独立 `cancellation_signal` + `plf::hive` 注册表，见 `asio-notes.md` §7）、SIGINT 优雅停机、io_uring 后端（实测 `anon_inode:[io_uring]`）、**声明式路由**（`route_on` + 编译期冲突检查 + 每 method 一棵树，见 §11.2 C）、**协议层应答**（405/OPTIONS 的 `Allow`、HEAD 复用 GET，见 §11.2 G）、**请求体**（llhttp `on_body` + 上限 → 413，见 §3.1）<br>⚠️ 其中**声明式路由 / 协议层应答 / 请求体**三项会被 Stage 1 **重新打开**（形态变更） | 能稳定服务（外部基线见 Stage 2） |
| **1** | **框架形态定形**：交棒（handler 读体，§4.1）· 响应类型多种（§4.1.1）· 擦除点合并（§11.2 C）· **留出观测点 / 可调参数 / 指纹挂点**（§1.1）· `routing.md` 重写 | e2e 全绿；接口能承担 clio；文档与实现一致 |
| → **MU** | 🎯 **最小可用 = Stage 1 完成即达**。只服务 clio（本地、无 TLS、小请求） | C++ 模块可被 `import` 消费；拿去 clio 试用 |
| **2** | **有界资源与稳定性验收**：有界队列 / 在途上限 / 准入控制（固定值即可）+ **外部压测 60s 基线** | 60s 无泄漏无挂死，记录基线 |
| **3** | **只测不调**：按 §4 的结论建在 "stream/请求" 层的指标；请求级直方图 + 计数器 + OS 侧；周期报告；**观测开销自测**；冻结模式 | 指标稳定；开销 < 1~2% |
| **4** | 动态池：§5.4 上下界 + EWMA/死区/迟滞/冷却 + 护栏/限步/回滚/冻结。引入 TLS 作为可控 CPU 密集负载 | 阶跃负载收敛性；固定 N（含 2N）vs 动态；**CPU 密集 vs 阻塞型各跑一遍** |
| **5** | **离线扫参 + 跨窗口累积**（利用允许停机维护）：维护窗口内重启扫参 → 归档 / 固化；指纹 + 版本化；冻结 / 重置 / 打印 | 第二次启动"进入良好区间所需时间"缩短；模型跨窗口改进（见 §7.2） |
| **6** | 建模与 Pareto 报告：拟合 USL，输出前沿；预测误差作漂移报警 | 用未参与拟合的负载档位检验预测误差 |
| **7** | **HTTP/3 独立轨道**：包 ngtcp2/nghttp3 + OpenSSL；另建连/流级流控与拥塞控制参数模型 | — |

> 🎯 **MU（最小可用）里程碑**（✅ 2026-10-13 定义）：**形态定形完成**（§4.1.1 / §11.2 C）+ e2e 全绿 +
> C++ 模块可被 `import` 消费 + 已知缺口成清单（§1.5）。
> **只服务 clio**（本地、无 TLS、小请求）；公网需求（TLS / 大响应 / 高并发）只进留白清单。
> 到达 MU 后先拿去 clio 试用，把情况带回来再定下一步 —— 这是打破“想法一直加、迟迟不可用”的那把钥匙。
>
> ✅ **编号已定（2026-10-13）**：`Stage 0` 原先混了**两根轴** —— **协议基线**与**框架形态**；已被 ✅ 的三项
> （声明式路由 / 协议层应答 / 请求体）会被形态变更**重新打开**，所以“Stage 0 快完成”是错觉。
> 现拆开：**形态定形 = Stage 1**（MU 依赖它），**有界资源 + 外部基线 = Stage 2**（MU 之后）；原 1~5 顺延为 **3~7**。
> 理由：**观测的单位就是请求生命周期** —— 形态不定就建指标必返工（同 §7 的“挂点要提前留”）。

---

## 10. 尚未决定（❓）

1. **应用层目录名**：✅ **已定：`server/`（2026-10-12）**。用户说"建一个新文件夹，现在 `main.cpp` 的内容抛弃"；候选曾为 `app/` / `server/` / 其他。
2. **入口归属**：✅ **已定（2026-10-12，🔄 2026-10-13 修订）**：根 `main.cpp` 删除，入口归**应用层**。
   当时放在 `server/` 并让 `pylai` 是可执行文件；**现修订**为：`server/` 只出**静态库 `pylai`**（框架，无 `main`），
   示例应用 + 入口在 `tests/`，可执行 target 名 **`app`**（πύλαι 这个命名留给框架库）。
3. **HTTP 解析库选型**：✅ **已定：`llhttp`（2026-10-12，可推翻）**；`picohttpparser` 为后路（§3）。
4. **HTTP/2、HTTP/3 的技术栈**（§3）。
5. **内存方案**：mimalloc + pmr 的接入方式与分配器边界（§8）。
6. **参数档案存放位置**：`build/`（已 gitignore）还是独立配置路径？
7. TLS 何时引入（当前倾向：**“动态池”那一阶段**，当 CPU 密集负载用 —— 不写阶段号，免得重编号时又过期）。
8. io_uring：用户倾向**不做运行期开关**（恒定启用）。⚠️ 若 WSL2 上 io_uring 异常则无退路；
   缓解：那一行 `target_compile_definitions` 随时可改（"不做开关"≠"关不掉"），但那就属于改模块层。
9. **维护窗口的形态**（❓ 直接影响 §7.2）：窗口内**能否重放生产流量**？窗口多长、多频繁？
   —— 这是"离线 PGO 形态能否成立"的前提。
10. ✅ **§1.3 的非目标已定（2026-10-13）**：不做多机；**要**生产级健壮性（见 §1.4）。
11. **路由层形态**：✅ **已定（2026-10-12）**
    - 现有 `App` + `Router`（监听 / 接受 / 会话注册表 / 事件循环 / 停机）**合并为 `Server`**，
      用 **builder 模式**构造；`Router` 这个名字腾出来专做**路由**。
    - handler 形态：🔄 **2026-10-13 修订** —— 不再是 `awaitable<Response>`。现为：声明搬到第一个形参、`res` **按引用**、响应**由框架按响应类型发**（见 §4.1.1）。
    - 匹配能力：🟡 **已重新定案（2026-10-12 晚）**——不再走"Stage 0 精确匹配 → 将来再演进"的
      渐进路线，改为**一步到位**：字面量 pattern + `constexpr` 建树 + 路径参数编译期注入。
      **完整定案见 §11.2**；旧的渐进结论（§11.1）已作废。

---

## 11. 路由设计

> 本节是**独立于 §10 的正文**：§10 记“还有什么没决定”，这里记“路由这块决定了什么”。
> **11.1 是已作废的旧调研**（保留作历史留档），**11.2 是当前定案**。

### 11.1 路由匹配的调研结论（2026-10-12）

> ⚠️ **修订（2026-10-12 晚）：本节整体已被 §11.2 推翻，保留作历史留档。**
> 它记录的"不引 `std::regex`"这一步仍然成立；但"Stage 0 用 `unordered_map`、
> 将来再演进到分段匹配 / radix tree"的**渐进路线已废弃**——改为一步到位。**以 §11.2 为准。**

以 `cpp-httplib`（16.9k★，同类最主流）为参照：

| 它的能力 | 做法 | 对我们 |
|---|---|---|
| 精确匹配 `svr.Get("/hi", …)` | —— | ❌ **已移除**（2026-10-13）—— 那时曾拿它做 Stage 0，现改用声明式路由 |
| 正则路由 `R"(/numbers/(\d+))"` | `std::regex` | ❌ **不用** |
| 路径参数 `"/users/:id"` → `req.path_params.at("id")` | **不走 `std::regex`** | ✅ 将来走这条 |

⚠️ 它对正则路由的自我警告值得记住：`std::regex` 匹配长输入会**爆栈**（连 `.*` 都会），
所以它给正则路由加了路径长度上限（`CPPHTTPLIB_REGEX_ROUTE_PATH_LENGTH`，256），
并明确写着 *"Path parameters are matched without `std::regex` and have no such limit, so prefer them"*。

**所以道路是**：精确匹配 → 分段路径参数（**自己按 `/` 切段比较，不引 `std::regex`**）→ 必要时再上 radix tree。
**不引 regex，这两条路都不用推倒重来**：

- Stage 0：`unordered_map`（key = method + path，异构查找避免为查询构造 `std::string`）；
- 加参数时：换成分段匹配或 radix tree —— **只在 `Router` 内部动，接口不变**。

---

### 11.2 路由设计定案（2026-10-12 晚）

> 🟡 **性质**：下列每一条都是用户**逐项拍板**的结果；仍有未落地项，逐条在对应小节标了状态。
>
> 📌 **进度（2026-10-13 晚）**：**A / B / C / D 的主体已落地并端到端跑通** ——
> 代码在 `server/route.cppm`（`pylai::route`）：A 节全部语法（含 `{rest...}` / `{$}` / 尾斜杠子树 / 6 类错误）、
> 字符级压缩 radix 匹配、声明式 `route_on` 注册与 `req.param<>()` 取参；接线在 `server/route.cpp` / `server/server.cppm`。
> 编译期用例在 `tests/route_static.cpp`（跟着 `cmake --build` 一起求值）。
> 验证方式：`cmake --build --preset debug_llvm`（`static_assert` 与用例每次构建都会跑）；端到端证据见 §11.2 C 的落地进度块。

**一句话**：`Get<">...">` 写在**第一个形参**上声明一条路由（它同时就是请求） → **`constexpr` 构建器**在编译期解析 pattern、
把整包压成**一棵字符级 radix tree** → 运行期持表匹配；路径参数在 handler 里用 `req.param<T, "名字">()` 取，
**注册点**（`Builder::route_on`）做 pattern ↔ handler 的编译期交叉核对（合法性与冲突）。

```cpp
using namespace pylai::route;

struct App
{
	asio::awaitable<void> health(Get<"/health"> req, http::Response& res);
	asio::awaitable<void> user(Get<"/users/{id}"> req, http::Response& res);
};

App app;
auto server = pylai::Server::builder()
    .bind("127.0.0.1", 18080)
    .route_on(&app, &App::health, &App::user)
    .build();
```

#### A. 语法

| 项 | 决定 | 状态 |
|---|---|---|
| 路径参数 | `{name}`；字面量花括号写 `{{` / `}}` | 🟡 |
| catch-all | `{rest...}`（Go 1.22 写法）；**只允许出现在末尾**，中间出现 → 报错；**至少吃掉一段**（`/files` 不命中 `/files/{p...}`，`/files/` 命中且 `p=""`） | 🟡 |
| 参数名 | 任意非空（不含 `{` `}` `/`）；**同一路由内不可重名** | 🟡 |
| 尾斜杠 | `/a` 与 `/a/` **不同**；采用 **Go 1.22 语义**：路径**以 `/` 结尾 = 匿名 `...` 通配（子树匹配）** | 🟡 |
| `{$}` | 保留：`/a/{$}` 只匹配 `/a/`，用来"钉住到此为止"（对应 Go 的 `{$}`） | 🟡 |
| 百分号解码 | **逐段解码**；`%2F` **不作为段分隔符**（Go 1.22 行为） | 🟡 |
| regex 路由 | **不做** | 🟡 |
| 段内前后缀（`img-{id}.png`） | **不做** —— 🔄 2026-10-13 重新评估后**仍不做**，理由见下 | 🟡 |

> **段内前后缀为什么不做（2026-10-13 重新评估）**
> 用户一度要求加上（"段内前后缀可能很有用"），语义也已定（一段内最多一个参数；**非贪婪**、失败再放宽；`{x...}` / `{$}` 不进段内）。
> 但推冲突规则时发现了**无法静态消解**的歧义：
>
> | 两条路由 | 请求 | 结果 |
> |---|---|---|
> | `img-{id}.png` + `img-{name}.jpg` | `img-a.png.jpg` | **两条都命中**（前者 `id="a"`，后者 `name="a.png"`） |
>
> 根因是**段级安全性不可推广**：段级之所以安全，靠的是"**段文本不同 ⇒ 匹配集合不相交**"（段是对齐单位）；
> 段内参数打破它 —— **参数可以跨过后缀**，于是"后缀字面量不同"再也证明不了不相交。
> 只剩**前缀**还能分隔（且要**互不为前缀**，否则 `img-{x}` 与 `i{z}` 也会撞），那会把 `img-{id}.png` + `img-{name}.jpg` 这类组合一律拒掉 ——
> 表达力没增加多少，规则却要分叉（"后缀更长者优先"在后缀等长时仍定不出序）。
> **结论：不做。** 若将来真需要，从"前缀能分开"那一类用例重新评估。

#### B. 匹配策略

| 项 | 决定 | 状态 |
|---|---|---|
| 优先级 | **静态优先**：静态边 → 参数边 → 通配边 | 🟡 |
| 冲突定义 | **🔄 修订（2026-10-12 晚，核实 Go 源码后）**：**"分歧点上必须有字面量"**。<br>分歧点是**字面量** → 它 rank 最高、一眼看得出谁赢 → **放行**（如 `/a/{x}/c` + `/a/b/{y}`）<br>分歧点是**两个通配符** → 没有字面量可定优先级 → **报错**（如 `/a/{x}` + `/a/{y...}`、`/a/{y...}` + `/a/{$}`）<br>公共前缀完全同形**且等长** → 落在同一节点、后注册覆盖前一个 → **报错**（重复注册 / 同形异名 / `/files/` 与 `/files/{p...}`）<br>⚠️ 原记的"照抄 Go 子集规则"**已作废** | 🟡 |
| 与 Go 的差异 | ✅ 已核实（`src/net/http/pattern.go` 的 `compareSegments` / `combineRelationships`）：Go 用**"谁更具体"**定序，于是 **放过** `/a/{x}`+`/a/{y...}`（`s2.multi` → `moreSpecific`）、**拒掉** `/a/{x}/c`+`/a/b/{y}`（`moreGeneral` ∧ `moreSpecific` → `overlaps`）。**我们恰好相反** —— 因为我们靠**字面量**定序，不靠"谁更具体" | ✅ |
| 报错时机 | ❌ **不需要运行期报错**——用户判断：**类型信息在编译期，错误的路由表无法通过编译** | 🟡 |

> ⚠️ 最后一条的成立前提见 §F 风险 2。

> **实现进度（2026-10-13 晚④，`server/route.cppm`，1391 行 · 104 条 `static_assert`）**
> - **段级视图**（校验 + 参数提取）：`Segment` / `PatternView` / `Pattern<S>` / `params()` ✅
> - **段级冲突检查**：`segments_disjoint` / `routes_conflict` @287 / `RouteConflict` @306 / `find_table_conflict` @680 ✅
> - **字符级压缩 radix**（运行期匹配）：`RadixKind` @320 · `RadixEdge` @328 · `RadixNode` @336 · `RadixTree` @344 · `Capture` @354 · `PieceCursor` @391 · `insert_pattern` @515 · `build` @599 · `match_at` @606 · `match` @667 ✅
> - **每 method 一棵树** ✅（2026-10-13 晚②）：`SelectPatterns` @838 按动词筛 pattern → 每个动词一棵 `build_of()`；
>   `VerbRoutes` @1033 / `add_verb_routes` / `make_routes` @1074（各树擦成闭包，类型各异也能同放一个 vector）
>   —— `RouteGroup` 包装层**已删**（2026-10-13 晚③）：`Router` 直接持有 `std::vector<VerbRoutes>`（一个成员，不再是“装一个元素的 vector”）
> - **声明搬到第一个形参**：`Request<verb,pattern>` @756（`slot_of` @764 · `param` @780）· `IsRouteRequest` @824 ·
>   `RouteOf` @833 · `validate_route` @917 · `make_arg` @954 · `make_handler` @978 · `VerbRoutes` @996（`server/route.cppm`）✅
> - **`pmf` 类型萃取改用 jk 的 `Callable`** ✅（2026-10-13 晚④）：`RouteOf<Pmf>` 不再手写偏特化，形参表由 `std::tuple` 换成 `TList`
>   → **`ParamSlot` 那条递归整块删掉**（`SlotOf<i, L> = ParamCount<L::Slice<0, i>>`）。白拿 cv / ref / noexcept / 变参全组合
>   （反证：旧写法对 `const` / `noexcept` 成员指针报 "implicit instantiation of undefined template" —— `.vscode/tmp/jk-callable-probe/old-trait.cpp`，`-DWHICH=1/2`）。
>   ⚠️ 代价：`:route` 现在 `import jk;`（见 `modules.md`），且 `RouteOf<Pmf>::Params` 对外是 `JK::Meta::TList`。
> - **组级冲突检查（已接线）** ✅：`GroupClash` @942 + `validate_group` @953；冲突时实例化未定义的 `ShadowedRoute<两条 pattern>`
>   → **编译错误里直接点名**（实测：`ShadowedRoute<FixedString<7>{"/items"}, FixedString<7>{"/items"}>`）
> - 编译期用例：`tests/route_static.cpp`（`selftest` · `dsl_selftest` · `conflict_selftest` · `match_selftest`，一共 99 条 `static_assert`）✅
>   🔄 **2026-10-13 已从 `route.cppm` 搬出** —— 走 `import pylai;` 的**导出面**，顺带验收“该导出的都导出了”；
>   `Table` / `On` / `table_selftest` **已删** —— `validate_group` 把它接进了真实注册路径，独立的"手工路由表"没用了
> - 验证通道：**`tools/clice query`**（`--fresh` = 先重索引/重编译再回答）。命名检查才用 `clice lint`。
>   `.clang-tidy` 已用 `HeaderFilterRegex` + `ExcludeHeaderFilterRegex` 把范围收窄到本工作区。
> - **协议层应答（已落地）** ✅：405 / OPTIONS 带 `Allow`（GET 存在时隐含 HEAD）、HEAD 复用 GET 且无体（见 §G）
> - **未落地 🔍**：运行期重建/切换活跃集合 · `param<T, "名字">()` 的**转换失败语义**（现 `from_chars` 失败静默给 0）

#### C. 结构与实现

| 项 | 决定 | 状态 |
|---|---|---|
| **粒度分工** | **🔄 修订（2026-10-12 晚，用户明确）**：**两阶段两种粒度**。<br>**编译期**（校验 + **参数提取**）= **段级**：用 `PatternView` 的段序列跑冲突检查（"分歧点上有没有字面量"本来就是段级概念），并**按出现顺序提取命名捕获**（`Pattern<S>::params()`，供给 `req.param<>()` 算下标）。<br>**运行期**（请求路径匹配）= **字符级压缩**（radix）：边是字符片段，对请求路径做字符串匹配。<br>✅ 已按此**删除**原先的段级运行期匹配器 | 🟡 |
| 树形 | **每 method 一棵字符级压缩 radix tree**（⚠️ 不是段级 trie） | ✅ |
| 建树方式 | **`constexpr` 构建器**：把各路由的**段序列**压成**字符级** radix；冲突检查在段级完成。方法论借自 `ctre`（用 `constexpr` 函数而非模板元编程） | ✅ |
| 边的存储 | label 全部是某个 pattern 的 `raw` 上的**切片**（静态存储）→ **零拷贝**；分叉时只把切片一分为二，不拷贝字符 | ✅ |
| 匹配器 | 按字符游标 `pos`前进；`chars` 边比字符、`param` 吃 `/` + 非空段、`catch_all` 吃到尾、`end_mark` 要求“恰好到尾”。出边按 rank 试，**失败就回溯**（如 `/files/static/x` 字面量边走到底不够 → 回退给 `/files/{path...}`） | ✅ |
| 两侧如何对齐 | 段级提取出的**捕获顺序**就是字符级树的捕获槽位序（第 i 个命名占位符 ↔ `capture[i]`） | 🟡 |
| 表的生命周期 | **运行期对象**，可**全量重建**；"修改路由"= 切换活跃集合后重建 | 🟡 |
| pattern 存储 | 字面量有**静态存储期** → 树里直接存 `string_view`，**不需要 `deque` / arena、零分配** | 🟡 |
| 擦除边界 | handler 的类型擦除发生在 **`Router` 内部**；`Server` 保持**非模板单类型**。<br>🟡 **计划变更（2026-10-13）**：擦除粒度从**每条路由一个**降到**每个动词一处** —— 路由表里存具体类型的 tuple，“匹配 + 调用”收进同一个擦除单元（现在 `match` 与 `handlers[slot]` 是**两道**间接，互不可见）。**排在 §4.1 之后**（§4.1 会改 handler 签名） | 🟡 |

> `Server` 不成模板这一点很关键：`run()` / `accept_loop()` / `session::handle()` **都不必改**，
> 也就避免了"模板定义必须搬回 `.cppm`"的连锁反应。

> **落地进度（2026-10-13 晚，已跑通）✅ —— 修订：本节先前记的"未落地"已作废**
> - **接线**：`Router::set_routes` + `dispatch`（先试请求动词那棵树 → 否则 405/404）（`server/route.cpp`）；
>   `Builder::route_on` = `validate_route` 折叠 + `validate_group` + `set_routes(make_routes(...))`（`server/server.cppm`）。
> - **只留一种注册方式**（2026-10-13）：旧字符串 API（`Router::add/get/post/put` + 精确表 + `operator""_route` UDL）已删；
>   `dispatch` 不再有"精确表"那两步。
> - **每 method 一棵树**（2026-10-13 晚②）✅ —— 实测过：所有路由进同一棵树时 `Get<"/items">` 与 `Post<"/items">`
>   会落到同一节点互相覆盖（`GET /items` 变 405、`POST /items` 变 200）。现在按动词分开建树，`tests/http_e2e.py` 有这两条断言钉住。
> - **一个 builder 只允许一次 `route_on`**（2026-10-13 晚②）✅ —— 编译期：`route_on` 返回的 `Builder::Configured` 只有 `build()`，
>   于是"声明两组路由"编不过、跨组冲突不存在；组内冲突由 `validate_group` 在注册点**点名两条 pattern** 拦下。
> - **协程返回类型**：~~`Endpoint` 能当协程返回类型靠 `route.cppm` 尾部的 `coroutine_traits` 特化~~
>   🔄 **2026-10-13 晚已删** —— 声明搬到第一个形参后，handler 返回 `awaitable<void>` 是**天然**的，
>   无需借 asio 的 frame 当返回值 ⇒ `coroutine_traits` 特化与 `release()` 一并删除（手写协程正文也是那个理由不再需要）。
> - **端到端已验证** ✅：`tests/http_e2e.py`（裸 socket，避开 curl 的环境变量）22 条用例全过 ——
>   `/users/42 → user id=42`（`req.param<>()` 生效）· `GET /items` 与 `POST /items`（**同路径两动词各走各的**）·
>   `POST /health → 405` + `Allow` · `/nope` → 404 · 请求体（含 chunked）与 413。复核：`cmake --build --preset debug_llvm` → `./tests/run.sh`。
> - **已知缺口** 🔍：`/users/abc → user id=0`（`from_chars` 失败静默给 0 → 需要 400 路径）；
>   `make_handler` 擦除后**没有捕获通道**，只能按本条 pattern **再匹配一次**（正确但冗余）。
> - **三层级分工**（2026-10-13 讨论结论）：`Pattern` = **单条 route** 的语法扫描（产段）· `PieceCursor` = **变换**（段序列 → 片段序列，
>   **不是**第二个解析器 —— 它消费 `Pattern` 的 `kind`/`text`，一个字都不重新判类型）· `insert_pattern` = **跨所有 route** 的合并（产 label）。
>   三者输入不同 ⇒ 分三份实现是**层级划分**，不是重复。真正多余的只有 `Segment::pos` / `raw_len`（只为"事后回折"存在）。
> - **「先算后建」是权宜，不是规范**：`std::vector` 不能进 `constexpr` 变量 ⇒ 只能定容 `std::array` ⇒ 只能先数后建。
>   备选是"**数组长度进类型**"（实验：`.vscode/tmp/route-digest/pieces_by_type.cpp`）—— 省 69~88% 编译期槽位，
>   但每步 O(n) 拷贝（整体 O(n²)）、实例化数 = 段数，**运行期零差别**。结论：现状够用，不为此再动 `Pattern`。
>   🔄 **修订（2026-10-13 同日，用户指出）**：上面那个 69~88% **对比错了基准** —— **段数本来就是 `count(raw, '/')`**
>   （扫描每轮至多 push 一个段、消耗一个 `/`；合法 pattern 恰好相等，非法的也不超），用 `constexpr` **朴素数一下**就能拿到，
>   **不需要元编程**；`names` 再数一遍命名捕获即可。按此重算（现在 / 朴素精确 / 技巧）：`/health` 16 / **1** / 2 · `/api/v1/echo` 26 / **3** / 4 ·
>   `/files/{path...}` 34 / **3** / 4 · `/a/{x}/b/{y}` 26 / **6** / 8 —— **朴素精确计数全面优于技巧**。
>   ⇒ **技巧在此处没有性价比**。原代码用宽松的 `raw.size()+1` 是因为**一个常量兼三用**（`segs` / `names` 容量 + `k_node_bound`）。
>   ✅ **已实施（2026-10-13）**：拆成三个常量 —— `segment_count = count('/')`、`name_count = count('{')` 给两个数组，
>   单留 `node_budget = raw.size() + 1` 给 `k_node_bound`（**值不变** ⇒ **建树行为一字不动**）。
>   ⚠️ `k_node_bound` **不能**跟着收紧：`end_mark` 与匿名 `catch_all` 的边 label 是**空**的，"`#edges ≤ Σ 字符数`"不成立。
>   复核：新增 5 条 `static_assert`（钉住维度）+ 原有 96 条 + `./tests/run.sh`（9/9）。
#### D. 路径参数怎么给 handler（🔄 2026-10-13 重写：`Param<>` 整套已废）

| 项 | 决定 | 状态 |
|---|---|---|
| 参数写法 | ✅ **`req.param<T, "名字">()`**（`Request` 的成员函数，名字必写）—— 声明搬到第一个形参后，**请求对象本身就能带捕获**，不再需要在形参表里摆载体。于是 `Param` / `IsParam` / `ParamCount` / `SlotOf` / 位置配对 全部删除 | ✅ |
| ~~名字式 vs 位置式混用~~ | 不存在了 —— 只有按名字一种，没有"第 N 个"这个概念 | ✅ |
| 校对时机 | ✅ 挪到**调用点**：`slot_of("名字")` 在编译期算出下标，不在 pattern 里就编不过（错误定位到写的那一行）<br>⚠️ **代价**：“pattern 里的名字没人读”不再算错 —— 读取发生在 handler 体内，编译期看不到 | ✅ |
| 失败语义 | 🔍 **仍是缺口**：整数转换失败静默给 0（`/users/abc → id == 0`）。备选：`std::expected` 形状的返回值（`has_value()` / `value()` / `error()`），由 handler 自检 | 🟡 |
| 注入范围 | ✅ 现在只有 `Get<…>` 请求 + `http::Response&` 两种；body / query / header 留待将来（读体见 §4.1） | 🟡 |
| handler 返回值 | **必须是 `asio::awaitable<void>`**（2026-10-13 修订：曾是 `awaitable<Response>` 或同步 `Response`） | ✅ |

#### E. 与主流实现的对照（本次调研来源）

| 实现 | 优先级策略 | 冲突处理 | 出处 |
|---|---|---|---|
| Go 1.22 `net/http.ServeMux` | 静态优先（子集定义） | **注册即 panic** | pkg.go.dev/net/http#ServeMux |
| Werkzeug / Flask | 静态优先 + 长静态优先 + converter 权重 | 稳定排序兜底（先注册先） | werkzeug.palletsprojects.com/routing |
| Spring `PathPattern` | 打分：变量/通配越少越具体 | 排序，不报错 | `SPECIFICITY_COMPARATOR` Javadoc |
| Fastify / find-my-way | 固定 5 级：static → 带静态结尾参数 → 正则/多参数 → 参数 → 通配 | 排序 | fastify.dev/docs/latest/Reference/Routes |
| httprouter / Gin | 静态优先 | **歧义即拒绝** | gin-gonic.com/docs/routing |
| matchit / axum | static > 后缀参数 > 前缀参数 > 参数 > catch-all | **歧义即插值报错** | docs.rs/matchit |
| Express / Fiber / Hono / FastAPI | **注册顺序**（first match wins） | 无 | 各自官方文档 |
| `ctre`（方法论参照） | —— | —— | github.com/hanickadot/compile-time-regular-expressions |

**结论**：主流分两派（静态优先 6 家 vs 注册顺序 4 家），本项目选**静态优先**；
"静态优先"派里，**新实现倾向于在注册/构造期直接拒绝歧义**（Go / httprouter / matchit），与本项目一致。

#### F. 保留意见与风险 ⚠️

1. **UDL 写法 —— ✅ 已实测（2026-10-12 晚，clang 23.1.1，`-std=c++23`）**：
   - **A 路** `template <FixedString S> constexpr auto operator""_route()` → **零警告**，**采用这条**；
   - B 路 `template <typename CharT, CharT...>`（N3599，`ctre` 走这条）→ 能编，但报
     `-Wgnu-string-literal-operator-template`。
   - 代码当时随 §11.2 A 一起落进 `server/route.cppm`（`operator""_route`，**A 路**），**2026-10-13 已删**；
     当时的独立探针（`.vscode/tmp/udl-probe/`、`server/router-proto/`）**验证后已删**。
   - 若要复验 B 路的告警：把该 operator 的形参改成 `template <typename CharT, CharT... Cs>`
     并用一次字面量，`-Wall -Wextra` 下会报 `-Wgnu-string-literal-operator-template`。
   - ⚠️ **推翻了我先前的判断**：我曾据 `ctre` 文档断言"UDL 形式必须依赖 N3599"——
     实测表明 **class-type NTTP 的 UDL 形式在 clang 23 上直接可用**（`ctre` 文档可能滞后）。
   - 🔄 **作废（2026-10-13）**：`operator""_route` 已随**旧字符串路由接口**一并从 `server/route.cppm` 删除
     —— 声明语法改为 `Get<">...">` 住在**第一个形参**，不再需要这个 UDL。上面的实测结论仍可复验，但**不再是活跃设计**。
2. **"错误无法通过编译"依赖两个前提**：① Builder 必须是**类型级**累积（否则 `build()` 只能在运行期跑）；
   ② pattern 必须**全部来自字面量**（用户已确认活跃集合只做切换）。任一前提变化，编译期错误检测就退化为运行期。
3. **编译时间 / 模板深度**：类型级 Builder + `constexpr` 建树 + 协程 + 模块，四者叠加。
   路由少时看不出来，路由多时需实测。
4. **声明式路由要求 handler 签名可内省** → **泛型 lambda（`auto` 参数）不支持**，需 `static_assert` 给出清晰报错
   （现只剩两个约束：第一个形参是请求类型、返回类型是 `awaitable<void>`）。
5. **静态优先 + 冲突报错会拒掉一些"看起来合理"的组合**（如 `/a/{x}` 与 `/a/{y}` 直接编译错）。
6. **`{rest...}` / `{$}` / 尾斜杠子树 三者语义接近**，容易误用；实现时需给对照例子。

#### G. 本层：决定与遗留

- **HEAD** —— ✅ 已落地（2026-10-13）：没写 `Head<>` 时，HEAD 复用同路径的 GET handler（响应**无体**、`Content-Length` 仍按 GET 算）；显式 `Head<>` 优先。
  抑制正文收口在 `http::serialize(response, keep_alive, head_only)` 一处（`session.cpp` 传 `req.method == "HEAD"`）→ 显式 `Head<>` 填了 `body` 也一并不发。
- **OPTIONS** —— ✅ 已落地：自动应答 `204` + `Allow`（按该路径已注册的动词，无体、不声称 `Content-Type`）；路径不存在 → 404。
- **405 的 `Allow` 头** —— ✅ 已落地，与上面两条同一批（都在 `dispatch` 里“逐棵动词树试一遍”）。
  ⚠️ **用户决定（2026-10-13）**：路径上有 `GET` 而无显式 `Head<>` 时，`Allow` **要带上 HEAD**（RFC 9110 §9.3.2 的隐含搭档），位置紧跟 GET。
- **重复注册同一 `(method, path)`** —— ✅ 已定（2026-10-13）：**编译期报错**（`validate_group`，错误里点名两条 pattern）。跨组不可能发生：一个 builder 只允许一次 `route_on`。**已落地 ✅**
- **重建 API 形态**：`build()` 怎么暴露、"活跃集合"怎么表达
- 路由分组 / 前缀挂载 / 中间件
- 路由表打印（调试用）

---

## 12. 研究方法

- 负载来源：**外部工具为主**（`oha` / `wrk` / `hey`）+ **仓库内自带压测客户端**（需要精确控制 $\lambda$ 波形时用）。
  内置客户端建议**独立 target**，避免污染服务端指标。
- 实验纪律（冻结参数、记录构建 id + 参数 + 指纹、按请求类别归因、丢弃过载数据）见 `AGENTS.md` §4。
- 取源方式（选型 / 调研类问题必须上网核实）见 `AGENTS.md` §2 与全局 `preferences.md`。
