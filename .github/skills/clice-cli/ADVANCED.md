# clice ADVANCED —— 冷门功能参考

主流程见同目录 `SKILL.md`；这些功能不常用，需要时再来查。同样遵守「本地 `<子命令> --help` 为准」。

## query 其余方法

```bash
tools/clice query --method typeHierarchy --name 'basic_io_object' --direction subtypes  # supertypes|subtypes|both
tools/clice query --method compileCommand --path standalone/asio.cpp    # 编辑器会用的完整编译命令（含 target 与系统 include 目录）
tools/clice query --method projectFiles --filter source|header|module
tools/clice query --method fileDeps --path standalone/asio.cppm --direction includes|includers --depth 1
tools/clice query --method impactAnalysis --path standalone/asio.cppm   # 改这文件会波及谁
tools/clice query --method counterparts --path standalone/asio.cpp      # 头/源/模块接口配对，preferred=编辑器会打开的那个
```

方法全集：`compileCommand, projectFiles, fileDeps, impactAnalysis, counterparts, symbolSearch, readSymbol, documentSymbols, definition, references, callGraph, typeHierarchy`。

- `symbolSearch` 附加项：`--limit`（默认 100）、`--kind Function,Struct,...`。
- 路径在问题里可相对、答案里一律绝对；行号 1 起。

## index 只读检查与运行细节

```bash
tools/clice index --show-symbol 'asio::io_context'   # 名字 / 限定名 / '#<hash>'（文件局部符号的唯一入口）
tools/clice index --show-file standalone/asio.cppm   # 该文件的持久化行：变体、贡献 TU、行数
tools/clice index --show-tu standalone/asio.cpp      # TU 清单：索引时间、贡献文件、include 树
tools/clice index --stats --top 5 --variants         # --variants 追加每 shard 一行 TSV，可对比两次运行
```

- 运行细节：Ctrl-C 断点续建、每 5 分钟自动存进度；退出码 0 完建 / 1 部分失败 / 2 参数错 / 130 中断。
- 锁：编辑器会话开着时，`clice index` 请 serve 代跑并等待；锁被无法请求的进程（别的 index、不同版本服务端）持有 → 直接失败并报出持有者。
- 检查模式全部**只读**、纯文本行（可 grep），不动索引。

## refactor rename 细节

```bash
tools/clice refactor rename --name 'foo' --to 'bar' --dry-run
tools/clice refactor rename --symbol '#1a2b3c...' --path standalone/asio.cpp --to 'bar' --fresh
```

- 重命名跨全仓的声明/定义/引用（含构造函数/析构、模板特化、虚函数 override 链）。运算符、宏、系统头里的符号不可重命名。
- 只写工作区文件（跳过隐藏目录、缓存目录、含 `CMakeCache.txt`/`build.ninja` 的构建树）；**不读未保存的 buffer**。
- 静态检查看不到语义变化（ADL、重载冲突等）→ **改完必须构建**。
- 文档里提到的 `--spec` 批量改名本机还没有。

## analyze modules —— 模块化改造前的只读报告

描述目录之间的依赖：环、环上的关键实体、内部分区、重建代价、热点、移动/拆分候选、障碍清单。是 `clice modularize` 的输入。

```bash
tools/clice analyze modules --view overview             # 默认
tools/clice analyze modules --view edge --from A --to B # 环上每条边的实体（id/归属/使用位置）
tools/clice analyze modules --view obstacles            # 机械改写带不走的构造
tools/clice analyze modules --view interface --module standalone
tools/clice analyze modules --view impact               # 每个头文件今天 vs 分区后的重建 TU 数
tools/clice analyze modules --view macros               # 跨文件使用的宏（import 不带宏）
tools/clice analyze modules --scope 'standalone/**' --depth 1
```

本仓库的 asio → `standalone/asio.cppm` 是手写模块封装；将来评估"应用代码该不该成模块、按什么粒度"时用它出报告。

## 其它子命令（本仓库没配 `clice.toml` 规则，慎用）

- `lint`：按最近的 `.clang-tidy` 跑全部 TU（没有配置时用默认小集合）。
- `format`：按 `.clang-format` 格式化。
- `inspect`：对源文件跑单个特性、打印原始 JSON；官网没有文档页，`tools/clice inspect --help` 是唯一说明。
- `modularize`：不改源文件地把第三方库包成 C++20 命名模块；已知限制多（full BMI、内部链接实体、std 只支持 libc++ 等）——使用前先通读 docs.clice.io/clice/cli/modularize。

## 不要用它做这些

- 编译 / 跑测试 / 构建 → 那是 CMake 与 ctest 的事（见 `AGENTS.md`）。
- 手工编辑 `.clice/` —— 它被 `.gitignore` 与 `CACHEDIR.TAG` 排除，删了重建即可。
