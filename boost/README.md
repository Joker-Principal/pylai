# boost/ —— boost.asio 的 module 封装（草稿，未参与构建）

## 现状（✅ 已核实）

- 目录里只有 `boost.asio.cppm` 与 `CMakeLists.txt`。
- `boost/CMakeLists.txt` 里是 `add_library(boost-asio-module EXCLUDE_FROM_ALL)`，
  而**根 `CMakeLists.txt` 没有 `add_subdirectory(boost)`** → **当前完全不参与构建**。

## 它的职责（🟡 推断，待确认）

与 `standalone/` 同级——都属"**模块封装层**"：
把 **boost.asio**（vcpkg 的 `boost-asio` 端口，仓库里已作为依赖）也包成 C++20 module
（`export module boost.asio;`），与 standalone asio 的模块**并存**。

**不是**应用层，也不放业务逻辑。

## 将来若要启用

需要依次决定：
1. 根 `CMakeLists.txt` 加 `add_subdirectory(boost)`；
2. `standalone/`（standalone asio）与 `boost/`（boost.asio）是**并存**还是**二选一**？
3. 两者命名空间不同（`asio` vs `boost::asio`），本身不冲突，
   但**不要**在同一个 TU 里同时 import 两个模块并混用类型。

## 参考

- `standalone/README.md` —— 同层的参照物，里面写了封装层的一般做法与已知坑
- `docs/asio-notes.md` —— 机制细节
