---
name: clice-cli
description: '用 clice 的持久化索引做 C++ 代码检索与重构：query（符号搜索/定义/引用/调用图/类型层次/影响面等）、index（建索引与只读检查）、refactor rename（先 --dry-run）、analyze modules（模块划分报告）。跨文件找符号、评估改动影响面时使用；接口以本机 `tools/clice <子命令> --help` 为准。'
---

# 用 clice CLI 做代码检索与重构

clice 是 LLVM/Clang 之上的 C++ 工具链，**一个二进制**：编辑器跑 `clice serve`（LSP），终端里的其它子命令驱动同一套编译数据库、缓存与持久化索引。`query` / `refactor` / `analyze` **直接从磁盘读索引**——编辑器开着也能用，关着也不需要服务端。价值：**编译级**的符号事实，比 grep 准、省 token。

## 入口与版本纪律

```
tools/clice <子命令> [选项]
```

- `tools/clice` 是仓库内启动器，每次解析插件自带的**最新** clice（目录带版本后缀，路径会变，勿写死）；`CLICE_BIN` 可覆盖。
- ⚠️ **本机 clice 几乎每天更新**，但**不必每次现查**：日常照本文件的配方用即可。
  **只有**发现配方与本机 `--help` 不一致（例如报 `unknown option`）时，才去拿该版原文并把新用法**回填本文件** ——
  网站内容就来自 [`clice-io/clice` 的 `docs/`](https://github.com/clice-io/clice/tree/main/docs)，
  把 `--version` 里的短 hash 当 ref 即可读到**这一版**（格式示例：`0.1.0+gfb0fa5d` ⇒ ref `fb0fa5d`）：

  ```bash
  # 中英双语都在：docs/zh/ · docs/en/；专题目录：cli/ guide/ features/ design/ dev/
  https://raw.githubusercontent.com/clice-io/clice/<短 hash>/docs/zh/cli/<专题>.md
  ```
- CLI 最好与编辑器里的 `clice serve` 同版本（共享索引格式）；升级插件后若索引异常，删 `.clice/cache` 重建。

## 本仓库现状（✅ 已验证）

- **没有 `clice.toml`** → 用默认配置；`--configuration` 在本仓库**无意义**。
- 编译库**自动发现**：无规则时加载工作区根目录及其**直接子目录**里的所有 `compile_commands.json` → `build-dev_llvm/compile_commands.json` 自动采用。
- 索引在 `.clice/`（自带 `.gitignore` 与 `CACHEDIR.TAG`，勿手改）。
- ⚠️ **每次 `cmake --preset` 都会重写 compile_commands.json** → 索引过期。查询前加 `--fresh` 或 `tools/clice index`；**没有索引且不加 `--fresh` 会直接报错**。

## 高频配方

```bash
tools/clice query --method symbolSearch --symbol 'async_echo' --limit 10
tools/clice query --method definition --symbol 'foo'
tools/clice query --method references --symbol 'foo' --include-declaration
tools/clice query --method callGraph --symbol '#1a2b3c...' --direction callers
tools/clice query --method documentSymbols --path standalone/asio.cppm
tools/clice refactor rename --symbol 'server/session.cppm:56' --to 'ctx' --dry-run --fresh # ✅ 改名（先 dry-run 看 edits/conflicts，再去掉 --dry-run 落盘）
tools/clice query --method symbolSearch --symbol 'ns::Type::*' --limit 40   # ✅ 某个类的直接成员（带 kind：Method / Field / Operator）
tools/clice query --method symbolSearch --symbol '* kind:class' --limit 80 # ✅ 全仓的类；`struct` 是另一个 kind（kind:struct），可与 `path:` 组合收窄
tools/clice index                 # 增量建索引；Ctrl-C 断点续建
tools/clice index --stats --top 5 # 只读：索引构成
```

- 指认符号：**一律用 `--symbol`**（三种定位符：名字 / 答案里的 `#<hex>` / `file:line[:column]`）；
  ⚠️ **内部链接符号必须用 `#<hex>` 并同时加 `--path`**。输出是 JSON，`stale` 非空说明有文件被剔除。
- 答案约定：成功 `{"result": ..., "stale": [...]}`；答不上 `{"error": ...}` 退出码 1；参数错退出码 2。**`stale` 务必看**（被剔除的过期文件；定义落在这里的符号报 not found 并给文件名）；索引只认磁盘，未保存的 buffer 看不到。
- 输出是 JSON，用 `jq` 看。
- ⚠️ `refactor rename` **会改文件**：先 `--dry-run`，改完必须构建。

其余方法（typeHierarchy / compileCommand / projectFiles / fileDeps / impactAnalysis / counterparts / readSymbol）、
index 的只读检查（`--show-*`）、refactor 细节、`analyze modules`、`modularize` / `lint` / `format` / `inspect` → 见同目录 `ADVANCED.md`。


