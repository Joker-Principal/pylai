# 变更史（按时间倒序）

> **只记"改了什么 + 为什么 + 怎么复核"**，不复述细节 —— 细节在对应专题文档里。
> 更早的历史看 `git log`。写作约定见 `README.md`（结论要给 `文件:行` 或可执行命令，标状态）。

## 2026-10-13

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
  删掉已经不存在的 `_route` UDL 行。
  复核：`cmake --build --preset debug_llvm`；`grep -rn 'pylai:router\|""_route' docs/` 只剩历史条目。

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
