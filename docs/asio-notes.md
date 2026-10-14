# asio 笔记（机制 / 调参 / 已知坑）

> ⚠️ **这是调研笔记，不是决策记录。** 所有结论都附 `文件:行`，方便复核；asio 版本一变就可能失效。
> 状态标记：**✅ 已验证**（代码或实验） · **🟡 倾向**（待评估，可能被推翻） · **❓ 未定** · **⚠️ 风险**
>
> 📌 **工具用法 / 模块与头文件混用规则**等"规矩"在 `AGENTS.md`；本文件只留**知识性**内容（机制、语义、数值）。

---

## 0. 环境与版本

> 版本**随 vcpkg 根更新而变** ⇒ 以 `build-dev_llvm/vcpkg_installed/vcpkg/status` 里的实测值为准（`grep -E '^Package:|^Version:' …/status | paste - -`）。

| 项 | 值 |
|---|---|
| asio | **1.38.2**，通过 **overlay port** 提供（见 §1），不是 vcpkg 内置端口 |
| liburing | vcpkg 端口，静态 `liburing.a` + `liburing.pc` |
| OpenSSL | vcpkg `3.6.5`（≥3.2，理论上可支撑 ngtcp2 的 QUIC crypto） |
| Boost | vcpkg `1.92.0`（`boost-context` / `boost-asio`） |
| 编译器 / 构建 | clang++ 23，C++23，Ninja Multi-Config，C++20 modules |
| CMake | 4.4.4；但项目 `cmake_minimum_required` **保持 3.28** |
| 构建 / 环境 | 命令、工具纪律见 **`AGENTS.md`** |

---

## 1. ✅ asio 通过 overlay port 提供（为什么）

- vcpkg **内置** `asio` 端口停在 **1.32.0**，不够新。
- 因此自建 overlay port：`ports/asio/`（`vcpkg.json` / `portfile.cmake` / `CMakeLists.txt` / `asio-config.cmake`），
  版本 `1.38.2`，SHA512 已校验。
- 用 `vcpkg-configuration.json` 注册：`{"overlay-ports": ["./ports"]}`。
- 端口特性映射：`spawn → boost-context`、`ssl → openssl`、`regex → boost-regex`。
- asio ≥1.36 在 C++11+ 下**默认 standalone**，不再需要 `ASIO_STANDALONE` 补丁。
- asio ≈1.38 的源码布局变了：根目录是 `include/` + 根级 `asio.pc.in`（旧的 `asio/` 目录只是符号链接）。
  内置端口的 CMakeLists 路径（`asio/include/...`、`asio/asio.pc.in`）**不能照抄**。

---

## 2. 模块层现状（`standalone/` → target `asio-module`）

> 这一层**只做 asio 的 module 封装**，与应用无关。应用代码另建目录（见 `http-server-plan.md`）。

- 模块文件：`standalone/asio.cppm`、`standalone/asio/ssl.cppm`、`standalone/asio/experimental.cppm`
- 分离编译：`standalone/asio.cpp`（`<asio/impl/src.hpp>`）、`standalone/asio/ssl.cpp`、`standalone/asio/experimental.cpp`
- 链接：`asio::asio`、`Boost::context`、`OpenSSL::SSL/Crypto`、`PkgConfig::liburing`
- 编译定义（PUBLIC，会传播给消费者）：
  `ASIO_SEPARATE_COMPILATION=1`、`ASIO_NO_DEPRECATED=1`、`ASIO_HAS_IO_URING=1`、`ASIO_HAS_IO_URING_AS_DEFAULT=1`

---

## 3. ✅ io_uring 模式下的调度机制

### 3.1 骨架：scheduler 是引擎，"reactor" 只是一个 task

- `io_context::run()` → `scheduler::run()`（`asio/impl/io_context.ipp:60`、`asio/detail/impl/scheduler.ipp:204`）
- `scheduler::run()` 循环调用 `do_run_one()`（`scheduler.ipp:459`）
- 真正的"等待 I/O"被外包给一个 **`scheduler_task`**：
  `scheduler::get_default_task`（`scheduler.ipp:711`）在 `ASIO_HAS_IO_URING_AS_DEFAULT` 下返回
  **`io_uring_service`**，否则返回 `reactor`。
- 已用 clice 复核：`io_uring_service` 的父类含 `scheduler_task`；
  `scheduler_task::run` 的调用点是 `do_run_one` / `do_wait_one` / `do_poll_one`。

### 3.2 队列与锁：多线程并行的真正来源

- **只有一个全局** `op_queue_` + 一把 `mutex_`（`asio/detail/scheduler.hpp:236` 附近）。
  没有 per-thread 队列池，**没有 work stealing**。
- `do_run_one` 取到普通 op 后**先 `lock.unlock()` 再 `o->complete(...)`**
  → 所以 N 个线程能同时执行 N 个 handler。
- `task_operation_` 是 task 的**哨兵**：task 想再次被调度就把它重新 push 回队列
  （`task_cleanup`，`scheduler.ipp:49`）。
- **`private_op_queue` 是线程私有的暂存队列**，其真正用途是：task / handler 在**持锁之外**运行，
  期间不能碰全局队列，于是先攒到私有队列，结束时就地合并回全局
  （`task_cleanup` / `work_cleanup`，`scheduler.ipp:49-105`）。
  它**不是**负载分区机制。
- 唤醒：`wake_one_thread_and_unlock`（`scheduler.ipp:673`）—— 有空闲线程就唤醒一个，
  否则 `task_->interrupt()`。

### 3.3 io_uring 侧

| 事 | 位置 / 结论 |
|---|---|
| 提交 SQE | `io_uring_service::start_op`（`impl/io_uring_service.ipp:357`）；CQE 的 `user_data` 指向 `io_queue` |
| 批量提交 | 攒到 `io_uring_submit_batch_size`（默认 **128**）才提交，否则挂一个 `submit_sqes_op` 延后 |
| 收割 CQE（= reactor 的替代） | `run` → `run_single_ring`(`:532`) / `run_multi_ring`(`:643`) |
| 单 ring 等待 | `usec == 0` → `io_uring_peek_cqe`；否则 `io_uring_wait_cqe`（阻塞） |
| 多 ring | 先 `poll()` 各 `ring_fd` 找 ready；ring>0 用 `IORING_SETUP_ATTACH_WQ` 共享内核 worker pool |
| 唤醒 = 中断 task | `io_uring_service::interrupt`(`:786`) = 塞一个 **nop SQE** 并提交 |
| 定时器 | 永远在 **ring 0**（`timer_mutex()` = `rings_[0].mutex_`） |
| 提交侧的线程亲和 | `current_ring_index()`(`:999`)：TSS 记 raw 序号（`next_ring_` 自增），再 `% rings_.size()` |
| 完成事件分发关键点 | `io_queue::perform_io`：**第一个** handler 由收割线程当场执行，**其余** `post_deferred_completions` 进全局队列，任意 `run()` 线程都能抢 |

**实验验证（怎么复核）**：
```bash
# 后端类型断言（能编过即证明 io_uring 是实际后端）
clang++ -std=gnu++23 -fsyntax-only \
  -DASIO_HAS_IO_URING=1 -DASIO_HAS_IO_URING_AS_DEFAULT=1 -DASIO_NO_DEPRECATED=1 \
  -isystem <vcpkg_installed>/x64-linux/include -x c++ - <<'EOF'
#include <type_traits>
#include <asio/detail/timer_scheduler_fwd.hpp>
#include <asio/detail/io_uring_service.hpp>
static_assert(std::is_same_v<asio::detail::timer_scheduler,
                             asio::detail::io_uring_service>);
static_assert(ASIO_HAS_IO_URING_AS_DEFAULT == 1);
EOF
```

### 3.4 🟡 构建侧：liburing 怎么接进来

- vcpkg 的 `liburing` **只给 pkg-config**，没有 CMake config → 用 `FindPkgConfig`：
  `pkg_check_modules(liburing REQUIRED IMPORTED_TARGET liburing)`，并把
  `${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig` 加进 `PKG_CONFIG_PATH` → link `PkgConfig::liburing`。
- ⚠️ CMake 4.4.4 的 `cmake_pkg_config()` **读不了 vcpkg 的 `.pc`**：不支持 `${pcfiledir}`（prefix 会变成 `/..`），
  带引号的 `"-I${includedir}"` 也不认（会漏进 COMPILE_OPTIONS）→ vcpkg 的 `.pc` 别用它。
- 宏 / pin：`ports/asio/` overlay + `ASIO_HAS_IO_URING=1`、`ASIO_HAS_IO_URING_AS_DEFAULT=1`（复核见上方实验）。

---

## 4. ✅ 参数与语义（官方口径）

配置项通过 execution context 的 `config` 读取（`section.key`）。

| 键 | 默认 | 说明 |
|---|---|---|
| `scheduler.concurrency_hint` | `0` | **只是"建议"**；reactor 后端**只认真对待 `1`**（单线程假设 → 开快速路径）。无论取何值都保证线程安全 |
| `scheduler.assume_continuation` | `= (hint==1)` | 为真时把所有 post 当作 continuation，进**线程本地快速队列**；官方警告：若 handler 长跑且 post 出**指望其他线程立刻接手**的工作，此假设**不安全** |
| `scheduler.locking` / `locking_spin_count` | `true` / `0` | |
| `scheduler.task_usec` / `wait_usec` | `-1` / `-1` | `-1` = 不限；`0` = CPU 空转（用 CPU 换延迟） |
| `reactor.io_uring_ring_count` | `1` | ring 数；官方：每线程固定分配一个 ring（round-robin），多线程场景调大可能改善扩展性 |
| `reactor.io_uring_ring_size` | `16384` | |
| `reactor.io_uring_submit_batch_size` | `128` | |
| `reactor.io_uring_iowait` | `true` | 有未提交 SQE 时把线程标记为 in iowait（可能虚高 CPU 利用率；需内核 6.15+ 才有真实效果） |
| `resolver.threads` | `0` | 异步解析的内部线程数 |

并发提示宏的等价映射：`n < 0xFFFF` → hint=n + 全锁；`UNSAFE` → hint=1 + 全无锁；
`UNSAFE_IO` → hint=1 + 仅 io_locking=false；`SAFE` → hint=0 + 全锁。

### ✅ 官方对"该用多少线程"的立场（`Concurrency Without Threads`）

- *"all threads that have joined an io_context's pool are considered equivalent, and the io_context may
  distribute work across them in an **arbitrary** fashion."*
- *"applications do **not** need to spawn many threads in order to increase concurrency."*
- *"…avoid the cost of context switching by **minimising the number of operating system threads — typically
  a limited resource**…"*

→ 官方倾向：**线程数应尽量少（1 或 N=核数），并发应存进廉价的状态里**。

---

## 5. 🟡 线程池尺寸：2N 的来历

- `asio::thread_pool` 默认 = `hardware_concurrency() * 2`，取不到则 **2**
  （`asio/impl/thread_pool.ipp:52`）；`system_context` 也是 `*2`（`impl/system_context.ipp:55`）。
- `hardware_concurrency()`：POSIX 下是 `sysconf(_SC_NPROCESSORS_ONLN)`（`detail/impl/posix_thread.ipp:48`），
  **看的是宿主机核数，不看 cgroup 配额**。
- 自动定大小的构造函数被 `#if !defined(ASIO_NO_TS_EXECUTORS)` 包住 —— 属旧接口，**新代码建议显式传 n**。
- 显式 `thread_pool(n)` 仅在 `n == 1` 时才把 `concurrency_hint` 设为 1。
- **官方没有解释 `*2` 的理由**（`src/doc/history.qbk` 无说明；上游 issue 搜 `default_thread_pool_size` 为 0 结果）。
- 社区口径 `N` / `N+1` / `2N` / `2N+2` 都建立在"**线程会阻塞**"的前提上；异步化正是为了取消这个前提。

---

## 6. asio cancellation：accept 与会话共用一个 `cancellation_signal` 的 slot → 段错误

**状态**：✅ 2026-10-12 已定位（有最小复现；**纯头文件版**，与模块无关）

### 结论

**同一个 `cancellation_signal` 的 slot，不能同时给“accept 操作”和“会话协程”用**
（后者指 `co_spawn(..., bind_cancellation_slot(sig.slot(), ...))`）。

实测触发条件，三条**缺一不可**：

| # | 条件 |
|---|---|
| 1 | `acceptor.async_accept(...)` 绑 `sig.slot()` |
| 2 | `co_spawn(session, bind_cancellation_slot(sig.slot(), ...))` 绑**同一个** `sig` 的 slot |
| 3 | 会话协程内 `co_await (timer.async_wait(...) \|\| socket.async_read_some(...))` |

→ SIGSEGV，且**崩在会话第一次 `co_await (… || …)` 里**：日志能打出 `begin`，打不出 `returned`。

复现用例：`.vscode/tmp/asio-cancel-probe/probe_bind_slot.cpp`
（纯头文件版，无模块、无 CMake，`clang++ -std=c++23 …` 直接编）。
⚠️ 那是**当时的一次性探针**，而 `.vscode/` 已在 `.gitignore` 里 ⇒ **不在版本库里**；要复核请按上面三条条件自己重建。

对照：**只让会话绑、accept 不绑** → 连续三条路径（超时 ×2 / 有数据 / 并发空闲连接）**都不崩**。

### 机制

`cancellation_signal` 是**单槽**的，再次绑定会先销毁旧 handler；
而 `awaitable` 每个 handler 完成时清的是**整条协程栈共用的那一个** slot：

```cpp
// asio/cancellation_signal.hpp —— 单槽
/// A cancellation signal with a single slot.
class cancellation_signal { detail::cancellation_handler_base* handler_; };

// asio/impl/cancellation_signal.ipp  cancellation_slot::prepare_memory()
if (*handler_) { mem = (*handler_)->destroy(); *handler_ = 0; }   // 新绑定先销毁旧的

// asio/impl/awaitable.hpp:170-172（所有操作都从同一个 slot 取：同文件 :755-757）
void clear_cancellation_slot()
{ this->attached_thread_->entry_point()->cancellation_state_.slot().clear(); }
```

→ accept 的 handler **长期挂在该槽上**；会话协程的 `cancellation_state_` 一旦挂上去，
就会 `destroy()` 掉 accept 的 handler，而 accept 还在等 → 悬垂。

### ⚠️ 曾经的误判（已否证）

2026-10-12 早期把这条归因于“**asio 经 C++ module 封装**”，据此做过大量加法实验
（完整导出列表、`namespace std { using std::coroutine_traits; }`、多模块共存、模块分区、
嵌套协程、成员协程、跨模块 `import asio.experimental` 取 `operator||`、GMF 里 include spdlog
……**逐项都不复现**）。

后来在**纯头文件版**最小用例里复现出段错误，证明**与模块无关** —— 真实条件就是上面那三条。
**排查 cancellation / UAF 时不要再拿“经模块 vs 不经模块”当变量。**

**仍然有效、且与运行时无关的**只是“模块封装的**编译期**坑”（见本文件相关小节与 `AGENTS.md`）。

### 附：误判期的原始记录（2026-10-12）

> ⚠️ 下表是**误判期**的分组方式（按"经模块 / 不经模块"），仅作历史留档；
> 真实触发条件是本节开头那三条，与这里的分组无关。

| 当时的超时写法 | 当时观察 |
|---|---|
| `experimental::awaitable_operators::operator\|\|`（HH 的写法），复用成员 timer | ❌ 同一连接**第二次读**时崩 |
| 同上，但每个使用点用一个**局部** timer | ❌ **第一次读**就崩 |
| 手工 `bind_cancellation_slot` + `steady_timer` | ❌ |
| `asio::cancel_after(…)` | ❌ **第一次读**就崩 |
| 纯 `async_read_some`（无 cancellation） | ✅ |

**崩溃点**（gdb 与 ASan 一致）：

```
cancellation_slot::clear()                            include/asio/impl/cancellation_signal.ipp:44
  ← awaitable_frame_base::clear_cancellation_slot()    include/asio/impl/awaitable.hpp:170
    ← awaitable_handler<…, std::tuple<error_code, size_t>>::operator()   include/asio/impl/use_awaitable.hpp:101
```

- `||` 版再往上两层：`parallel_group_op_handler::operator()` ← `parallel_group_completion_handler`
- `cancel_after` 版再往上几层：`completion_payload_handler` ← `timed_cancel_op::complete()`
  ← `handle_timer()`（`detail/timed_cancel_op.hpp:123 / 152`）

**ASan**（构建目录 `build-asan/`）：报 SEGV，寄存器 `rdx = 0xf5f5f5f5…`（ASan 的已释放内存填充）
→ 属 **use-after-free**；但没被标成 `heap-use-after-free`，说明那块内存由 asio 自己管理
（`detail/thread_info_base` 的按线程回收池），ASan 不跟踪。

### 修法方向

`cancellation_signal` **不可拷贝、不可移动**，所以容器必须不移动元素：
`std::vector<signal>` ✗ · `std::list<signal>` ✅ · `plf::hive<signal>` ✅（推荐：无逐节点分配，
且插入/删除其他元素时已有迭代器仍有效）· `std::vector<std::unique_ptr<signal>>` ✅。

- **accept 与会话各自独立 signal**：accept 用一个；每个连接 `emplace` 一个自己的 signal 并绑它，
  会话协程结束时 `erase`；`stop()` 遍历 emit。（**推荐**，真正的结构化并发停机）
- **只用 `io_context` 停机**：不绑 slot，`App::quit()` 走 `context.stop()`（最省事，丢掉优雅取消）
- **只保留 accept 一侧的 slot**：会话不绑

### 并发注意

`plf::hive` **不是线程安全的**，且并发矩阵里最弱的一格恰是“迭代 vs 插入/删除”
（`stop()` 遍历 emit 的同时，会话在 `emplace`/`erase`）：

| plf::colony / hive | Insertion | Erasure | Iteration | Read |
|---|---|---|---|---|
| Insertion | No | No | No | Yes |
| Erasure | No | No | No | Mostly* |
| Iteration | No | No | **No** | Yes |
| Read | Yes | Mostly* | Yes | Yes |

而 `cancellation_signal::emit()` / `slot.assign()` 自身也没有原子操作。
→ 结论与取舍见 **§7**。

**对照材料**：

- `.vscode/tmp/asio-cancel-probe/probe_bind_slot.cpp` —— ✅ 复现用例（头文件版）
- `.vscode/tmp/asio-cancel-probe/probe.cpp` / `probe_or.cpp` —— 非模块对照（不崩；ASan 也干净）
- `.vscode/tmp/asio-cancel-probe/run_bind_slot_test.py` —— 驱动脚本
- `build-asan/` —— pylai 的 ASan 构建

⚠️ 上面前三条在 `.vscode/tmp/` 里 ⇒ **都不在版本库**（`.vscode/` 已 gitignore）；`build-asan/` 也是本机构建目录。要复核就按条件重建。

**重要旁证**：`standalone/asio.cppm` 末尾的

```cpp
namespace std { using std::coroutine_traits; }
```

**不是**可选装饰 —— 去掉它，使用方 TU 会报
`'std::coroutine_traits<asio::awaitable<…>, …>' has no member named 'promise_type'`
（asio 靠 `std::coroutine_traits` 特化提供 promise_type，而该特化在 GMF 里、默认不可达）。

---

## 7. Router 的设计约定（取消传播 + 线程模型）

### 取消传播 —— ✅ 已实现（2026-10-12）

- **accept 用自己的 `cancellation_signal`**；**每个连接一个独立的 `cancellation_signal`**，
  放在 `plf::hive` 注册表里，会话协程结束时摘除，`stop()` 逐个 `emit`。
  ⚠️ 不能"一个 signal 贯穿"：单槽语义见 §6。
- 容器选 `plf::hive`：`cancellation_signal` **不可拷贝、不可移动** → 容器必须不移动元素，
  而 hive 的 `emplace()` 就地构造（元素只需满足 Erasable）。
  备选：`std::list`（可用，但每节点一次分配）、`vector<unique_ptr<signal>>`（多一跳）。
- 摘除**不是**就地 `erase`，而是 `asio::post` 延后一拍 —— 完成回调可能正由该 signal
  的取消路径调用，handler 尚未退栈，就地销毁会连槽一起毁。

### 为什么 `pylai:server` 不需要 pimpl

`plf::hive` 作为**未导出的私有成员类型**（在接口单元的 GMF 里 include）就够用：
模块的接口单元本身就是编译边界，改 `server.cpp` 不触发依赖方重编 ——
pimpl 存在的最大理由已经由模块代劳。

### 线程模型：❓ 待定 —— 与线程池一起设计（`http-server-plan.md` §5）

- ⚠️ **修订（2026-10-14）**：此前写的“每线程一个 `io_context` + 各自一个 acceptor（SO_REUSEPORT），对齐 hical”是**过时说法** ——
  我们**不打算对齐 hical**；单机之内的并发 / 分片模型**还没定**，要和**线程池尺寸一起设计**。
  下面那张“每线程一套 `Router` ⇒ 访问天然无同步”的推论，只有在那个模型定下来之后才成立 —— **现在只当候选方案记录**。

- 候选：**每线程一个 `io_context` + 各自一个 acceptor**，于是每个线程一整套 `Router`，`sessions` 只被所属线程访问：

  | 访问 | 冷/热 | 同步 |
  |---|---|---|
  | `emplace()`（建连） | **热** | 无 —— 就在本线程的 accept 协程里 |
  | `erase()`（会话结束） | 悲观 | 无 —— 已在本线程 |
  | `stop()` 的遍历 emit | **悲观** | 无 —— 调用者需在本线程上 |

- **不用 strand**：strand 会让同一逻辑流在多核之间游走，**破坏绑核的内存局部性**
  （hical 那种把线程绑 CPU 的思路值得保留）。
- **也不用 `atomic_bool` + 双检**：检查通过到 `emplace()` 之间存在窗口，与 `stop()` 的遍历
  仍会并发（hive 的 Insertion vs Iteration = No）—— 这是窗口本身的产物，不是概率问题；
  而且它覆盖不到 `erase()` 之间的并发。
- ⚠️ **当前只有一个 `io_context`**（还没到“每线程一套”，那是“动态池”那一阶段）。
  在那之前 `Settings::io_threads` 必须保持 **1**（`Server` 构造函数里有 `assert` 钉住）。

