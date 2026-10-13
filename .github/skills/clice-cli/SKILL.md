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
- ⚠️ **本地 `--help` 是接口真相**：docs.clice.io 常比已装版本新（如统一 `--symbol` 定位符、`refactor --spec` 本机还没有），用法冲突时以 `tools/clice <子命令> --help` 为准。
- CLI 最好与编辑器里的 `clice serve` 同版本（共享索引格式）；升级插件后若索引异常，删 `.clice/cache` 重建。

## 本仓库现状（✅ 已验证）

- **没有 `clice.toml`** → 用默认配置；`--configuration` 在本仓库**无意义**。
- 编译库**自动发现**：无规则时加载工作区根目录及其**直接子目录**里的所有 `compile_commands.json` → `build-dev_llvm/compile_commands.json` 自动采用。
- 索引在 `.clice/`（自带 `.gitignore` 与 `CACHEDIR.TAG`，勿手改）。
- ⚠️ **每次 `cmake --preset` 都会重写 compile_commands.json** → 索引过期。查询前加 `--fresh` 或 `tools/clice index`；**没有索引且不加 `--fresh` 会直接报错**。

## 高频配方

```bash
tools/clice query --method symbolSearch --query 'async_echo' --limit 10
tools/clice query --method definition --name 'foo'
tools/clice query --method references --name 'foo' --include-declaration
tools/clice query --method callGraph --symbol '#1a2b3c...' --direction callers
tools/clice query --method documentSymbols --path standalone/asio.cppm
tools/clice index                 # 增量建索引；Ctrl-C 断点续建
tools/clice index --stats --top 5 # 只读：索引构成
```

- 指认符号：`--name <名字>`（模糊/限定名）；`--symbol '#<hex>'`（答案里的 id，**内部链接符号必须同时加 `--path`**）；`--path <文件> --line <n>`。
- 答案约定：成功 `{"result": ..., "stale": [...]}`；答不上 `{"error": ...}` 退出码 1；参数错退出码 2。**`stale` 务必看**（被剔除的过期文件；定义落在这里的符号报 not found 并给文件名）；索引只认磁盘，未保存的 buffer 看不到。
- 输出是 JSON，用 `jq` 看。
- ⚠️ `refactor rename` **会改文件**：先 `--dry-run`，改完必须构建。

其余方法（typeHierarchy / compileCommand / projectFiles / fileDeps / impactAnalysis / counterparts / readSymbol）、
index 的只读检查（`--show-*`）、refactor 细节、`analyze modules`、`modularize` / `lint` / `format` / `inspect` → 见同目录 `ADVANCED.md`。


