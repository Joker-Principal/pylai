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

### 1.1 做什么（🟡 用户表态）

**最终要 HTTP/1.1 + HTTP/2 + HTTP/3** 的服务器。

> 用户原话大意：QUIC over asio 可能是个大工程，但"它也是值得为之建模一些参数的场景"。

### 1.2 约束

| 约束 | 内容 |
|---|---|
| **可停机维护** | 服务器**允许停机维护与调参**。这条直接决定了 §7 的"离线 / 在线"分工（见 §7.2） |
| 模块层边界 | `standalone/` **只**做 asio → module 封装；应用另建目录（`AGENTS.md` §1） |
| 环境 | io_uring 恒定启用（用户倾向不做运行期开关）；构建与工具纪律见 `AGENTS.md` |

### 1.3 非目标（❓ 待确认）

- 是否**不追求生产级健壮性**（定位为研究 / 建模用服务器）？
- 是否**不做多机 / 分布式协调**？

### 1.4 上游关系（🟡 2026-10-12 用户拍板）

本仓库的应用层（`server/`，可执行 **`pylai`**）将成为
[herodotus](https://github.com/Joker-Principal/herodotus) 的 **`clio` 组件的 HTTP server 实现**，
目标是**取代 hical**。

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
| **0** | HTTP/1.1 基线：单 io_context + io_uring，固定线程数，keep-alive，**有界队列 + 在途上限 + 超时**（哪怕固定值）。**无指标、无自适应、无 TLS**。<br>🟡 **进行中（2026-10-12）**<br>✅ 已跑通：llhttp 解析（pipeline / 半包 / 400 / 431）、keep-alive、空闲与写超时、**取消传播**（每连接独立 `cancellation_signal` + `plf::hive` 注册表，见 `asio-notes.md` §7）、SIGINT 优雅停机、io_uring 后端（实测 `anon_inode:[io_uring]`）<br>❌ 尚差：**有界队列 / 准入控制**、**外部压测 60s 基线**<br>🟡 进行中：**响应与路由**（当前是解析结果回显，不是真正的 HTTP 语义） | 外部压测 60s 无泄漏无挂死，记录基线 |
| **1** | **只测不调**：按 §4 的结论建在 "stream/请求" 层的指标；请求级直方图 + 计数器 + OS 侧；周期报告；**观测开销自测**；冻结模式 | 指标稳定；开销 < 1~2% |
| **2** | 动态池：§5.4 上下界 + EWMA/死区/迟滞/冷却 + 护栏/限步/回滚/冻结。引入 TLS 作为可控 CPU 密集负载 | 阶跃负载收敛性；固定 N（含 2N）vs 动态；**CPU 密集 vs 阻塞型各跑一遍** |
| **3** | **离线扫参 + 跨窗口累积**（利用允许停机维护）：维护窗口内重启扫参 → 归档 / 固化；指纹 + 版本化；冻结 / 重置 / 打印 | 第二次启动"进入良好区间所需时间"缩短；模型跨窗口改进（见 §7.2） |
| **4** | 建模与 Pareto 报告：拟合 USL，输出前沿；预测误差作漂移报警 | 用未参与拟合的负载档位检验预测误差 |
| **5** | **HTTP/3 独立轨道**：包 ngtcp2/nghttp3 + OpenSSL；另建连/流级流控与拥塞控制参数模型 | — |

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
7. TLS 何时引入（当前倾向：Stage 2，当 CPU 密集负载用）。
8. io_uring：用户倾向**不做运行期开关**（恒定启用）。⚠️ 若 WSL2 上 io_uring 异常则无退路；
   缓解：那一行 `target_compile_definitions` 随时可改（"不做开关"≠"关不掉"），但那就属于改模块层。
9. **维护窗口的形态**（❓ 直接影响 §7.2）：窗口内**能否重放生产流量**？窗口多长、多频繁？
   —— 这是"离线 PGO 形态能否成立"的前提。
10. **§1.3 的非目标**是否成立。
11. **路由层形态**：✅ **已定（2026-10-12）**
    - 现有 `App` + `Router`（监听 / 接受 / 会话注册表 / 事件循环 / 停机）**合并为 `Server`**，
      用 **builder 模式**构造；`Router` 这个名字腾出来专做**路由**。
    - handler 形态 = **`asio::awaitable<Response>`**（一步到位，允许 handler 内部 `co_await`；
      为将来查 DB / 回源上游留路，分发层因此也是协程）。
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
> 字符级压缩 radix 匹配、声明式 `route_on` 注册与位置注入；接线在 `server/route.cpp` / `server/server.cppm`。
> 验证方式：`cmake --build --preset debug_llvm`（`static_assert` 与用例每次构建都会跑）；端到端证据见 §11.2 C 的落地进度块。

**一句话**：`Get<"...">` 写在**返回类型位置**声明一条路由 → **`constexpr` 构建器**在编译期解析 pattern、
把整包压成**一棵字符级 radix tree** → 运行期持表匹配；handler 用 `Param<...>` 显式声明要哪些参数，
**注册点**（`Builder::route_on`）做 pattern ↔ handler 的编译期交叉核对。

```cpp
using namespace pylai::route;

struct App
{
	Get<"/health"> health(const http::Request& request);
	Get<"/users/{id}"> user(Param<int, "id"> id, const http::Request& request);
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
| 段内前后缀（`img-{id}.png`） | **不做** | 🟡 |

#### B. 匹配策略

| 项 | 决定 | 状态 |
|---|---|---|
| 优先级 | **静态优先**：静态边 → 参数边 → 通配边 | 🟡 |
| 冲突定义 | **🔄 修订（2026-10-12 晚，核实 Go 源码后）**：**"分歧点上必须有字面量"**。<br>分歧点是**字面量** → 它 rank 最高、一眼看得出谁赢 → **放行**（如 `/a/{x}/c` + `/a/b/{y}`）<br>分歧点是**两个通配符** → 没有字面量可定优先级 → **报错**（如 `/a/{x}` + `/a/{y...}`、`/a/{y...}` + `/a/{$}`）<br>公共前缀完全同形**且等长** → 落在同一节点、后注册覆盖前一个 → **报错**（重复注册 / 同形异名 / `/files/` 与 `/files/{p...}`）<br>⚠️ 原记的"照抄 Go 子集规则"**已作废** | 🟡 |
| 与 Go 的差异 | ✅ 已核实（`src/net/http/pattern.go` 的 `compareSegments` / `combineRelationships`）：Go 用**"谁更具体"**定序，于是 **放过** `/a/{x}`+`/a/{y...}`（`s2.multi` → `moreSpecific`）、**拒掉** `/a/{x}/c`+`/a/b/{y}`（`moreGeneral` ∧ `moreSpecific` → `overlaps`）。**我们恰好相反** —— 因为我们靠**字面量**定序，不靠"谁更具体" | ✅ |
| 报错时机 | ❌ **不需要运行期报错**——用户判断：**类型信息在编译期，错误的路由表无法通过编译** | 🟡 |

> ⚠️ 最后一条的成立前提见 §F 风险 2。

> **实现进度（2026-10-13 晚，`server/route.cppm`，1285 行 · 99 条 `static_assert`）**
> - **段级视图**（校验 + 参数提取）：`Segment` / `PatternView` / `Pattern<S>` / `params()` ✅
> - **段级冲突检查**：`segments_disjoint` / `routes_conflict` / `RouteConflict` ✅
> - **编译期路由表**：`On` @683 · `find_table_conflict` @691 · `Table` @707（**声明即校验**，冲突 → 编译错）✅
> - **字符级压缩 radix**（运行期匹配）：`RadixKind` @318 · `RadixEdge` @326 · `RadixNode` @334 · `RadixTree` @342 · `Capture` @352 · `PieceCursor` @389 · `insert_pattern` @513 · `build` @597 · `match_at` @604 · `match` @665 ✅
> - **位置注入 DSL**：`Verb` @730 · `Endpoint`/`Get…Patch` @758 · `Param` / `RouteOf` / `ParamCount` / `SlotOf` · `validate_route` @888 · `make_arg` / `make_handler` · `RouteGroup` @946 / `make_group` · `std::coroutine_traits` 特化 ✅
> - 编译期用例：`selftest` · `conflict_selftest` · `table_selftest` · `match_selftest` · `dsl_selftest` ✅
> - 验证通道：**`tools/clice query`**（`--fresh` = 先重索引/重编译再回答）。命名检查才用 `clice lint`。
>   `.clang-tidy` 已用 `HeaderFilterRegex` + `ExcludeHeaderFilterRegex` 把范围收窄到本工作区。
> - **未落地 🔍**：**每 method 一棵树**（现为**每组**一棵，动词混在同一棵树里）· 405 的 `Allow` 头 · 运行期重建/切换活跃集合 · `Param` 的**转换失败语义**（现 `from_chars` 失败静默给 0）

#### C. 结构与实现

| 项 | 决定 | 状态 |
|---|---|---|
| **粒度分工** | **🔄 修订（2026-10-12 晚，用户明确）**：**两阶段两种粒度**。<br>**编译期**（校验 + **参数提取**）= **段级**：用 `PatternView` 的段序列跑冲突检查（"分歧点上有没有字面量"本来就是段级概念），并**按出现顺序提取命名捕获**（`Pattern<S>::params()`，供与 handler 的 `Param<...>` 交叉核对）。<br>**运行期**（请求路径匹配）= **字符级压缩**（radix）：边是字符片段，对请求路径做字符串匹配。<br>✅ 已按此**删除**原先的段级运行期匹配器 | 🟡 |
| 树形 | **每 method 一棵字符级压缩 radix tree**（⚠️ 不是段级 trie） | ✅ |
| 建树方式 | **`constexpr` 构建器**：把各路由的**段序列**压成**字符级** radix；冲突检查在段级完成。方法论借自 `ctre`（用 `constexpr` 函数而非模板元编程） | ✅ |
| 边的存储 | label 全部是某个 pattern 的 `raw` 上的**切片**（静态存储）→ **零拷贝**；分叉时只把切片一分为二，不拷贝字符 | ✅ |
| 匹配器 | 按字符游标 `pos`前进；`chars` 边比字符、`param` 吃 `/` + 非空段、`catch_all` 吃到尾、`end_mark` 要求“恰好到尾”。出边按 rank 试，**失败就回溯**（如 `/files/static/x` 字面量边走到底不够 → 回退给 `/files/{path...}`） | ✅ |
| 两侧如何对齐 | 段级提取出的**捕获顺序**就是字符级树的捕获槽位序（第 i 个命名占位符 ↔ `capture[i]`） | 🟡 |
| 表的生命周期 | **运行期对象**，可**全量重建**；"修改路由"= 切换活跃集合后重建 | 🟡 |
| pattern 存储 | 字面量有**静态存储期** → 树里直接存 `string_view`，**不需要 `deque` / arena、零分配** | 🟡 |
| 擦除边界 | handler 的类型擦除发生在 **`Router` 内部**；`Server` 保持**非模板单类型** | 🟡 |

> `Server` 不成模板这一点很关键：`run()` / `accept_loop()` / `session::handle()` **都不必改**，
> 也就避免了"模板定义必须搬回 `.cppm`"的连锁反应。

> **落地进度（2026-10-13 晚，已跑通）✅ —— 修订：本节先前记的"未落地"已作废**
> - **接线**：`Router::add_group` + `dispatch`（组匹配 → 动词核对 → 405/404）（`server/route.cpp`）；
>   `Builder::route_on` = `validate_route` 折叠 + `add_group(make_group(...))`（`server/server.cppm`）。
> - **只留一种注册方式**（2026-10-13）：旧字符串 API（`Router::add/get/post/put` + 精确表 + `operator""_route` UDL）已删；
>   `dispatch` 不再有"精确表"那两步。
> - **协程返回类型**：`Endpoint` 能当协程返回类型靠 `route.cppm` 尾部的 `coroutine_traits` 特化 →
>   `standalone/asio.cppm` 因此导出 `asio::detail::awaitable_frame`。
> - **端到端已验证** ✅：`tests/http_e2e.py`（裸 socket，避开 curl 的环境变量）发 7 个请求全过 ——
>   `/users/42 → user id=42`（位置注入生效）· `POST /submit/7`（同组另一动词）· `POST /health → 405` ·
>   `/nope` 与 `/echo → 404`。复核：`cmake --build --preset debug_llvm` → `./tests/run.sh`
>   （2026-10-13 起：示例应用与脚本已从 `.vscode/tmp/route-dsl-probe/` 搬进 `tests/`）。
> - **已知缺口** 🔍：`/users/abc → user id=0`（`from_chars` 失败静默给 0 → 需要 400 路径）；
>   `make_handler` 擦除后**没有捕获通道**，只能按本条 pattern **再匹配一次**（正确但冗余）。

#### D. 位置注入

| 项 | 决定 | 状态 |
|---|---|---|
| 参数写法 | `Param<"id", int>`（名字+类型）/ `Param<"id">`（名字）/ `Param<int>`（位置） | 🟡 |
| 名字式 vs 位置式 | **禁止混用**（编译期 `static_assert` 拦住） | 🟡 |
| 失败语义 | `Param` 呈 **`std::expected` 形状**（`has_value()` / `value()` / `error()`），**转换失败由 handler 自检**，框架不自动拦 | 🟡 |
| 注入范围 | 只要 **`Param<...>` + `const http::Request&`**（body / query / header 留待将来） | 🟡 |
| handler 返回值 | **`awaitable<Response>` 与同步 `Response` 都支持**（擦除时统一包成协程） | 🟡 |
| 交叉核对 | 注册点 `static_assert`：pattern 的占位符集合 ↔ handler 的 `Param` 集合（名字 / 个数对不上 → 编译错） | 🟡 |

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
     —— 注册语法改为 `Get<"...">` 放返回类型位置，不再需要这个 UDL。上面的实测结论仍可复验，但**不再是活跃设计**。
2. **"错误无法通过编译"依赖两个前提**：① Builder 必须是**类型级**累积（否则 `build()` 只能在运行期跑）；
   ② pattern 必须**全部来自字面量**（用户已确认活跃集合只做切换）。任一前提变化，编译期错误检测就退化为运行期。
3. **编译时间 / 模板深度**：类型级 Builder + `constexpr` 建树 + 协程 + 模块，四者叠加。
   路由少时看不出来，路由多时需实测。
4. **位置注入要求 handler 签名可内省** → **泛型 lambda（`auto` 参数）不支持**，需 `static_assert` 给出清晰报错。
5. **静态优先 + 冲突报错会拒掉一些"看起来合理"的组合**（如 `/a/{x}` 与 `/a/{y}` 直接编译错）。
6. **`{rest...}` / `{$}` / 尾斜杠子树 三者语义接近**，容易误用；实现时需给对照例子。

#### G. 本层尚未决定

- GET 是否**自动注册 HEAD**；`OPTIONS` 自动应答；405 的 `Allow` 头
- **重复注册**同一 `(method, path)` 的语义（现状是"最后一次覆盖"，与新设计是否冲突待定）
- **重建 API 形态**：`build()` 怎么暴露、"活跃集合"怎么表达
- 路由分组 / 前缀挂载 / 中间件
- 路由表打印（调试用）

---

## 12. 研究方法

- 负载来源：**外部工具为主**（`oha` / `wrk` / `hey`）+ **仓库内自带压测客户端**（需要精确控制 $\lambda$ 波形时用）。
  内置客户端建议**独立 target**，避免污染服务端指标。
- 实验纪律（冻结参数、记录构建 id + 参数 + 指纹、按请求类别归因、丢弃过载数据）见 `AGENTS.md` §4。
- 取源方式（选型 / 调研类问题必须上网核实）见 `AGENTS.md` §2 与全局 `preferences.md`。
