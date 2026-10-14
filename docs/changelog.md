# 变更史 —— 历史论证（新在上）

> **这里是"账本"，不是"说明书"。** 现状设计看 `http-server-plan.md` / `routing.md` / `modules.md` 等专题文档；
> 本文件只记**怎么走到现状的** —— 当时怎么想、谁推翻了什么、改成了什么、怎么复核。
> 更早的历史看 `git log`。

## 写作约定（2026-10-14 定）

- **一轮 = 一次讨论 + 一次可评审产出**，编号 **`S<阶段>-R<轮次>`**（阶段内从 1 计数）。
  阶段号见 `http-server-plan.md` §9：0 协议基线 → 1 框架形态定形 → MU → 2 有界资源 → 3 观测 → 4 动态池 → 5 离线扫参 → 6 建模 → 7 H3。
  只有一个日期时"一天里的多轮讨论"分不清前后，编号就是为解决它而设。
- **本文件是流水账**：按讨论顺序叙述论证过程，**不压缩、不提炼**。
  想让别人一眼看懂现状，请改专题文档，不要改这里。
- **分节依据 = 讨论发生的阶段**（不是功能归属）；`工具与构建` 一节收跨阶段的工程杂项。
- **编号规则确立之前的条目（2026-10-14 之前）不补编号**，只按阶段分节、保持原样；编号从下一个轮次开始。
- 被推翻的结论**不删**：标「修订」，写清改成什么、为什么。
- 证据：结论给 `文件:行` 或可直接执行的命令；状态标 ✅ 已验证 / 🟡 倾向 / 🔍 待调研 / ❓ 未定。

---

## 阶段 1 · 框架形态定形

- **2026-10-14 · S1-R4 · 文本与字节分开：字节一律走 `std::byte`** ✅ —— 用户要求“在项目中区分字符串 `string_view`/`string` 和字节串 `span<byte>`/`vector<byte>`”。
  盘出的现状是：`Request::body()` 返回 `std::string`、`read_some` 收 `span<char>`、`ResponseWriter::write` 收 `string_view`、
  成帧函数收 `string_view`、`Session::buffer` 是 `array<char>` —— 全都是“拿 char 当字节”。用户拍板三条：
  ① 字节类型用 **`std::byte`**（不参与算术，谁也拿不走当字符用）；② **公开 API 与传输层一起改**；③ 便捷写法用**双重载**。
  落地产出：接口直接用 `std::span<const std::byte>` / `std::span<std::byte>` / `std::vector<std::byte>` + `as_text(...)`
  （显式的“换个看法”，反方向用标准库 `std::as_bytes`）。改了：`BodyReader::read_some`、`Request::body()`/`read_some`、
  类型化 `Request` 的同名接口、`ResponseWriter::write`、`BodyResponse::body(块)`/`pending_`、`chunk_bytes`/`last_chunk_bytes`、
  `Parser::execute`/`read_body`、`Session::buffer`/`read_some`/`write`、`route::text_response`、tests 的 `peek`/`echo`。
  ⚠️ **保留文本的两处**（有意为之，不是漏改）：`head_bytes` 仍返回 `std::string` —— 头部**本来就是文本**（状态行 + 字段），
  它在 `send_head` 里用 `std::as_bytes` 显式换成字节交给通道；`session.cpp` 的 `remote`、错误消息、`parser.error_reason()` 同理是文本。
  ⚠️ **cast 边界只剩 llhttp 一处**（`const char*` 的 C API）：`Parser::execute` / `read_body` 各一次 `reinterpret_cast`，
  代码里点明了“这是唯二必须 cast 的边界”；`asio::buffer` 对 `std::byte*` 是原生支持的，asio 侧零 cast。
  便利性：`body(块)` 与 `text_response(...)` 都给 `string_view` 重载 —— 文本可以当字节看（子集关系），反方向不行（编译期就拦下）。
  ⚠️ **修订（我多做了一步）**：我最初顺手加了三个别名（`http::Bytes` / `MutBytes` / `ByteBuf`），用户当即叫停 ——
  **`std::span` 的语义已经足够明确，别名是多余抽象**。已删；接口与实现现在都写裸类型。
  验收：构建退出码 0；e2e **29 条一字未改、全过**（`tests/http_e2e.py` 本次零改动）。复核：`cmake --build --preset debug_llvm` + `./tests/run.sh`。

- **2026-10-14 · S1-R3 · 字符串构造统一走 fmt** ✅ —— 用户要求“改进所有的字符串格式化和非日志的打印，采用 fmt”。
  前置核实：`fmt` **12.2.0** 本来就在 `vcpkg.json` 里（`spdlog` 1.17.0 用的就是**外部** fmt —— `spdlog/tweakme.h` 里
  `SPDLOG_FMT_EXTERNAL` 已定义），所以**只有一份 fmt**，不存在与 spdlog 内置副本打架的问题；
  vcpkg 的 fmt **没装 C++20 模块**（`share/fmt/` 里无 modules 产物）⇒ `import fmt;` 不可用，
  正好与既有约定一致（`docs/modules.md`：第三方库不封 module，直接 include 头文件），CMake 侧 `find_package(fmt CONFIG REQUIRED)` + `fmt::fmt`。
  改动：`head_bytes` 的状态行 / `Trailer:` 预告 / `append_header` / `append_chunk`（十六进制块长）全走 `fmt::format_to(back_inserter(out), …)`
  （直接往 `out` 上追加，**不产生中间 string**）；`req.version`（原两处 `std::to_string` 拼接）、`Content-Length`（`fmt::to_string`）、
  `session.cpp` 的 `remote`（`{}:{}`）与 413 消息、`route.cpp` 的 `Allow` 列表（`fmt::join`，顺带把“插 HEAD”挪到摊平之后 —— 输出逐字不变）、
  `tests/main.cpp` 的响应体构造改用 `fmt::format`、`std::fprintf(stderr, …)` → `fmt::print(stderr, …)`。
  取舍：`append_chunk` 原本用 `std::to_chars(…, 16)`（标准库最快的整数转字符），换成 `{:x}` 是为了**一处成帧逻辑只写一行**；
  两者都在纳秒级，相对一次 `async_write` 可忽略 —— 将来若真压出热点，再上 `FMT_COMPILE`（编译期解析格式串）或退回 `to_chars`。
  验收：构建退出码 0；e2e **29 条一字未改、全过**（用例本身断言的就是格式化后的字节，比如 `user id=42`、`Allow: GET, HEAD`、trailer 名 ——
  29/29 全过即“格式化结果逐字不变”）。复核：`cmake --build --preset debug_llvm` + `./tests/run.sh`。

- **2026-10-14 · S1-R2 · 响应改成「链式填状态 + flush 出网」** ✅ —— 用户要求“各种 Response 都加 `write(头)` / `write(体)`，
  这才是 handler 驱动的读写”，并给出形态：链式 setter 写状态、`operator co_await` 做 flush、顺序错抛异常。
  我问了三个细节，用户定：**自定义 `error_category`**（不是 `bool`）· **显式 `finish()` + 框架兜底** · **字段私有化只走链式**。
  落地产出：`http::WriteErrc`（`closed` / `timed_out`）+ `write_category()`；`ResponseWriter::write` 改返回 `std::error_code`；
  `ResponseBase<Derived>`（CRTP：链式 setter 返回**具体类型** ⇒ 可混着链）+ `BodyResponse<Derived, streaming>`；
  三种响应各自 `flush()` / `finish()`，**状态机** `config → head_sent → finished`（头发出后再改 status/header ⇒ `throw_out_of_order`
  抛 `std::logic_error`）；`route` 在 handler 返回后兜底 `co_await response.finish()`（幂等）。
  ⚠️ **修订（我的方案被标准否掉）**：`co_await res` 这种字面语法**做不出来** —— asio 的 promise 有成员 `await_transform`，
  标准规定它**优先于** `operator co_await`（promise 只接受 `awaitable<T,Executor>` 与满足 `is_async_operation` 的对象，
  `asio/impl/awaitable.hpp:186`），所以 `operator co_await` 根本没机会参与（编译报错：11 errors，全部指向 `await_transform` 候选）。
  要真让 `co_await res` 生效，得把响应做成 asio 的 **async operation 对象**（`operator()(token)` → `async_initiate` +
  `ResponseWriter::get_executor()` + `co_spawn` 启动 flush，`asio/async_result.hpp:861` / `:985`）—— 用户拍板**不做**：
  代价是每次 flush 多一份协程帧分配、且与等待方脱离父子关系（异常/取消传播变弱）。**最终形态：链尾 `co_await res.flush()`。**
  ⚠️ **修订**：接口名换了一套 —— `declare_trailer` → `trailer(name)`；推式的 `write(块)` → `body(块)`（**只攒内存，不碰 socket**）
  + `flush()`（出网）；`send()` 取消，统一成 `finish()`（幂等 + 框架兜底）。`EmptyResponse` 干脆**没有** `body()`：
  能在类型层面拦的错就不要留到运行期抛。
  ⚠️ **修订**：“惰性发头”（第一次 `write()` 顺带把头发出去）这个隐藏时机没了 —— 头发不发完全由 handler 的 `flush()` 决定，
  于是“**先发头、再慢慢读请求体、再推体**”这才表达得出来（`Expect: 100-continue` 的口子由此打开，自动回 100 未做）。
  `Connection` 的采样/冻结仍在**首次出网那一刻**（`session.cpp` 的 `write` 首次调用），语义比之前更清楚：那一刻的 `body_finished` 说了算。
  （工程杂项同一轮：建了仓库根 `.venv`（Python 3.14.8）给测试脚本装库用，已进 `.gitignore`。）
  验收：构建退出码 0；e2e **29 条一字未改、全过**（`tests/http_e2e.py` 本次**零改动** —— 新接口行为等价）；
  `/stream` 用例覆盖“两次 `flush()` 分段推 + `finish(trailers)`”的全过程。复核：`cmake --build --preset debug_llvm` + `./tests/run.sh`。

- **2026-10-14 · S1-R1 · 响应类型三种落地：响应自己发，框架只给写通道** ✅ —— 起点是用户问“发体途中还能发头吧？得讨论一下”。
  查 RFC 后确认：**能，但只有一处** —— 响应侧 chunked 的 `trailer-section`（RFC 9112 §7.1.2 `chunked-body = *chunk last-chunk trailer-section CRLF`），
  且受三重约束：必须 chunked 定界（`Content-Length` 定界的响应根本没有 trailer）；字段得是**其定义允许出现在 trailer** 的
  （`ETag` / `Accept-Ranges` / `Authentication-Info` 等，RFC 9110 §6.5.1）；**中间设备“往往直接丢掉”** ⇒ 不能放用户必需的信息；
  另 §6.6.2 说 `Trailer:` **SHOULD** 预告、§8.8.3 说 `ETag` 可作 trailer（但“除非生成于发送内容期间，否则宁可作头”）。
  ⇒ **决定甲：trailer 作为写通道的能力提供（`declare_trailer` 预告 + `finish(trailers)` 交付），不搞“边发体边发头”的通用机制。**
  落地产出：`http::ResponseWriter`（`write` / `keep_alive()` / `head_only()`，`server/http.cppm:88`）+
  成帧纯函数 `head_bytes` / `chunk_bytes` / `last_chunk_bytes`（`server/http.cppm:108~118`）+
  三种响应类型（`StringResponse` / `EmptyResponse` / `UnknownLengthResponse`，各自 `awaitable<bool> send(ResponseWriter&)`）+
  `IsResponse` 概念（`server/http.cppm:173`）⇒ `make_handler` 按**静态已知**的响应类型构造对象（零分配）后 `co_await response.send(writer)`。
  ⚠️ **修订（我自己的错）**：讨论时我给的选项表把“值语义 variant”列为备选 C，**不成立**——推式响应必须穿过一条活的写通道，塞不进 variant；
  而 variant 也没有用户：具体类型在 `make_handler` 里静态已知（`Pmf` 是模板参数）。用户拍板“不要 variant”，最终形态 = 具体类型 + 概念约束 + 编译期分派，
  **响应侧零虚函数**（唯一的虚接口是写通道本身）。
  ⚠️ **修订**：`204` / `304` 及 `1xx` **不得带 `Content-Length`**（RFC 9110 §8.6）；旧实现一律写 `Content-Length: 0` ⇒ 已修，e2e 钉住。
  ⚠️ **修订**：“响应形参可以不写”作废 —— 现在**必需**，且必须点名具体响应类型（`static_assert` 给出三选一的提示）；
  理由：类型即“我产出哪种响应”，不写就退化成运行期分支。
  ⚠️ **新发现的硬约束**：`Connection` 的值必须**在写响应头之前采样、之后就冻结**（`Session::write()` 首次调用时定，`server/session.cpp:286~291`）——
  推式响应的头在 handler 还活着时就出去了，此后 handler 再读体也改不了它；采样用的就是“体读完了吗”（`body_finished`），
  ⇒ 要么在第一次 `write()` 之前读完体，要么就是 `close`。
  验收：既有 e2e **26 条一字未改、全过**；新增 3 条（`/stream` 的 chunked + `Trailer:` 预告 + 末块后 trailer 字段；stream 之后 pipelining 仍可用；
  `OPTIONS /health` 断言 204 **无** `Content-Length`）⇒ **29/29**；构建退出码 0。`tests/http_e2e.py` 的改动是**纯新增**（`git diff` 里没有一行既有断言被改）。
  🔍 留下的缺口：`Expect: 100-continue`（`on_headers_complete` 交棒后没人立刻回 `100`，客户端可能干等；RFC 9110 §10.1.1 要求服务端要么发终响应、要么**立刻**发 `100`，不得等体）——单独一轮。
  复核：`cmake --build --preset debug_llvm` + `./tests/run.sh`；文档同步 `routing.md` §1/§1.2、`http-server-plan.md` §4.1.1/§4.1.2。

- **2026-10-14 · 文档重组：历史论证从 plan 搬到这里；changelog 改定位**（规则确立，不编号）——
  用户指出两点：① 讨论轮次要**小步快跑、每轮一个编号**（原先只有日期，一天多轮分不清前后）；
  ② `changelog.md` 的定位改为**专记历史论证**（流式叙述），`http-server-plan.md` 只留现状设计。
  本轮把 plan 里的日期戳 / `🔄 修订` / ~~删除线~~ / 落地进度块整体搬进本文件，plan 顶部只留一行指针；
  顺带核对 plan 里所有 `文件:行` 引用（交棒那刀之后大多失效，已逐条重新核对：`http.cpp:144` / `220~229` / `137`、
  `http.cppm:96`、`session.cpp:131` / `139` / `256`、`session.cppm:47`、`route.cppm:24`）。
  本轮的"规矩"写进上面的「写作约定」；下一个轮次的产出开始带 `S1-R1` 这样的编号。
  复核：`docs/http-server-plan.md` 头部指针行 + 本文件「写作约定」。

- **2026-10-14 · 交棒：框架读到 headers 就把体交给 handler** ✅ —— 先出设计（`http-server-plan.md` §4.1.2，含落地时的两处修订）再落地。
  框架只解析到 `on_headers_complete` 就交棒：新增 `http::BodyReader`（抽象基类，`Session` 自己实现；擦除点合并后会重构掉）+
  `Request::body()` / `read_some()` / `has_body()`；**删掉 `Request::body` 这个数据成员**（框架不再读完体，留着就是谎）。
  413 有两条路：`Content-Length` 在头部阶段预检（不进路由），长度未知（chunked）由读体侧计数拦（`BodyError{413}`）；
  未读体 ⇒ 响应写 `Connection: close` 并关连接；**不做**请求总时限（用户拍板）。
  实现要点：每次喂给 llhttp 的输入窗口**夹到调用者缓冲大小** ⇒ `on_body` 拿到的载荷必定放得下（不必在回调里 pause、不必挂 span）；
  进度记账不需要新机制（就用 llhttp 的 `consumed`）；读体复用 `fill_buffer` 的超时 ⇒ 绕不过超时与取消；**拉取式天然背压**。
  ⚠️ 踩坑：`on_headers_complete` 返回 `HPE_USER` 时 llhttp 报的是 `HPE_CB_headers_complete` ⇒ 超限判定要按自己设的标志位（症状：413 变 400）。
  验收：既有 e2e **22 条一字未改、全过** + 新增 4 条（`close头` / `unread` / `halfread` / chunked 超限）⇒ **26/26**；构建退出码 0。

- **2026-10-13 · `Param<>` 变成 `Request` 的成员函数；静态用例搬出模块** ✅ —— 两件事同一动机：声明搬到第一个形参后，
  请求对象本身就带捕获（`Request` 新增 `std::span<const Capture> captures`），路径参数不必再占一个形参位。
  `req.param<T, "名字">()`：名字**必写**，`slot_of("名字")` 在编译期算出下标（`static_assert` 拦错，`from_chars` 语义与旧的相同）；
  ✅ **删掉** `Param` / `IsParam` / `ParamCount` / `SlotOf` / `check_param_name` / `validate_route` 里的计数与名字校对。
  ⚠️ 代价：“pattern 里的名字没人读”不再算错（读取在 handler 体内，编译期看不到）。
  ✅ 编译期用例整体搬到 **`tests/route_static.cpp`**（99 条 `static_assert`，`route.cppm` 从 1361 行降到 1051 行）——
  它只走 `import pylai;` 的**导出面**，顺带验收“该导出的都导出了”；`tests/CMakeLists.txt` 的 GLOB 自动收，无需改 CMake。
  复核：`cmake --build --preset debug_llvm`（构建过 = 用例过）→ `./tests/run.sh` 22/22，用例文件一字未改。

- **2026-10-13 · 架构变更①落地：声明搬到第一个形参** ✅ —— `asio::awaitable<void> handle(Get<"/health"> req, http::Response& res)`。
  `Endpoint`（住在返回类型位置的声明）换成 `Request<verb,pattern>`（**请求类型**，持 `http::Request& raw`）；`RouteOf` 改从 `Ps...[0]` 取声明；
  `make_arg` 按形参类型分三类（请求 / `Param<>` / `Response&`）；`make_handler` 直接 `co_await` handler，响应由框架持有并返回。
  ✅ **删掉**：`std::coroutine_traits` 特化、`Inner`、`release()`、以及绕 `await_transform` 模板推导的那段注释 —— 整个 DSL 里最拐的一处没了。
  验收：**`./tests/run.sh` 22/22，且 `tests/http_e2e.py` 一字未改**（“运行期行为零变化”的可验证形态）；新增两条编译期诊断。
  ⚠️ 消费方现在要自己 `import asio;`（已记进 `modules.md`）。`routing.md` §1 重写。

- **2026-10-13 · 目标结构重排：四层目标 + 两根轴 + MU 里程碑** 🟡 —— 用户澄清三点：clio **尚未施工**（⇒ 框架形态自由）、但另有**公网内容供应**等消费者（需求差异大 ⇒ 动态调优的由来）、且承认**想法太多导致迟迟达不到最小可用**。
  §1 重写为四层：①**为什么做**（场景自适应 —— 是差异化，不是收尾）②**质量底线**（把"不亏欠"翻译成三条可验收：性能**无结构性劣势** / 易用性**形式能改** / 功能覆盖 = **每个缺口有名字 + 挂点**）③**地基**（协议基线 · 框架形态 = 两根轴）④**§1.5 留白清单**。
  §1.4 非目标定案：**不做多机，但要生产级健壮性**；§1.6：clio 从【定】降为"**第一个消费者**"——
  理由：原先记的那些是 **clio 侧**的部署约定（只监听 `127.0.0.1`、基地址 `127.0.0.1:7788`、模块命名 `herodotus.clio.<domain>`…），
  不是本仓库的约束；本仓库只保证"**可配置**"（`Settings` / `Builder`），不把 clio 的取值写死。
  §9 新增 **MU（最小可用）**：形态定形 + e2e 全绿 + 模块可 `import` + 缺口成清单，**只服务 clio**。
  编号已定（2026-10-13）：**0 协议基线** → **1 框架形态定形** → **MU** → **2 有界资源与 60s 基线** → **3~7** = 原 1~5（观测 / 动态池 / 离线扫参 / 建模 / H3）。
  🔄 这次编号本身就是修订：`Stage 0` 原先混了**两根轴**（协议基线 + 框架形态），
  而其中三项（声明式路由 / 协议层应答 / 请求体）会被形态变更**重新打开** ⇒ "Stage 0 快完成"是错觉。
  划分依据：**观测的单位就是请求生命周期** —— 形态不定就建指标必返工（同 §7"挂点要提前留"）。
  顺带修 §10 三处过期项：非目标、TLS 的阶段号、"`handler 形态 = awaitable<Response>`"（已被 §4.1.1 推翻）。

- **2026-10-13 · 架构变更（已定，待落地）：交棒给 handler + 擦除点合并 + 声明搬到形参** 🟡 —— 用户 2026-10-13 提出三项重大变更。
  ① handler 自己**读体**、框架**读到 headers 就路由**（不再读完体再派发）；② handler 的**类型擦除**从“每条路由一个”降到“每个动词一处”（让匹配与调用合体）；③ **声明从返回类型搬到第一个形参**，响应类型多种（beast 式）。
  已定：`asio::awaitable<void> handle(Get<"/health"> req, FileResponse res)` —— `req` 是**请求本身**（读体接口在它上面，含 `read_some`）、**响应由框架发**（发法由响应类型决定：String / Empty / File / 未知长度-chunked）；**未读体 → 直接关连接**；② 排到 ① 之后。
  ✅ 随带的大简化：③ 之后 `Endpoint` 不再是协程返回类型 → `coroutine_traits` 特化与 `release()` 可删。`Request::body` 会删掉（框架不再读完体，留着就是谎）。
  写进 `http-server-plan.md` **§4.1 / §4.1.1**（并给 §3.1 / §11.2 C / `routing.md` 加了“将被取代”的指针）。尚存 🟡：返回类型、`res` 的所有权、handler 未填响应时的默认。

- **2026-10-13 · 离线条件变了 ⇒ 调参分工要重估（L4 层，`http-server-plan.md` §7.2）** ——
  起因：我最初是在"**不能停机**"的假设下给结论的（原话：*"只有冷层值得跨运行记忆"*、*"只能在线闭环"*）；
  **用户指出我们的服务器可以停机维护、可以重启调参**，于是判断要改。
  - **"能否在线改"不再限制"能否实验"**：重启才生效的参数（`io_uring_ring_count` / `ring_size` / `submit_batch_size`、预分配量、分配器参数）
    现在可以被**主动搜索**，而不只是当"先验"。
  - **拿到了 PGO 最大的好处：干净归因** —— 重启 = 状态归零 ⇒ 每次实验同一起点 ⇒ 参数差异可归因，正好解掉"混杂效应 / 一次只能动一个维度"的顾虑（仅当每次实验都重启时成立）。
  - ⚠️ **但 PGO 真正的难点没被解决：代表性负载。** 维护窗口里通常没有真实流量：
    能**重放生产流量**或用**与生产校准过的生成器**才叫真 PGO 流程；否则窗口里测出的 $\lambda/W/S$ 不代表生产。
    **这一条比"能不能重启"更决定成败。**
  - ⚠️ **新引入的方法学陷阱：短窗口 ≠ 稳态** —— 缓存冷、连接在爬坡、池 / 定时器堆 / ring 没填满，必须丢预热期；
    更要紧的是**长期累积效应**（碎片、池增长、哈希冲突、fd 增长）在几分钟窗口里显现不出来 ⇒
    **离线窗口会系统性低估 USL 的 $\sigma/\kappa$**。所以离线扫参只适合"快速、无状态、效果立刻可见"的参数。
  - **修订后的分工**：**离线（PGO 形态）找起点 / 结构，在线（控制形态）治窗口之间的漂移 —— 两者是分工，不是二选一。**
  - **修正原结论（诚实标注）**：「只有冷层值得跨运行记忆」→「冷层**不仅该记忆，还可以被主动搜索**」；
    分层依据从"生效时间"改为 **"改它是否需重启" × "效果是否需要真实 / 长期负载才显现"**（`http-server-plan.md` §7.1 已同步）。
  - 附带两条：**窗口预算是硬约束**（一次实验 = 停机 + 启动 + 加载 + 跑够时长 + 丢预热 ⇒ 实验次数有限 ⇒
    更该"拟合模型 + 少量点"而不是穷举网格，USL 用 3~5 点就能定 $\sigma,\kappa$）；**跨窗口累积**才是 PGO 形态（每个窗口贡献几个测点，指纹在这里才真正发挥作用）；
    ⚠️ 升级 asio / 编译器 / 内核会让老测点作废（等价于 PGO profile 跨版本失效）。

---

## 阶段 0 · 协议基线

- **2026-10-13 · 请求体：接上 llhttp 的 `on_body`** ✅ —— `Request::body`（chunked 已由 llhttp 解码）；
  `Settings::max_body_bytes`（默认 1 MiB）+ `Builder::max_body_bytes()`：超限时 `on_body` 返回 `HPE_USER` **就地中止解析** → `ParseStatus::too_large` → **413 + 关连接**。设计见 `http-server-plan.md` §3.1。
  复核：`./tests/run.sh`（22 项全过，含 chunked 分块、空体、1025 > 1024 的 413、以及带体请求与下一请求的 pipeline）。

- **2026-10-13 · 协议层应答：405 / OPTIONS 带 `Allow`，HEAD 复用 GET** ✅ —— 见 `http-server-plan.md` §11.2 G。
  `dispatch`（`server/route.cpp`）里“逐棵动词树试一遍”的结果现在被留下来算 `Allow`；HEAD 没有显式 `Head<>` 时落到同路径的 GET。
  正文抑制收口在 `http::serialize(response, keep_alive, head_only)`（`session.cpp` 传 `req.method == "HEAD"`）→ 显式 `Head<>` 也不会漏发体。
  ⚠️ 用户决定：`Allow` 里 **GET 存在时带上 HEAD**（RFC 9110 §9.3.2），紧跟 GET。
  复核：`./tests/run.sh`（17/17，含 `Allow: GET, HEAD, POST`、`HEAD /both` 的 `Content-Length: 14` 证明显式优先）。

- **2026-10-13 · `pmf` 类型萃取改用 jk 的 `Callable`；形参表换 `TList`** ✅ —— 用户提示 jk 有 callable 工具。
  `RouteOf` 不再手写 `RouteOf<R (C::*)(Ps...)>` 偏特化（只匹配裸形式），改用 `JK::Meta::Return` / `Arguments`；
  `make_handler` 也不再分解 `R / C / Ps`。`Params` 由 `std::tuple` 换 `TList` → `ParamSlot` 递归 + `SlotOf` 偏特化（20 行）
  合并成 5 行的 `SlotOf<i, L> = ParamCount<L::Slice<0, i>>`；`tuple_element_t` / `tuple_size_v` → `At<i>` / `size()`。结构上净删约 16 行
  （文件总行数 1392 → 1391：删了 16 行，又加了 16 行钉住 cv/noexcept 新能力的用例）。
  ⚠️ 新增依赖面：`route.cppm` 现在 `import jk;`（此前只有 `session.cpp:21` 用它），`RouteOf<Pmf>::Params` 对外是 `JK::Meta::TList`。
  复核：构建绿 + `./tests/run.sh`（9/9）+ `static_assert` 101 → 104；反证旧写法匹配不上 `const` / `noexcept` 见 `.vscode/tmp/jk-callable-probe/old-trait.cpp`。

- **2026-10-13 · `RouteGroup` 包装层删除：`Router` 直接持有路由表** ✅ —— 用户追问“只有一个，为什么不直接用 `methods`”。
  `RouteGroup` 原本只是 `{ std::vector<VerbRoutes> methods; }`，而 `groups` 用 vector 是**不完整类型的逃生舱**（`optional`/按值都不行），不是“想装多个”。
  现改成 `Router::methods` 一个成员 + `VerbRoutes` 前置声明；`set_group`/`make_group` → `set_routes`/`make_routes`（不再有包装类型）。
  `dispatch` 去掉两层 `for` 中的外层（`server/route.cpp:39,48`）。
  复核：构建绿 + `./tests/run.sh`（9/9）+ `clice query --method documentSymbols` 里 `RouteGroup` 已消失。
  ⚠️ 工具发现：`clice` 的 `symbolSearch` **两个方向都会骗人** ——
  ① **漏**：`make_routes` 在索引里 `kind=Invalid` → 搜不到（`clice index --show-symbol make_routes` 能看到它）；
  ② **残留**：`ParamSlot` 已从源码删掉，符号表里却还留着（`--show-symbol` 仍列出，但那行**没有 `文件:定义数`**部分是残留特征）。
  记：**存在性判据一律落回源码 grep + 编译**。

- **2026-10-13 · `Pattern` 的数组维度改紧（不再按 raw 长度过分配）** ✅ —— 用户指出：**段数就是 `count('/')`**。
  拆掉原来兼三用的 `cap`：`segment_count = count('/')`（段，合法 pattern 精确）、`name_count = count('{')`（命名捕获上界）给数组，
  `node_budget = raw.size() + 1` 留给 `k_node_bound`（**值不变 ⇒ 建树行为一字不动**）。
  🔄 **修订**：更早的一版我曾提出"数组长度进类型"（元编程，实验在 `.vscode/tmp/route-digest/pieces_by_type.cpp`），
  并宣称省 69~88% 槽位 —— **那个对比用错了基准**：`count(raw, '/')` 用 `constexpr` 朴素数一下就能拿到，
  不需要元编程；按"现状 / 朴素精确 / 技巧"重算：`/health` 16 / **1** / 2 · `/api/v1/echo` 26 / **3** / 4 ·
  `/files/{path...}` 34 / **3** / 4 · `/a/{x}/b/{y}` 26 / **6** / 8 ⇒ **朴素精确全面优于技巧**，技巧在此处没有性价比。
  复核：`route.cppm` 新增 5 条 `static_assert` 钉住维度 + `./tests/run.sh`（9/9）。
  ⚠️ 顺带修正：我先前写"节点上界靠 label 非空"是**错的** —— `end_mark` / 匿名 `catch_all` 的边 label 是空的。

- **2026-10-13 · 段内前后缀：重新评估后决定不做** ❌ —— 曾被要求加上（`img-{id}.png`），语义也定好了（一段内最多一个参数、非贪婪失败再放宽、
  `{x...}`/`{$}` 不进段内），但推冲突规则时撞上**无法静态消解**的歧义：`img-{id}.png` 与 `img-{name}.jpg` 会同时命中 `img-a.png.jpg`。
  根因：段级安全靠的是"**段文本不同 ⇒ 不相交**"这条对齐性质，段内参数把它打破了（参数可跨过后缀）。细节 → `http-server-plan.md` §11.2 A。
  顺带记下两条讨论结论：`Pattern`/`PieceCursor`/`insert_pattern` 是**三层级划分**（输入不同、不是重复实现）· **「先算后建」是权宜**（源自
  `std::vector` 不能进 `constexpr` 变量）而非规范做法 —— 见 §11.2 C。

- **2026-10-13 · `server/` 降为框架库，示例应用搬到 `tests/`** ✅ —— `server/` 的 target 由可执行 `pylai` 改为**静态库 `pylai`**
  （模块文件集 PUBLIC；asio-module/joker::module 提为 PUBLIC，供消费方 `import pylai;`）；
  `server/main.cpp` 删除，示例应用 `App` 搬到 `tests/main.cpp`，可执行 target 名 **`app`**（`build-dev_llvm/tests/Debug/app`）。
  E2E 脚本与入口一并收进 `tests/`：`http_e2e.py` + `run.sh`（查端口→起服务器→断言→收工）。
  复核：`cmake --build --preset debug_llvm` → `./tests/run.sh`（7/7，退出码 0）。
  🔄 这里的"入口归属"本身是一次修订：早先定的是"入口归 `server/`、`pylai` 就是可执行文件"，
  现改为"`server/` 只出框架库（无 `main`），示例应用 + 入口在 `tests/`" —— `πύλαι` 这个名字留给框架库。

- **2026-10-13 · 路由：每 method 一棵树 + 冲突检查接线 + 单次 `route_on`** ✅ ——
  ① 实测出「同一棵树里 `Get<"/x">` 与 `Post<"/x">` 会互相覆盖槽位」（`RadixNode::route` 只有一个）→ 改为**按动词分树**
  （`SelectPatterns` 筛 pattern、每个动词一棵 `build_of()`，各树擦成闭包放进 `VerbRoutes`）；
  ② `validate_group` 接进 `route_on`：冲突时实例化未定义的 `ShadowedRoute<两条 pattern>`，**编译错误里直接点名**；
  ③ `route_on` 返回只有 `build()` 的 `Configured` → **第二次 `route_on` 编不过**（跨组冲突因此不存在）；
  ④ 删掉 `Table` / `On` / `table_selftest`。复核：`./tests/run.sh`（9/9，含 `GET /items` 与 `POST /items`）；细节 → `http-server-plan.md` §11.2 B/C。
  📌 这段时期 `route.cppm` 的落地面貌（当时记的进度快照）：段级视图（`Segment` / `PatternView` / `Pattern<S>` / `params()`）+ 段级冲突检查
  （`segments_disjoint` / `routes_conflict` / `RouteConflict` / `find_table_conflict`）+ 运行期字符级 radix
  （`RadixKind` / `RadixEdge` / `RadixNode` / `RadixTree` / `Capture` / `PieceCursor` / `insert_pattern` / `build` / `match`），
  编译器用例 104 条 `static_assert`，文件 1391 行 —— ⚠️ 这些行号/条数都已过期，只作"当时长什么样"的留档。

- **2026-10-13 · 移除旧字符串路由接口** ✅ —— 删掉 `Router::add/get/post/put` 与精确表（`dispatch` 的 ①③ 两步）、
  `Builder::route/get/post/put`、以及只为旧 API 存在的 `operator""_route` UDL；注册**只剩** `Builder::route_on`，
  `dispatch` 简化为「组匹配 → 动词核对 → 405/404」。理由：两套注册方式 = 两套语义（精确表是"最后一次覆盖"，
  声明式是"编译期定容分组"），留一套可消除歧义。
  复核：`cmake --build --preset debug_llvm` → `./tests/run.sh`（7/7；`/echo` 现为 404）。

- **2026-10-13 · 位置注入 DSL 落地到 `Router` / `Server`，端到端跑通** ✅ —— 修掉注入链最后一环的类型断层
  （`make_arg` 的捕获形参由 `span<const std::string_view>` 改为 `span<const Capture>` 并取 `.value`；
  `make_handler` 接住 `match` 的 `[[nodiscard]]` 槽位）；`server/main.cpp` 改用 `route_on`（当时还临时留着
  一条精确表路由做对照，**已随上条移除删掉**）。
  复核：`cmake --build --preset debug_llvm` → `./tests/run.sh`（7/7 通过）。细节 → `http-server-plan.md` §11.2 C。

- **2026-10-13 · 路由声明 DSL 落地**（"位置注入"风格）✅ —— 动词 + 路径放在**返回类型位置**（`Get<"/health">`），
  `Param<T[, name]>` 按**位置**吃第 N 个命名捕获，`Builder::route_on(this, &C::m...)` 一次注册整包；
  返回类型靠 `std::coroutine_traits` 特化借用 asio 的 frame 成为合法协程返回类型。
  ⚠️ 其中"UDL vs class-type NTTP"的插曲：曾实测 **A 路** `template <FixedString S> constexpr auto operator""_route()` 零警告、
  **B 路** `template <typename CharT, CharT...>`（N3599，`ctre` 走这条）会报 `-Wgnu-string-literal-operator-template` ⇒ 选 A 路。
  🔄 这段实测**推翻了**我据 `ctre` 文档下的判断（"UDL 形式必须依赖 N3599"）—— clang 23 上 class-type NTTP 的 UDL 直接可用；
  而 `operator""_route` 本身**已随旧字符串接口删掉**（声明改住第一个形参后不再需要），A/B 的结论只作留档。
  细节与全部证据 → `http-server-plan.md` §11.2 C。

- **2026-10-13 · `Router` 模式感知** ✅ —— `dispatch` = ① 精确表（旧字符串 API）→ ② 声明式组（每组一棵**编译期定容**树 + 槽位表，
  命中后核对动词）→ ③ 路径在但动词不符 → 405。
  🔄 **修订**：①②③ 已收敛为「组匹配 → 动词核对 → 405/404」，①③ 随旧接口移除（见本日"移除旧字符串路由接口"条）。

- **2026-10-13 · Stage 0 收官：认定"协议基线已完成"** ✅ ——
  阶段 0 = 单 `io_context` + io_uring、固定线程数、keep-alive、空闲 / 写超时，**无指标、无自适应、无 TLS**。
  ✅ 已跑通清单：llhttp 解析（pipeline / 半包 / 400 / 431）· keep-alive · 空闲与写超时 · **取消传播**
  （每连接独立 `cancellation_signal` + `plf::hive` 注册表，见 `docs/asio-notes.md`）· SIGINT 优雅停机 ·
  io_uring 后端（实测 `anon_inode:[io_uring]`）· 声明式路由（`route_on` + 编译期冲突检查 + 每 method 一棵树，§11.2 C）·
  协议层应答（405/OPTIONS 的 `Allow`、HEAD 复用 GET，§11.2 G）· 请求体（`on_body` + 上限 → 413，§3.1）。
  ⚠️ 其中**声明式路由 / 协议层应答 / 请求体**三项**随后被 Stage 1 重新打开**（形态变更）—— 见"目标结构重排"条。

- **2026-10-12 · 路由匹配：从"渐进路线"改为"一步到位"（旧调研作废）** ——
  起点是拿 `cpp-httplib`（16.9k★，同类最主流）作参照。**它给正则路由的自我警告值得留档**：
  `std::regex` 匹配长输入会**爆栈**（连 `.*` 都会）⇒ 它给正则路由加了路径长度上限
  （`CPPHTTPLIB_REGEX_ROUTE_PATH_LENGTH` = 256），并写明 *"Path parameters are matched without `std::regex` …
  so prefer them"*。⇒ **"不引 `std::regex`"这条一直成立**（`http-server-plan.md` §11.1）。
  🔄 **被推翻的是路线**：原结论是"Stage 0 用 `unordered_map` 精确匹配（key = method + path，异构查找避免构造 `std::string`）
  → 加参数时换分段匹配 → 必要时 radix tree"，**2026-10-12 晚改为一步到位**
  （字面量 pattern + `constexpr` 建树 + 路径参数编译期注入）⇒ §11.1 整个降为历史留档，§11.2 为准。
  ⚠️ 连带作废：Stage 0 早期曾拿 `svr.Get("/hi", …)` 式的**字符串精确表**做路由 —— 2026-10-13 连同 UDL 一起删除。
  复核：`grep -rn '""_route\|pylai:router' docs/`（只应剩历史条目）。

- **2026-10-12 · 选型：HTTP/1.1 解析定 `llhttp`；`picohttpparser` 改判"可用"** ✅ ——
  ✅ **`llhttp`（用户拍板，可推翻）**：本地 vcpkg 根现成可用（9.3.1，target `llhttp::llhttp_static`）；
  零分配（状态机在调用方内存）；**默认严格校验**（9 个 `lenient` 开关默认关 ⇒ 走私防护），回调式与 `async_read_some` 循环天然契合；
  内置 `llhttp_should_keep_alive()` / `llhttp_finish()`（keep-alive 判定与 EOF 定界）。
  `picohttpparser` 留作**后路**：它的"零分配"优势 llhttp 同样具备，实际只剩 API 极简（4 函数）与**原地** chunked 两点；
  ⚠️ 它自述 *"tiny, primitive"* 且**校验宽松**，不适合直接面向公网。
  🔄 **修订（我原先记的"❌ 不在 vcpkg"是错的）**：官方 registry 已收录（PR #51743，约 2026-05 合入，
  target `unofficial::picohttpparser`）⇒ 不再需要自建 overlay port；旧结论来自**当时本地 vcpkg 根落后**（2026-04-23）。

---

## 工具与构建基线（跨阶段）

- **2026-10-13 · clang-format：链式调用的缩进只能靠 `PenaltyIndentedWhitespace`** ✅ ——
  需求是"`.bind()` / `.max_body_bytes()` 这类链式调用比 `auto server =` 再缩进一个 tab"。
  **查文档得到的硬结论（clang-format 23）：没有任何选项控制它** ——
  续行缩进恒为 **表达式起始列 + `ContinuationIndentWidth`**；`AlignOperands` 只管二元 / 三元运算；
  `PenaltyBreakBeforeMemberAccess` 只管"断不断行"；`ColumnLimit: 0` 只保行不断、**不保缩进**。
  唯一能改变版式的旋钮是 **`PenaltyIndentedWhitespace`**（它只影响代价函数里"缩进"的权重）。
  实测（探针 `.vscode/tmp/clang-format-chain/`）：取 **10** 得到"RHS 换行 + 每层一个 tab"的版式。
  ⚠️ 别顺手全量格式化：仓库不是 clang-format 的不动点（当时 14 个文件 / 约 2147 行会有 diff）。
  复核：`git show c46a094`（值由用户拍板合并）+ `.clang-format` 里该行上方的说明。

- **2026-10-13 · 统一语言标准：`set(CMAKE_CXX_EXTENSIONS OFF)`（`-std=gnu++23` → `-std=c++23`）** ✅ —— clice 索引退化的根因。
  起因：clice 对 `server/route.cpp` 里一部分符号没反应。日志（`.clice/logs/<会话>/master.log`）：
  `BuildPCM failed for module pylai:route: GNU extensions was disabled in precompiled file …/pcm/jk-….pcm but is currently enabled`
  → clice 缓存的 jk PCM 是 `-std=c++23`（jk 的 `CMakeLists.txt:70` 是 `CXX_EXTENSIONS OFF`），我们的 TU 是 CMake 默认的 `-std=gnu++23`
  → clang 拒绝加载 → `JK` 全未声明 → `route.cppm`（今天起 `import jk;`）编不过，连带 `route.cpp` 编不过 → 无 AST → 索引只剩扫描级浅结果。
  复核：重建后新会话 `BuildPCM failed` / `GNU extensions` 均 **0** 条；`route.cpp` 符号 19 → **50**，`Router::dispatch` 恢复 `HasDefinition` + `route.cpp: definitions=1`；`session.cpp`（早就 `import jk;`）同样 19 → 146。
  细节与征兆见 `modules.md`（跨 target PCM 选项必须一致）。

- **2026-10-13 · 本机 clice 配置：`readonly` 必须保持 `off`** ✅ —— 起因：加了它之后编辑器里导航/补全变差。定位：`[project] readonly` 管的**不是**索引写锁，而是
  **给打开的文件建不建完整 AST**；`auto` = 文件一律先只由后台索引作答。证据（`.clice/logs/*/master.log`）：`off` 的会话有
  `pch.cpp:300 PCH built`，`auto` 的会话 **PCH 活动 0 行**，且编辑器的 definition/references 全由 `[perf:index_query]` 直答 —— 而索引会漏符号
  （`make_routes`）也留旧符号（`ParamSlot`）。→ 改回 `readonly = "off"`。
  同时修**索引空转**：`.vscode/tmp/**` 探针不在任何 DB 里却被 tracker 拉进索引队列，每会话刷几十条“无法解析编译命令”（30 → 32 → 46 条，29 条 owed）
  → 加规则 `patterns = [".vscode/tmp/**"]` + `index/lint/format = false`。
  复核：重启服务后新会话真实失败数 **0**、调度 `total=20`（原 41~49）、`PCH built` 恢复、`Loaded CDB … with 63 entries` 仍在（规则未关掉数据库自动发现）。

- **2026-10-13 · 命名规则定稿并落地** ✅ —— `.clang-tidy`：类型名 CamelCase；函数 / 方法 / 变量 / 参数 / 成员 / 常量 lower_case；
  **模板参数分四类**（通用 / 类型 / 模板模板 → CamelCase，非类型 → lower_case）。
  ⚠️ 专用键缺失会**回退**到通用键 → 四行都要写；类型参数的键名是 `TypeTemplateParameterCase`
  （写成 `TemplateTypeParameterCase` **静默无效**）。复核：`tools/clice lint --log-level warn` → 全工作区 0 findings。

- **2026-10-13 · 验证通道改走 `clice query`** ✅ —— `query --fresh` = 先重索引（= 重编译）再回答（新符号出现在索引里 ⇒ 该 TU 编译过）；
  `lint` 只用于命名检查。索引写锁会被编辑器里的 `clice serve` 常驻占用 → CLI 重建不了，需先让 server 松手。

- **2026-10-13 · 文档整理 + `main.cpp` 去掉多余的 `import asio`** ✅ ——
  `asio-notes.md`：删掉只剩一句的「§6 工作纪律」桩（那句话提到文件头）、把「误判期的原始记录」挪到 cancellation 节下作附录、
  §7/§8 顺次重编为 §6/§7（内部与 `http-server-plan.md` 的 `§` 引用同步）、修 `pylai:router`/`router.cpp` →
  `pylai:server`/`server.cpp`（`plf::hive` 现在住在 `Server` 里）。
  `routing.md`：补 §1「怎么声明路由」（`Get<>` / `Param<>` / `route_on`），`Pattern<>`/`Table<…>` 降为 §1.1 并明说**不参与注册**，
  删掉已经不存在的 `_route` UDL 行。🔄 **同日稍后修订**：`Table` / `On` 整体删除，§1.1 已改写为「编译期会拦下什么」。
  复核：`cmake --build --preset debug_llvm`；`grep -rn 'pylai:router\|""_route' docs/` 只剩历史条目。

- **2026-10-13 · 会话记忆瘦身** ✅ —— 信息与历史搬进 `docs/`（asio 相关内容并入 `asio-notes.md` / `modules.md`），
  纪律进 `AGENTS.md`，记忆只留指针。
