# 变更史（按时间倒序）

> **只记"改了什么 + 为什么 + 怎么复核"**，不复述细节 —— 细节在对应专题文档里。
> 更早的历史看 `git log`。写作约定见 `README.md`（结论要给 `文件:行` 或可执行命令，标状态）。

## 2026-10-13

- **协议层应答：405 / OPTIONS 带 `Allow`，HEAD 复用 GET** ✅ —— 见 `http-server-plan.md` §11.2 G。
  `dispatch`（`server/route.cpp`）里“逐棵动词树试一遍”的结果现在被留下来算 `Allow`；HEAD 没有显式 `Head<>` 时落到同路径的 GET。
  正文抑制收口在 `http::serialize(response, keep_alive, head_only)`（`session.cpp` 传 `req.method == "HEAD"`）→ 显式 `Head<>` 也不会漏发体。
  ⚠️ 用户决定：`Allow` 里 **GET 存在时带上 HEAD**（RFC 9110 §9.3.2），紧跟 GET。
  复核：`./tests/run.sh`（17/17，含 `Allow: GET, HEAD, POST`、`HEAD /both` 的 `Content-Length: 14` 证明显式优先）。

- **统一语言标准：`set(CMAKE_CXX_EXTENSIONS OFF)`（`-std=gnu++23` → `-std=c++23`）** ✅ —— clice 索引退化的根因。
  起因：clice 对 `server/route.cpp` 里一部分符号没反应。日志（`.clice/logs/<会话>/master.log`）：
  `BuildPCM failed for module pylai:route: GNU extensions was disabled in precompiled file …/pcm/jk-….pcm but is currently enabled`
  → clice 缓存的 jk PCM 是 `-std=c++23`（jk 的 `CMakeLists.txt:70` 是 `CXX_EXTENSIONS OFF`），我们的 TU 是 CMake 默认的 `-std=gnu++23`
  → clang 拒绝加载 → `JK` 全未声明 → `route.cppm`（今天起 `import jk;`）编不过，连带 `route.cpp` 编不过 → 无 AST → 索引只剩扫描级浅结果。
  复核：重建后新会话 `BuildPCM failed` / `GNU extensions` 均 **0** 条；`route.cpp` 符号 19 → **50**，`Router::dispatch` 恢复 `HasDefinition` + `route.cpp: definitions=1`；`session.cpp`（早就 `import jk;`）同样 19 → 146。
  细节与征兆见 `modules.md`（跨 target PCM 选项必须一致）。

- **本机 clice 配置：`readonly` 必须保持 `off`** ✅ —— 起因：加了它之后编辑器里导航/补全变差。定位：`[project] readonly` 管的**不是**索引写锁，而是
  **给打开的文件建不建完整 AST**；`auto` = 文件一律先只由后台索引作答。证据（`.clice/logs/*/master.log`）：`off` 的会话有
  `pch.cpp:300 PCH built`，`auto` 的会话 **PCH 活动 0 行**，且编辑器的 definition/references 全由 `[perf:index_query]` 直答 —— 而索引会漏符号
  （`make_routes`）也留旧符号（`ParamSlot`）。→ 改回 `readonly = "off"`。
  同时修**索引空转**：`.vscode/tmp/**` 探针不在任何 DB 里却被 tracker 拉进索引队列，每会话刷几十条“无法解析编译命令”（30 → 32 → 46 条，29 条 owed）
  → 加规则 `patterns = [".vscode/tmp/**"]` + `index/lint/format = false`。
  复核：重启服务后新会话真实失败数 **0**、调度 `total=20`（原 41~49）、`PCH built` 恢复、`Loaded CDB … with 63 entries` 仍在（规则未关掉数据库自动发现）。

- **`pmf` 类型萃取改用 jk 的 `Callable`；形参表换 `TList`** ✅ —— 用户提示 jk 有 callable 工具。
  `RouteOf` 不再手写 `RouteOf<R (C::*)(Ps...)>` 偏特化（只匹配裸形式），改用 `JK::Meta::Return` / `Arguments`；
  `make_handler` 也不再分解 `R / C / Ps`。`Params` 由 `std::tuple` 换 `TList` → `ParamSlot` 递归 + `SlotOf` 偏特化（20 行）
  合并成 5 行的 `SlotOf<i, L> = ParamCount<L::Slice<0, i>>`；`tuple_element_t` / `tuple_size_v` → `At<i>` / `size()`。结构上净删约 16 行
  （文件总行数 1392 → 1391：删了 16 行，又加了 16 行钉住 cv/noexcept 新能力的用例）。
  ⚠️ 新增依赖面：`route.cppm` 现在 `import jk;`（此前只有 `session.cpp:21` 用它），`RouteOf<Pmf>::Params` 对外是 `JK::Meta::TList`。
  复核：构建绿 + `./tests/run.sh`（9/9）+ `static_assert` 101 → 104；反证旧写法匹配不上 `const` / `noexcept` 见 `.vscode/tmp/jk-callable-probe/old-trait.cpp`。

- **`RouteGroup` 包装层删除：`Router` 直接持有路由表** ✅ —— 用户追问“只有一个，为什么不直接用 `methods`”。
  `RouteGroup` 原本只是 `{ std::vector<VerbRoutes> methods; }`，而 `groups` 用 vector 是**不完整类型的逃生舱**（`optional`/按值都不行），不是“想装多个”。
  现改成 `Router::methods` 一个成员 + `VerbRoutes` 前置声明；`set_group`/`make_group` → `set_routes`/`make_routes`（不再有包装类型）。
  `dispatch` 去掉两层 `for` 中的外层（`server/route.cpp:39,48`）。
  复核：构建绿 + `./tests/run.sh`（9/9）+ `clice query --method documentSymbols` 里 `RouteGroup` 已消失。
  ⚠️ 工具发现：`clice` 的 `symbolSearch` **两个方向都会骗人** ——
  ① **漏**：`make_routes` 在索引里 `kind=Invalid` → 搜不到（`clice index --show-symbol make_routes` 能看到它）；
  ② **残留**：`ParamSlot` 已从源码删掉，符号表里却还留着（`--show-symbol` 仍列出，但那行**没有 `文件:定义数`**部分是残留特征）。
  记：**存在性判据一律落回源码 grep + 编译**。

- **`Pattern` 的数组维度改紧（不再按 raw 长度过分配）** ✅ —— 用户指出：**段数就是 `count('/')`**。
  拆掉原来兼三用的 `cap`：`segment_count = count('/')`（段，合法 pattern 精确）、`name_count = count('{')`（命名捕获上界）给数组，
  `node_budget = raw.size() + 1` 留给 `k_node_bound`（**值不变 ⇒ 建树行为一字不动**）。
  复核：`route.cppm` 新增 5 条 `static_assert` 钉住维度 + `./tests/run.sh`（9/9）。⚠️ 顺带修正：我先前写"节点上界靠 label 非空"是**错的** —— `end_mark`/匿名 `catch_all` 的 label 是空的。

- **段内前后缀：重新评估后决定不做** ❌ —— 曾被要求加上（`img-{id}.png`），语义也定好了（一段内最多一个参数、非贪婪失败再放宽、
  `{x...}`/`{$}` 不进段内），但推冲突规则时撞上**无法静态消解**的歧义：`img-{id}.png` 与 `img-{name}.jpg` 会同时命中 `img-a.png.jpg`。
  根因：段级安全靠的是"**段文本不同 ⇒ 不相交**"这条对齐性质，段内参数把它打破了（参数可跨过后缀）。细节 → `http-server-plan.md` §11.2 A。
  顺带记下两条讨论结论：`Pattern`/`PieceCursor`/`insert_pattern` 是**三层级划分**（输入不同、不是重复实现）· **「先算后建」是权宜**（源自
  `std::vector` 不能进 `constexpr` 变量）而非规范做法 —— 见 §11.2 C。

- **`server/` 降为框架库，示例应用搬到 `tests/`** ✅ —— `server/` 的 target 由可执行 `pylai` 改为**静态库 `pylai`**
  （模块文件集 PUBLIC；asio-module/joker::module 提为 PUBLIC，供消费方 `import pylai;`）；
  `server/main.cpp` 删除，示例应用 `App` 搬到 `tests/main.cpp`，可执行 target 名 **`app`**（`build-dev_llvm/tests/Debug/app`）。
  E2E 脚本与入口一并收进 `tests/`：`http_e2e.py` + `run.sh`（查端口→起服务器→断言→收工）。
  复核：`cmake --build --preset debug_llvm` → `./tests/run.sh`（7/7，退出码 0）。

- **文档整理 + `main.cpp` 去掉多余的 `import asio`** ✅ ——
  `asio-notes.md`：删掉只剩一句的「§6 工作纪律」桩（那句话提到文件头）、把「误判期的原始记录」挪到 cancellation 节下作附录、
  §7/§8 顺次重编为 §6/§7（内部与 `http-server-plan.md` 的 `§` 引用同步）、修 `pylai:router`/`router.cpp` →
  `pylai:server`/`server.cpp`（`plf::hive` 现在住在 `Server` 里）。
  `routing.md`：补 §1「怎么声明路由」（`Get<>` / `Param<>` / `route_on`），`Pattern<>`/`Table<…>` 降为 §1.1 并明说**不参与注册**，
  删掉已经不存在的 `_route` UDL 行。🔄 **同日稍后修订**：`Table` / `On` 整体删除，§1.1 已改写为「编译期会拦下什么」。
  复核：`cmake --build --preset debug_llvm`；`grep -rn 'pylai:router\|""_route' docs/` 只剩历史条目。

- **路由：每 method 一棵树 + 冲突检查接线 + 单次 `route_on`** ✅ ——
  ① 实测出「同一棵树里 `Get<"/x">` 与 `Post<"/x">` 会互相覆盖槽位」（`RadixNode::route` 只有一个）→ 改为**按动词分树**
  （`SelectPatterns` 筛 pattern、每个动词一棵 `build_of()`，各树擦成闭包放进 `VerbRoutes`）；
  ② `validate_group` 接进 `route_on`：冲突时实例化未定义的 `ShadowedRoute<两条 pattern>`，**编译错误里直接点名**；
  ③ `route_on` 返回只有 `build()` 的 `Configured` → **第二次 `route_on` 编不过**（跨组冲突因此不存在）；
  ④ 删掉 `Table` / `On` / `table_selftest`。复核：`./tests/run.sh`（9/9，含 `GET /items` 与 `POST /items`）；细节 → `http-server-plan.md` §11.2 B/C。

- **移除旧字符串路由接口** ✅ —— 删掉 `Router::add/get/post/put` 与精确表（`dispatch` 的 ①③ 两步）、
  `Builder::route/get/post/put`、以及只为旧 API 存在的 `operator""_route` UDL；注册**只剩** `Builder::route_on`，
  `dispatch` 简化为「组匹配 → 动词核对 → 405/404」。理由：两套注册方式 = 两套语义（精确表是"最后一次覆盖"，
  声明式是"编译期定容分组"），留一套可消除歧义。
  复核：`cmake --build --preset debug_llvm` → `./tests/run.sh`（7/7；`/echo` 现为 404）。

- **位置注入 DSL 落地到 `Router` / `Server`，端到端跑通** ✅ —— 修掉注入链最后一环的类型断层
  （`make_arg` 的捕获形参由 `span<const std::string_view>` 改为 `span<const Capture>` 并取 `.value`；
  `make_handler` 接住 `match` 的 `[[nodiscard]]` 槽位）；`server/main.cpp` 改用 `route_on`（当时还临时留着
  一条精确表路由做对照，**已随上条移除删掉**）。
  复核：`cmake --build --preset debug_llvm` → `./tests/run.sh`（7/7 通过）。细节 → `http-server-plan.md` §11.2 C。

- **命名规则定稿并落地** ✅ —— `.clang-tidy`：类型名 CamelCase；函数 / 方法 / 变量 / 参数 / 成员 / 常量 lower_case；
  **模板参数分四类**（通用 / 类型 / 模板模板 → CamelCase，非类型 → lower_case）。
  ⚠️ 专用键缺失会**回退**到通用键 → 四行都要写；类型参数的键名是 `TypeTemplateParameterCase`
  （写成 `TemplateTypeParameterCase` **静默无效**）。复核：`tools/clice lint --log-level warn` → 全工作区 0 findings。
- **路由声明 DSL 落地**（"位置注入"风格）✅ —— 动词 + 路径放在**返回类型位置**（`Get<"/health">`），
  `Param<T[, name]>` 按**位置**吃第 N 个命名捕获，`Builder::route_on(this, &C::m...)` 一次注册整包；
  返回类型靠 `std::coroutine_traits` 特化借用 asio 的 frame 成为合法协程返回类型。
  细节与全部证据 → `http-server-plan.md` §11.2 C。
- **`Router` 模式感知** ✅ —— `dispatch` = ① 精确表（旧字符串 API）→ ② 声明式组（每组一棵**编译期定容**树 + 槽位表，
  命中后核对动词）→ ③ 路径在但动词不符 → 405。
  🔄 **修订**：①②③ 已收敛为「组匹配 → 动词核对 → 405/404」，①③ 随旧接口移除（见本日首条）。
- **验证通道改走 `clice query`** ✅ —— `query --fresh` = 先重索引（= 重编译）再回答；`lint` 只用于命名检查。
  索引写锁会被编辑器里的 `clice serve` 常驻占用 → CLI 重建不了，需先让 server 松手。
- **会话记忆瘦身** ✅ —— 信息与历史搬进 `docs/`（asio 相关内容并入 `asio-notes.md` / `modules.md`），
  纪律进 `AGENTS.md`，记忆只留指针。
