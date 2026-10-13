# 路由手册

> 写 handler、要指定路由时查这份。
>
> - **怎么声明路由** → §1：`Get<"…">` 写在**第一个形参**上（它既是声明、也是请求），handler 返回 `asio::awaitable<void>`，响应按引用填。
> - **pattern 的语法与匹配语义** → §2–§6：`Get<>` 尖括号里写的、`Pattern<>` 里写的，是**同一套** pattern。
> - 所有规则在**编译期**生效：pattern 写错、路由之间冲突，都**编不过**。

---

## 1. 怎么声明路由

动词与路径写在**第一个形参**上（`Get<P>` 既是路由声明、也是**这条路由的请求类型**）；
handler 返回 `asio::awaitable<void>`，响应按**引用**传进来由它填；整包一次注册：

```cpp
using namespace pylai::route;

struct App
{
	asio::awaitable<void> health(Get<"/health"> req, pylai::http::Response& res);
	asio::awaitable<void> user(Get<"/users/{id}"> req, pylai::http::Response& res);
	asio::awaitable<void> submit(Post<"/submit/{id}"> req, pylai::http::Response& res);
};

asio::awaitable<void> App::user(Get<"/users/{id}"> req, pylai::http::Response& res)
{
	res.body = "user id=" + std::to_string(req.param<int, "id">()) + "\n";
	co_return;
}

App app;   // 必须比 server 活得久：路由里存的是 &app
auto server = pylai::Server::builder()
    .bind("127.0.0.1", 18080)
    .route_on(&app, &App::health, &App::user, &App::submit)
    .build();
```

> 📌 消费方要自己 `import asio;` —— handler 的返回类型就是 asio 的 awaitable；实例化 handler 的 TU 还要 `#include <coroutine>`（见 `modules.md`）。

| 元素 | 说明 |
|---|---|
| **第一个形参** | 必须是 `Get<P>` / `Post<P>` / …：它在编译期给出动词与 path pattern，同时**就是请求**（本次请求的 method / path / headers 在它里面，`req.raw` 是框架解析出的 `http::Request`） |
| `req.param<T, "名字">()` | 取一个命名捕获。**名字必写**，且在**编译期**对着 pattern 校对；整数走 `from_chars`，`std::string_view` 直通 |
| `pylai::http::Response&` | 框架持有的响应对象，handler 往里填（状态码 / 头 / 体）。**可以不写** —— 不写就发默认的 `200` + 空体 |
| 返回类型 | 必须是 `asio::awaitable<void>` |
| `Builder::route_on(&app, &App::m...)` | 一次注册整包；**每个动词一棵**编译期定容的树（不是一棵） |

**编译期会拦住**：pattern 语法不合法 · 第一个形参不是请求类型 · 返回类型不是 `awaitable<void>` · `param<T, "名字">()` 里的名字在 pattern 里不存在 · 形参用了别的类型。

> 📌 **“pattern 里的名字没人读”不算错** —— 读取发生在 handler 体内，编译期看不到；这与旧的 `Param<>` 位置注入相比少了一个检查（换来的是错误定位到写的那一行）。

> ⚠️ **读体接口还没落地**：现在经 `req.raw.body` 读（框架先读完整个消息），
> `req.body()` / `req.read_some()` 是下一刀（见 `http-server-plan.md` §4.1）。

> ⚠️ **已知缺口**：`param<int, …>()` 用 `std::from_chars` 转换，**失败时静默取 0**（`/users/abc` → `id == 0`）。
> 要回 400 得由 handler 自己判；该语义尚未定案（见 `http-server-plan.md` §11.2 D）。

### 1.1 编译期会拦下什么

| 情形 | 怎么报 |
|---|---|
| pattern 语法不合法 | 「路由声明：路径 pattern 语法不合法…」 |
| 第一个形参不是 `Get<…>` 这类 | 「路由声明：第一个形参必须是 `Get<"…">` / `Post<"…">` 这样的请求类型…」 |
| 返回类型不是 `asio::awaitable<void>` | 「路由声明：handler 必须返回 `asio::awaitable<void>`…」 |
| `param<T, "名字">()` 里的名字不在 pattern 里 | 「路由声明：路径里没有这个名字的命名捕获」 |
| 形参是别的类型（比如 `int`） | 「路由注入：形参只能是 `Get<…>`（第 0 个）/ `http::Response&`」 |
| 同一动词下两条路由会为同一路径争抢 | 编译错误里直接出现这两条 pattern（`ShadowedRoute<"…", "…">`） |
| 写了两次 `route_on` | `no member named 'route_on'` —— `route_on` 之后返回的 builder 只剩 `build()` |

> 📌 **一个 server 只能声明一组路由**（`route_on` 之后就没有 `route_on` 了）—— 所以跨组冲突根本不存在，不需要运行期检查。
> 需要单独检查一条 pattern 时可以用 `Pattern<"…">`（`ok()` / `err` / `params()`），但它**不参与注册**。

---

## 2. pattern 语法

| 写法 | 含义 | 约束 |
|---|---|---|
| `/` **开头** | 必须 | 否则 `must_start_with_slash` |
| 字面量段 | 逐字符精确匹配 | 段内不能含 `{` 或 `}` |
| `{name}` | **单段参数** | 名字非空、不含 `{` `}` `/`；**匹配非空段** |
| `{name...}` | **全捕获** | 必须是**最后一段**（否则 `catch_all_not_last`）；名字规则同上 |
| `{$}` | **精确尾** —— "恰好到此为止" | 无名字 |
| **尾斜杠** `/a/` | **匿名全捕获**（子树语义），等价于 `/a/{无名...}` | 见 §3 |
| `/`（只有根斜杠） | 匿名全捕获（**匹配一切**，但不匹配空路径） | |
| `//` | 空段 | 除非在末尾 → `empty_segment` |
| 同一路由内重名 | 禁止 | `duplicate_param` |

段与段之间恒为一个 `/`；`/` 是**段分隔符**，不属于任何段。

> ⚠️ **不支持段内前后缀** —— 一段要么是**纯字面量**，要么是**整段参数**；`img-{id}.png` 这种"段内混写"写不出来。
> 原因：段内参数会让"后缀不同"证明不了两条路由不相交（`img-a.png.jpg` 会**同时**命中 `img-{id}.png` 与 `img-{name}.jpg`），
> 歧义无法在编译期消解 —— 详见 `http-server-plan.md` §11.2 A。

### 2.1 花括号的两个说明

| 写法 | 含义 |
|---|---|
| 段内的 `{` `}` | **一律按通配符语法解释**，无法作为字面量字符出现 |
| `{...}` | 等价于名字为 `...` 的**普通参数**（名字只禁止 `{` `}` `/`，所以 `...` 合法） |

### 2.2 编译期错误码（`ParseError`）

| 值 | 触发例 |
|---|---|
| `must_start_with_slash` | `users/{id}` |
| `empty_segment` | `/a//b` |
| `orphan_brace` | `/a/b{c}` |
| `empty_param_name` | `/a/{}` |
| `bad_param_name` | `/a/{{b}}`（名字含 `{`） |
| `catch_all_not_last` | `/a/{x...}/b` |
| `duplicate_param` | `/a/{x}/{x}` |

`Pattern<S>::ok()` 为假时 `count` / `kind` / `text` / `params()` 的内容**无意义**（解析中途就返回了）。

---

## 3. 匹配语义

**命中条件**：走到一个"挂了槽位"的节点，**并且路径已经被恰好走完**（不是前缀命中）。
换句话说，路由是**精确**的 —— 想让 `/files/` 吃下整个子树，必须显式写 `{name...}` 或尾斜杠。

路径一律以 `/` 开头（HTTP 如此）；**空路径不匹配任何 pattern**。

四条边的规则：

| 边 | 吃多少 | 细节 |
|---|---|---|
| `chars` | 一个字符片段（可能横跨多个段，含中间的 `/`） | 逐字符比较，**大小写敏感** |
| `param` | `/` + **非空**段 | 段到下一个 `/` 或串尾；**空段不算**（`/users/` 不匹配 `/users/{id}`） |
| `catch_all` | `/` + 剩余**全部**（可为空串） | **至少要有一个 `/`**，所以 `/files` 不匹配 `/files/{p...}`，而 `/files/` 匹配（`p == ""`） |
| `end_mark` | `/` + **立刻到尾** | `/a/{$}` 匹配 `/a/`，不匹配 `/a` 也不匹配 `/a/b` |

### 3.1 真值表（全部由 `static_assert` 钉住）

主路由集 = `["/health", "/api/v1/echo", "/api/v1/users/{id}", "/files/{path...}", "/a/{$}", "/api/v1/user", "/files/static"]`

| 路径 | 结果 |
|---|---|
| `/health` | ✅ `/health` |
| `/health/` | ❌ 404 |
| `/api/v1/echo` | ✅ `/api/v1/echo` |
| `/api/v1/user` | ✅ `/api/v1/user` |
| `/api/v1/users/42` | ✅ `/api/v1/users/{id}`，`id="42"` |
| `/api/v1/users` | ❌ 参数段必须存在 |
| `/api/v1/users/` | ❌ 空段不算捕获 |
| `/api/v1/users/42/posts` | ❌ 多了一段，且无匹配的更长路由 |
| `/files/a/b` | ✅ `/files/{path...}`，`path="a/b"` |
| `/files/` | ✅ `/files/{path...}`，`path=""` |
| `/files` | ❌ catch_all 至少要有一段 |
| `/files/static` | ✅ `/files/static`（**字面量优先于 catch_all**） |
| `/files/other` | ✅ `/files/{path...}`，`path="other"` |
| `/files/static/x` | ✅ `/files/{path...}`，`path="static/x"`（**回溯**，见 §4） |
| `/a/` | ✅ `/a/{$}` |
| `/a` | ❌ `{$}` 要求那个尾斜杠 |
| `/nope` | ❌ 404 |

单独验的两条（`/` 与 `{$}` 不能进上面那个集合：`/` 会吞掉所有 404 用例，且 `/` 与 `/{$}` 本身就冲突）：

| 路径 | 集合 | 结果 |
|---|---|---|
| `/` · `/a/b` | `["/"]` | ✅ 都命中根全捕获 |
| `""`（空路径） | `["/"]` | ❌ |
| `/` | `["/{$}", "/a/{$}"]` | ✅ `/{$}` |
| `/a/` | 同上 | ✅ `/a/{$}` |
| `/a` · `/a/b` | 同上 | ❌ |

---

## 4. 优先级与回溯

同一节点上的出边按固定 rank 排序（同 rank 保持**注册顺序**）：

```
chars(0)  →  param(1)  →  catch_all(2)  →  end_mark(3)
```

匹配时**按这个顺序依次试，某条边失败就试下一条** —— 也就是**回溯**。最典型的一例：

```
路由：/files/static   +   /files/{path...}
路径：/files/static/x
→ 先试 chars 边 "/static"：吃掉了，但路径还剩 "/x"、节点也没有出口 → 失败
→ 回溯，改试 catch_all 边 → 命中 {path...}，path="static/x"
```

所以"静态优先"是**排序上的偏好**，不是"字面量一票否决"。

---

## 5. 冲突规则（编译期报错）

**规则一句话：分歧点上必须有字面量。**

| 情形 | 判定 | 例 |
|---|---|---|
| 公共前缀完全同形，**且等长** | ❌ **报错**（同一动词下同一条路径只能有一条） | `/a/b` + `/a/b`；`/a/{x}` + `/a/{y}`（同形异名）；`/files/` + `/files/{p...}` |
| 分歧点是**字面量**（至少一侧） | ✅ 放行（字面量 rank 最高，谁赢一眼看得出） | `/a/{x}/c` + `/a/b/{y}` |
| 分歧点是**两个通配符** | ❌ **报错**（没有字面量可定优先级） | `/a/{x}` + `/a/{y...}`；`/a/{y...}` + `/a/{$}` |
| 两侧的段**根本对不上** | ✅ 放行（匹配集合不相交） | `/a` + `/b`；`/a/{x}` + `/a/{$}`（param 只吃非空段，`{$}` 只吃空段） |

**method 维度**：冲突只在**同一 method 内**判定 —— `Get<"/a">` 与 `Post<"/a">` 不算冲突（各走各的树，见 §1.1）。

报错形态是一个**未定义模板的实例化**：编译错误里会直接出现这两条冲突的 pattern（`ShadowedRoute<"…", "…">`）。

## 6. 参数提取

`Pattern<S>::params()` 返回 `std::span<const std::string_view>`：**按出现顺序**的命名捕获。

| pattern | `params()` |
|---|---|
| `/health` | `{}` |
| `/users/{id}` | `{"id"}` |
| `/users/{id}/posts/{pid}` | `{"id", "pid"}` |
| `/files/{path...}` | `{"path"}` |
| `/` · `/a/` · `/a/{$}` | `{}` —— **匿名占位符不计入** |

这份顺序就是**捕获顺序**：匹配时第 i 个命名占位符写进 `captures[i]`。

## 7. 协议层应答（405 / OPTIONS / HEAD）

这三样都由框架答，**不用你写 handler**；只有想盖掉默认行为时才需要显式注册。

| 请求 | 框架怎么答 |
|---|---|
| 路径在、动词不符 | `405` + `Allow: <该路径已注册的动词>` |
| `OPTIONS`，没写 `Options<>` | `204` + `Allow`（无体、不发 `Content-Type`） |
| `HEAD`，没写 `Head<>` | 复用同路径的 **GET** handler：跑它、**不发正文**、`Content-Length` 按 GET 的体算 |
| `HEAD`，写了 `Head<>` | 显式那条优先（同样不发正文） |
| 路径不存在 | `404`（三者一致） |

`Allow` 的两条规则：

- 顺序 = 注册顺序（`GET` → `POST` → `PUT` → `DELETE` → `HEAD` → `OPTIONS` → `PATCH` 中出现过的那些）；
- **路径上有 `GET` 却没有显式 `Head<>` 时补一个 `HEAD`**，紧跟 GET 之后 —— RFC 9110 §9.3.2 把 HEAD 当作 GET 的隐含搭档。
  所以只写一条 `Get<"/health">`，`Allow` 就是 `GET, HEAD`。

> ⚠️ “不发正文”是在 `http::serialize(response, keep_alive, head_only)` 里收口的（`session.cpp` 传 `req.method == "HEAD"`）——
> 于是**显式 `Head<>` 的 handler 就算填了 `response.body` 也不会发出去**（但它的长度会出现在 `Content-Length` 里）。
