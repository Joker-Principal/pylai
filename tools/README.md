# tools/ —— 仓库内小工具

## clice

`tools/clice` 是一个 **sh 启动器**：去解析 VS Code 插件自带的 clice 二进制
（插件安装目录带版本后缀，扩展升级后路径会变，所以**不能写死路径**），可用 `CLICE_BIN` 覆盖。

**用途**：基于持久化索引做 C++ 代码检索——符号搜索、定义、引用、调用图、类型层次、
文件大纲、编译命令、影响面分析、文件依赖。比逐个 grep 准，也更省 token。

**⚠️ 索引会过期**：编译库来自 `build-dev_llvm/compile_commands.json`，
**每次 configure 都会重写它**；改动代码或重新 configure 后，查询前要么重建索引
（`tools/clice index`），要么加 `--fresh`，否则会看到 `stale` 或"找不到定义"。

完整用法与纪律见 `AGENTS.md` §3；技能说明见 `.github/skills/clice-cli/SKILL.md`；接口以 `tools/clice <子命令> --help` 为准。

## 什么放这里

仓库级的**启动器 / 脚本 / 一次性工具**，与业务代码、模块封装分开。
**不要**把算法或应用逻辑塞进 `tools/`。
