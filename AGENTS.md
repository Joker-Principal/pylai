# AGENTS.md —— 本仓库怎么读 · 工作纪律

> 适用于所有在此仓库工作的 AI agent。
> **本文件是"规矩"（相对稳定）；`docs/` 里是"笔记"（会变，且明确标注了状态）。**
> 本文件是它在本仓库的"延伸"：只写本仓库特有、且经过验证的规矩，不重复全局内容。

---

## 0. 这个仓库怎么读（先看这里）

三类读者，入口不同：

| 你是 | 从哪开始 |
|---|---|
| **只想用它**（写 app / 接下游） | `server/README.md` 的「怎么用（使用手册）」—— 接入 · 最小可跑 · 路由 DSL 全表 · 请求与响应 · 观测挂点 |
| **读代码 / 改框架** | 本文（纪律与工具）→ `docs/http-server-plan.md`（现状为什么这么设计）→ `docs/changelog.md`（怎么走到现状的） |
| **只看接口边界** | `docs/modules.md`（模块与分区依赖）→ `docs/coding-discipline.md` §8 / §9（导出面 · 成员与段） |

⚠️ `docs/` 里的文档**分工不同**，别把“账本”当“说明书”：现状设计看 `http-server-plan.md` / `modules.md`；
历史论证看 `changelog.md`；写代码的规矩看 `coding-discipline.md`。

---

## 1. 目录职责：先读该目录的 README

**每个目录的职责写在它自己的 `README.md` 里——工作前先读对应目录的 README。**
这样你拿到的是“**该放什么**”的正向指引，而不只是“不该放什么”。

**目录地图（有哪些目录、各自一句话职责）以根 `README.md` 的「目录地图」为准** —— 本文不再抄一遍，免得两处各改各的。
工作区**之外**的目录（vcpkg 根、构建产物、扩展目录等）→ `docs/vcpkg.md` 及 `docs/` 下同类文档。

**新增或移动目录时：先写它的 `README.md`，再写代码**（并在根 `README.md` 的地图里补一行）。

---

## 2. 证据纪律

- 结论必须**可复核**：给**符号名**（`文件` + 函数 / 类型名），或给一段能直接复制执行的验证命令。
  ⚠️ **不要写行号** —— 我们的重构常往文件前部插代码，行号必然漂（2026-10-14 定；
  例外：`docs/changelog.md` 里作为**历史记录**的行号，那里记的就该是当时的样貌，不要回头去改）。
  指向**上游库**（asio / llhttp）的行号不受影响，可以保留 —— 它们绑的是那个版本的源码。
- 文档里标状态，统一用这套标记（全局偏好那条原则在本仓库的具体化）：
  **✅ 已验证（代码/实验）** · **🟡 倾向（待评估）** · **🔍 待调研 / ❓ 未定**。
  ⚠️ 状态要跟**事实**走，不要跟**情绪**走：已经实现/落地的写 ✅ —— 若语义仍可能被推翻，就写
  “✅ 已落地（语义可推翻）”，别用 🟡 含糊带过（🟡 是给"还没做"的）。

---

## 3. 工具纪律

### clice —— C++ 代码的**默认入口**（先 clice，再考虑 grep）

**只要问题在问“符号”，就先 clice**：定义 / 引用 / 调用图 / 类型层次 / 影响面 / 跨文件重构。
逐个 `grep` + `read_file` 自己去拼是**下策**（慢、且会漏掉签名与重载）。

**三条硬规矩**

1. **有可能用 clice，就一定用** —— 不是“可以考虑”，是“只要存在用法就不许绕过”。
2. **用不顺手（报错 / 接口对不上 / 索引不灵）⇒ 立刻讲**，由用户介入；**用成了 ⇒ 记进本文件**（一行、可复现就够）。
3. **做不到的一律不记** —— 本文件只留 clice 能做到的用法，别让做不到的条目稀释上面这条准则。

| 要问的 | 走哪里 |
|---|---|
| 某符号在哪定义 / 被谁用 / 改它会影响谁（含跨文件、模板实例） | **clice**（或 VS Code 的 `vscode_listCodeUsages` / `vscode_renameSymbol`） |
| 改名、重构 | **clice**（先 `--dry-run`）或 `vscode_renameSymbol` |
| 纯文本 —— 注释、错误消息、配置、`CMakeLists`、文档里写的名字 | grep（clice 看不到这些） |
| clice 报错 / 索引坏掉时的傅底 | grep + 编译 |

- 入口 `tools/clice`；**本地 `--help` 是接口真相**。照 SKILL.md 的配方用；**只有**发现配方与本机 `--help` 不一致（如 unknown option）
  ⇒ 才按本机版本读仓库 `docs/` 拿准新用法并**回填 SKILL.md**（本机 clice 几乎每天更新，做法见该文件）。
- ⚠️ 每次 `cmake --preset` 都会重写 compile_commands.json → 查询加 `--fresh` 或先 `tools/clice index`；
  没索引且没 `--fresh` 直接报错；`stale` 字段务必看。
- 内部链接符号（static / 匿名命名空间）查不到名字 → 用答案里的 `#<hex>` + `--path`。
- `refactor rename` 会改文件：先 `--dry-run`。其余细节见 `.github/skills/clice-cli/SKILL.md`。
- **验证通道用 `query`，不用 `lint`**：`tools/clice query --method documentSymbols --path <f> --fresh`
  （`--fresh` 会先把改动过的文件重索引 = 重编译）；**新符号出现在索引里 ⇒ 该 TU 编译过**。
  `lint` 是 clang-tidy 那套命名检查，慢且跑偏，只在要查命名时用。
- **改名 / 影响面复核**（已验证）：`query --method symbolSearch --symbol '<新名>' --fresh` 拿 `#<hex>`，
  再 `query --method references --symbol '#<hex>' --include-declaration` 列全部引用点。
  判据三者齐了才叫改完：**新名引用点齐全 + 旧的类型名零引用 + `stale` 为空**。
- ⚠️ 索引坏了、或写锁被编辑器里的 `clice serve` 占着：CLI 拿不到写锁（**重载窗口没用**，只会换个 server 继续占）。
  处置 = 让 server 松手（禁用 clice 扩展）+ 删 `.clice/cache` 重建。

### 注意利用 VS Code 的命令

- VS Code 及其安装的扩展会注册很多命令，可通过命令 ID 调用，会比终端或读写文件等更快。
- 命令不返回可读输出，所以**不要指望"用命令替代终端"**：需要看输出时，默认走终端 / 任务 / 文件 / 诊断。
- 破坏性命令不要随手调，例如：
  `cmake.resetState`（会重载窗口）、`cmake.clean` / `cmake.fullCleanConfigure`（会删构建目录）、
  `cmake.*All`（会影响所有工作区）——这类必须由用户确认后才执行。

### 构建

- `cmake --preset dev_llvm` → `cmake --build --preset debug_llvm`
- 端到端验证：`python tests/run.py`（跨平台，**唯一入口**；不再有 bash 脚本）。
- **平台差异只在 CMake / `vcpkg.json` 里分叉**（`if(CMAKE_SYSTEM_NAME …)`、manifest 的 `"platform"`），
  实现里不写 `#ifdef _WIN32`。清单与 Windows 构建步骤见 `docs/cross-platform.md`。
- vcpkg 根：`<vcpkg 根>`；工具链配置在 `CMakeUserPresets.json`（gitignored）

---

## 4. 做性能 / 参数相关的工作时

- 在 `.vscode/tmp` 中建立文件夹开展；**构建日志 / 临时文件也一律写这里，不要写 `/tmp`**
- **指标与实验规则不在本文件**：观测什么、在哪里观测、怎么归因、观测成本怎么算 —— 见 `http-server-plan.md` §4.1.4 与 §6。

---

## 5. 代码注释纪律

**只看 `docs/coding-discipline.md` §1**（那里有判据表与正反例）—— 一句话版：**接口文档留，实现注释不写，设计取舍进 docs**。

---

## 6. 信息 / 纪律 / 记忆的分工

- **信息与历史变更 → `docs/`**（每条标状态，**要短**）。`docs/changelog.md` 记"改了什么 + 为什么 + 怎么复核"。
- **工作纪律（规矩）→ 本文件**。
- **会话记忆 → 只留指针**："先读 `AGENTS.md` + `docs/`"，**不复制** docs 的内容。
  ⚠️ 写记忆时 `str_replace` 的锚点必须**唯一**：我曾反复锚同一行，把它后半段复制成几百行。

---

## 7. 提交纪律（git）

- **一次提交一个主题。** 先 `git status` + `git diff` 看全量，把改动按主题归类再动手；
  跨主题的改动不塞同一个提交。⚠️ **别 `git add -A` 一把梭** —— 那正是把"路由设计"和"改构建标准"混在一起的成因。
- **message 必须经用户核验后才提交。** 用选项卡把**每一条** message 先给用户看，**一轮一条**；
  用户通过才 `git commit`，不要连着自作主张提几个。
- **格式**：`type(scope): 中文描述`（沿用历史：`feat(server)` / `refactor(server)` / `docs` / `build` / `chore`）。
  **正文不超过 5 行**，只讲"改了什么 + 为什么"；细节留给 `docs/changelog.md`。
- **提交前跑一遍验证**（§2 证据纪律的落地）：`cmake --build --preset debug_llvm` + `python tests/run.py`，
  退出码写进当轮汇报。构建或测试不过就不提交。
- **拆分粒度以"能独立成立"为准**，不为凑主题制造编不过的中间态；跨文件强耦合的改动（改结构 + 同步改调用方 + 同步改文档）算**一个**主题。
- 生成物与本机配置都不进版本库：`.clice/` · `build-*/` · `.vscode/` · `CMakeUserPresets.json` · `clice.toml`（已在 `.gitignore`）。
