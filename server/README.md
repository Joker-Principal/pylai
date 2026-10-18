# server/ —— HTTP 服务器框架（静态库 `pylai`）

## 这个目录的职责

**唯一职责：提供 HTTP server 框架**，产出**静态库** target **`pylai`**。

**这里没有 `main`、没有任何业务路由** —— 框架不替使用者决定有哪些路由。
只用框架起服务器的示例应用（`App`）+ 端到端验证在 **`tests/`**。

- **名字来源**：**πύλαι（pylai）**，"门 / 关口"（如 Thermopylae 温泉关，字面即"热之门"）。
  命名顺着上游 Herodotus → Clio 的古典脉络：HTTP server 正是系统的**唯一门户**。

## 怎么用（使用手册）

> 只想**接进来跑** ⇒ 看下面 1–5（**路由 DSL 全表也在本文**）；想**改框架** ⇒ `docs/http-server-plan.md`。

### 1. 接进来

```cmake
find_package(pylai 0.1 REQUIRED CONFIG)   # 已安装的话（见根 README「安装」）
add_subdirectory(<本仓库> pylai)          # 或当子模块
set_target_properties(myapp PROPERTIES CXX_STANDARD 23)
target_link_libraries(myapp PRIVATE pylai)
```

- 需要 **C++20 模块**：本仓库用 clang 23 + CMake ≥ 3.28，消费方也要开模块支持。
- 代码里一律 `import pylai;` —— 框架**没有头文件**；asio 一律 `import asio;`，**不要** `#include <asio/...>`。

### 2. 最小可跑

```cpp
import asio;
import pylai;

using namespace pylai::route;

struct App
{
	asio::awaitable<void> health(Get<"/health">, pylai::route::StringResponse& res)
	{
		co_await res.body("ok\n").flush(); // 链尾 flush 才碰 socket
	}
};

int main()
{
	auto app = std::make_unique<App>();
	auto server = pylai::Server::builder()
		.bind("127.0.0.1", 18080)
		.io_threads(1)
		.idle_timeout(std::chrono::seconds{30})
		.write_timeout(std::chrono::seconds{30})
		.max_body_bytes(1 << 20)
		.route_on(app.get(), &App::health)	// 声明式注册：编不过 = 路由表有问题
		.build();
	return server.run();
}
```

能跑的版本是 `tests/main.cpp`；跑它：`cmake --build --preset debug_llvm && python tests/run.py`。

`Builder` 上就这些可调项：`bind` · `io_threads`（默认 1）· `idle_timeout` · `write_timeout` ·
`max_requests_per_connection`（默认 1000）· `max_body_bytes`（默认 1 MiB，超了 413 + 关连接）·
`route_on` · `build`。（`route_on` 之后返回的是 `Configured`，它只剩 `build()`。）

### 3. 路由声明（第一个形参**既是声明、也是请求**）

```cpp
using namespace pylai::route;

struct App
{
	asio::awaitable<void> health(Get<"/health"> req, StringResponse& res);
	asio::awaitable<void> user(Get<"/users/{id}"> req, StringResponse& res);
	asio::awaitable<void> sse(Get<"/sse"> req, UnknownLengthResponse& res); // 长度未知：边跑边推
};

asio::awaitable<void> App::user(Get<"/users/{id}"> req, StringResponse& res)
{
	co_await res.body(fmt::format("user id={}\n", req.param<int, "id">())).flush();
}

// 长度未知：边跑边推（响应侧 chunked），结束时补 trailer
asio::awaitable<void> App::sse(Get<"/sse">, UnknownLengthResponse& res)
{
	// 预告 `Trailer: X-Chunks`（必须在发头之前）→ 头 + 第一块由链尾的 flush 一起发出去
	if(co_await res.trailer("X-Chunks").body("chunk-a\n").flush()) co_return;
	if(co_await res.body("chunk-b\n").flush()) co_return;

	const std::array trailers{pylai::http::Header{"X-Chunks", "2"}};
	co_await res.finish(trailers);
}

App app; // 必须比 server 活得久：路由里存的是 &app
auto server = pylai::Server::builder()
	.bind("127.0.0.1", 18080)
	.route_on(&app, &App::health, &App::user)
	.build();
```

| 元素 | 说明 |
|---|---|
| **第一个形参** | 必须是 `Get<P>` / `Post<P>` / …：编译期给出动词与 pattern，同时**就是请求**（method / path / headers 在它里面；`req.raw()` 给框架解析出的 `http::Request`） |
| `req.param<T, "名字">()` | 取一个命名捕获。**名字必写**，且在**编译期**对着 pattern 校对；整数走 `from_chars`，`std::string_view` 直通 |
| `co_await req.body()` | 读完整个体（受 `max_body_bytes` 约束），返回 `std::vector<std::byte>`（**字节**），无体 ⇒ 空 |
| `co_await req.read_some(buf)` | 增量读一段（大文件 / SSE），`buf` 是 `std::span<std::byte>`；返回 0 = 体结束 |
| 响应形参（**必需**） | 声明本条路由产出哪种响应，见下面 §4 的表 |
| 返回类型 | 必须是 `asio::awaitable<void>` |
| `Builder::route_on(&app, &App::m...)` | 一次注册整包；**每个动词一棵**编译期定容的树（不是一棵） |

**编译期会拦住**：pattern 语法不合法 · 第一个形参不是请求类型 · 返回类型不是 `awaitable<void>` ·`param<T, "名字">()` 的名字不在 pattern 里 · 形参类型不对 · 同一动词下两条路由争抢同一路径（报错里直接出现这两条 pattern）· 写两次 `route_on`（`route_on` 之后返回的 builder 只剩 `build()`）。
> 📌 “pattern 里的名字没人读”**不算错** —— 读取发生在 handler 体内，编译期看不到。
> 📌 **一个 server 只能声明一组路由** ⇒ 跨组冲突根本不存在，不需要运行期检查。

**pattern 语法**（`Get<>` 与 `Pattern<>` 是同一套）：

| 写法 | 含义 | 约束 |
|---|---|---|
| `/` **开头** | 必须 | 否则 `must_start_with_slash` |
| 字面量段 | 逐字符精确匹配（**大小写敏感**） | 段内不能含 `{` `}` |
| `{name}` | **单段参数** | 名字非空、不含 `{` `}` `/`；**匹配非空段** |
| `{name...}` | **全捕获** | 必须是**最后一段**（否则 `catch_all_not_last`） |
| `{$}` | **精确尾** —— “恰好到此为止” | 无名字 |
| 尾斜杠 `/a/` | **匿名全捕获**（子树语义），等价 `/a/{无名...}` | |
| `/`（只有根斜杠） | 匿名全捕获（**匹配一切**，但不匹配空路径） | |
| `//` | 空段 | 除非在末尾 → `empty_segment` |
| 同一路由内重名 | 禁止 | `duplicate_param` |

⚠️ **不支持段内前后缀** —— 一段要么**纯字面量**、要么**整段参数**；`img-{id}.png` 这种写不出来。原因：段内参数会让“后缀不同”证明不了两条路由不相交（`img-a.png.jpg` 会**同时**命中 `img-{id}.png` 与 `img-{name}.jpg`）⇒ 歧义无法在编译期消解。

`ParseError` 取值：`must_start_with_slash` · `empty_segment` · `orphan_brace` · `empty_param_name` · `bad_param_name` · `catch_all_not_last` · `duplicate_param`。
`Pattern<S>::ok()` 为假时 `count` / `kind` / `text` / `params()` 无意义。

**匹配语义**：命中 = 走到一个挂了槽位的节点，**并且路径被恰好走完**（不是前缀命中）⇒ 想让 `/files/` 吃下整个子树，必须显式写 `{name...}` 或尾斜杠。**空路径不匹配任何 pattern**。

| 边 | 吃多少 | 细节 |
|---|---|---|
| `chars` | 一个字符片段（可横跨多个段，含中间的 `/`） | 逐字符比较，大小写敏感 |
| `param` | `/` + **非空**段 | `/users/` 不匹配 `/users/{id}` |
| `catch_all` | `/` + 剩余**全部**（可为空串） | **至少要有一个 `/`** ⇒ `/files` 不匹配 `/files/{p...}`，`/files/` 匹配（`p == ""`） |
| `end_mark` | `/` + **立刻到尾** | `/a/{$}` 匹配 `/a/`，不匹配 `/a` 也不匹配 `/a/b` |

**优先级与回溯**：同一节点上的出边按 rank 排序（同 rank 保注册顺序）—— `chars(0) → param(1) → catch_all(2) → end_mark(3)`；匹配时依次试、失败就**回溯**。
典型：路由 `/files/static` + `/files/{path...}`，路径 `/files/static/x` ⇒ 先试 `chars "/static"`（吃到但还剩 `/x`、无出口）→ 回溯改试 `catch_all` ⇒ `path="static/x"`。
所以“静态优先”是**排序偏好**，不是“字面量一票否决”。

**冲突规则**（编译期报错）：**分歧点上必须有字面量**。

| 情形 | 判定 | 例 |
|---|---|---|
| 公共前缀同形**且等长** | ❌ 报错 | `/a/b` + `/a/b`；`/a/{x}` + `/a/{y}`；`/files/` + `/files/{p...}` |
| 分歧点是**字面量**（至少一侧） | ✅ 放行 | `/a/{x}/c` + `/a/b/{y}` |
| 分歧点是**两个通配符** | ❌ 报错 | `/a/{x}` + `/a/{y...}`；`/a/{y...}` + `/a/{$}` |
| 两侧段**根本对不上** | ✅ 放行 | `/a` + `/b`；`/a/{x}` + `/a/{$}` |

冲突只在**同一 method 内**判定 —— `Get<"/a">` 与 `Post<"/a">` 不算冲突（各走各的树）。报错形态是**未定义模板的实例化**（`ShadowedRoute<"…", "…">`），编译错误里直接出现这两条 pattern。

**真值表**（全部由 `static_assert` 钉住。主集合 = `/health` · `/api/v1/echo` · `/api/v1/users/{id}` · `/files/{path...}` · `/a/{$}` · `/api/v1/user` · `/files/static`）：

| 路径 | 结果 |
|---|---|
| `/health` | ✅ `/health` |
| `/health/` | ❌ 404 |
| `/api/v1/echo` · `/api/v1/user` | ✅ 各命中自己 |
| `/api/v1/users/42` | ✅ `{id}`，`id="42"` |
| `/api/v1/users` · `/api/v1/users/` | ❌（参数段必须存在 · 空段不算捕获） |
| `/api/v1/users/42/posts` | ❌ 多了一段 |
| `/files/a/b` | ✅ `{path...}`，`path="a/b"` |
| `/files/` | ✅ `{path...}`，`path=""` |
| `/files` | ❌ catch_all 至少要有一段 |
| `/files/static` | ✅ 字面量优先于 catch_all |
| `/files/other` | ✅ `{path...}`，`path="other"` |
| `/files/static/x` | ✅ `{path...}`，`path="static/x"`（**回溯**） |
| `/a/` | ✅ `/a/{$}` |
| `/a` | ❌ `{$}` 要求那个尾斜杠 |
| `/nope` | ❌ 404 |

单独验的两组（`/` 会吞掉所有 404 用例）：`/` · `/a/b` 对 `["/"]` ⇒ 都命中根全捕获；**空路径**对 `["/"]` ⇒ ❌；`/` 对 `["/{$}", "/a/{$}"]` ⇒ `/{$}`；`/a/` ⇒ `/a/{$}`；`/a` · `/a/b` ⇒ ❌。

**编译期错误消息**（排错时对着找）：

| 情形 | 怎么报 |
|---|---|
| pattern 语法不合法 | 「路由声明：路径 pattern 语法不合法…」 |
| 第一个形参不是请求类型 | 「路由声明：第一个形参必须是 `Get<"…">` / `Post<"…">` 这样的请求类型…」 |
| 返回类型不是 `asio::awaitable<void>` | 「路由声明：handler 必须返回 `asio::awaitable<void>`…」 |
| `param<T,"名字">()` 的名字不在 pattern 里 | 「路由声明：路径里没有这个名字的命名捕获」 |
| 形参类型不对 | 「路由注入：形参只能是 `Get<…>`（第 0 个）/ 响应类型（`route::StringResponse&` 等）」 |
| 没写响应形参 | 「路由 handler 必须有一个响应形参：route::StringResponse& / route::EmptyResponse& / route::UnknownLengthResponse&」 |
| 同动词下两条路由争抢同一路径 | 编译错误里直接出现这两条 pattern（`ShadowedRoute<"…", "…">`） |
| 写了两次 `route_on` | `no member named 'route_on'` |

**参数提取**：`Pattern<S>::params()` 按出现顺序给命名捕获（`/users/{id}/posts/{pid}` ⇒ `{"id","pid"}`；匿名占位符不计入）；这个顺序就是**捕获顺序**。

### 4. 请求与响应

**读体**（体是**字节**，文本要显式转）：`co_await req.body()` 拿 `std::vector<std::byte>`；`co_await req.read_some(span)` 增量读；
也可以把 `req` 直接喂给 asio 组合操作 —— `asio::async_read(req, …)` / `async_read_until(req, …)`（它满足 `AsyncReadStream`，读侧“体到尾”在门面处就是 asio 的 `eof`）。
文本视图：`http::as_text(bytes)`；反方向 `std::as_bytes(std::span{sv})`。handler 写文本体不必转 —— `res.body("ok\n")` 走 `string_view` 重载。
⚠️ **没读完的体不会被 drain** ⇒ 响应带 `Connection: close` 并关连接（所以“忽略体的 POST”会自动变成一次性连接；读了就没事）。

**响应类型**（声明哪一种就用哪一种，框架按形参类型构造）：

| 类型 | 体在哪 | 定界 | 典型用途 |
|---|---|---|---|
| `route::StringResponse` | 内存里的字节（`body(块)` 追加） | `Content-Length` | 绝大多数接口 |
| `route::EmptyResponse` | 无（**没有** `body()`） | 204 / 304 / 1xx：**不发** `Content-Length`；其它状态码发 `Content-Length: 0` | OPTIONS 204、304 |
| `route::UnknownLengthResponse` | handler 边跑边推（`body(块)` + `flush()`） | **响应侧 chunked** | SSE / 大响应流 |

公共部分：`status(int)`（默认 200；`EmptyResponse` 默认 204）· `content_type(名字)` · `header(名字, 值)` · `trailer(名字)`（预告）。
它们**只改状态**（链式、返回具体类型，可混着链）；**出网只有一个动作** —— `co_await res.flush()`（返回 `std::error_code`，空 = 成功；否则 `http::WriteErrc::closed` / `timed_out`）。

```cpp
co_await res.status(201).content_type("application/json").header("X-Trace", "abc").body(R"({"ok":true})").flush();
```

| 动作 | 含义 |
|---|---|
| `body(块)` | **只往内存追加**，不碰 socket |
| `co_await res.flush()` | 把此刻攒着的发出去：头没发就先发头，然后发体（推式就是发一个块）。幂等 |
| `co_await res.finish(trailers)` | 收尾：flush + 末块 + trailer。handler 不调 ⇒ **框架在它返回后兜底调** |

**推式（`UnknownLengthResponse`）**：`flush()` 推一块，**什么时候推由 handler 定**（这是它存在的理由）；`finish(trailers)` 发终止块 + trailer。
⚠️ **顺序错会抛 `std::logic_error`**：头一出去，`status` / `content_type` / `header` / `trailer` 就不能再改；定长响应 flush 之后也不能再 `body()`。
⚠️ **`co_await res` 这种写法不存在** —— asio 的 promise 有 `await_transform`，它**优先于** `operator co_await`，所以出网只能 `co_await res.flush()` / `finish()`。
⚠️ **trailer 只在 chunked 下存在**，且字段必须是其定义允许出现在 trailer 的（`ETag` / `Accept-Ranges` / `Authentication-Info`）；中间设备往往直接丢掉，别放必需信息（RFC 9110 §6.5.1）。
⚠️ **HEAD**：头照发（`Content-Length` 按 GET 的体算），正文一个字节也不发。

### 4.1 协议层应答（405 / OPTIONS / HEAD 由框架答，不用写 handler）

| 请求 | 框架怎么答 |
|---|---|
| 路径在、动词不符 | `405` + `Allow: <该路径已注册的动词>` |
| `OPTIONS`，没写 `Options<>` | `204` + `Allow`（无体、不发 `Content-Type`） |
| `HEAD`，没写 `Head<>` | 复用同路径的 **GET** handler：跑它、**不发正文**、`Content-Length` 按 GET 的体算 |
| `HEAD`，写了 `Head<>` | 显式那条优先（同样不发正文） |
| 路径不存在 | `404` |

`Allow` 的顺序 = 注册顺序（`GET → POST → PUT → DELETE → HEAD → OPTIONS → PATCH` 中出现过的那些）；**路径上有 `GET` 却没显式 `Head<>` 时补一个 `HEAD`**，紧跟 GET 之后（RFC 9110 §9.3.2 把 HEAD 当 GET 的隐含搭档）⇒ 只写一条 `Get<"/health">`，`Allow` 就是 `GET, HEAD`。
“不发正文”由**响应类型**按 `ResponseWriter::head_only()` 抑制 ⇒ **显式 `Head<>` 的 handler 就算填了 `body` 也不会发出去**（但长度会出现在 `Content-Length` 里）。

### 5. 观测挂点（默认开，可关）

- **钩子**：`route::Observer`（`on_request_begin` / `on_request_end`）接在 `Router::dispatch` 的进出 ⇒
  **覆盖全部请求**，包括 404 / 405 / OPTIONS 这些没进 handler 的。
  🔍 当前只有 `Router::set_observer()`，`Builder` 还没暴露这个入口（MU 前补）。
- **类别指纹**：`Get<"/x">::fingerprint()` —— `method + pattern` 的稳定 hash；给指标分维度、也是“指纹 → 参数”表的键。
- **开关**：`-DPYLAI_ENABLE_OBSERVATION=OFF` 关掉整个观测槽（不占字节、不发分支）。
- 观测什么、在哪里观测、成本怎么算 → `docs/http-server-plan.md` §4.1.4。

## 当前状态（🟡 Stage 1：框架形态定形，进行中）

已有：llhttp 解析（pipeline / 半包 / 400 / 431）、keep-alive、空闲与写超时、
每连接独立 `cancellation_signal`、优雅停机（`SIGINT`；`SIGTERM` 仅 POSIX）、
后端随平台（Linux io_uring / Windows IOCP —— 见 `docs/cross-platform.md`）、
**声明式路由**（DSL 全表见上文「怎么用」§3）。

刻意**尚无**（按计划分层顺序逐步加）：

| 缺什么 | 计划位置 |
|---|---|
| 有界队列 / 在途上限 / 准入控制 | **Stage 2**（`docs/http-server-plan.md` §9） |
| 指标 · 自适应 | L3 / L4（同上） |
| TLS | “动态池”那一阶段（`docs/http-server-plan.md` §10.4） |

## 构建与运行

命令见根 `README.md`「快速开始」；本目录的产物是 `build-<preset>/server/<Config>/libpylai.a`。

## 想深入

clice 查询、调试配置、写死在源码顶部的常量、与 `standalone/` 的边界、编译期用例的位置 → `docs/maintainers.md`。
