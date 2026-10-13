# standalone/ —— asio → C++20 module 封装层

## 这个目录的职责

把 **standalone asio** 暴露成 C++20 modules，供应用 `import`。**只有这一件事。**

- **产出 target**：`asio-module`（静态库）
- **导出模块**：
  - `asio` ← `asio.cppm`
  - `asio.ssl` ← `asio/ssl.cppm`
  - `asio.experimental` ← `asio/experimental.cppm`
- **分离编译单元**（asio 需要，通常一个 `.cpp` 对应一个模块）：
  - `asio.cpp` → `<asio/impl/src.hpp>`
  - `asio/ssl.cpp` → `<asio/ssl/impl/src.hpp>`
  - `asio/experimental.cpp` → experimental 的 `.ipp`
- **`CMakeLists.txt`**：负责链接（`asio::asio`、`Boost::context`、OpenSSL、`PkgConfig::liburing`）
  与编译宏（`ASIO_SEPARATE_COMPILATION` / `ASIO_NO_DEPRECATED` / `ASIO_HAS_IO_URING[_AS_DEFAULT]`）
- 新增 asio 的 standalone 头文件不需要改 CMake：`GLOB_RECURSE *.cpp / *.cppm` 会自动纳入

## 什么**不该**放这里

应用逻辑、HTTP 相关的东西、指标、策略、业务配置。
那些属于**应用层**——见根 `README.md` 的目录地图。

## 怎么扩展（应用需要新 asio 名字时）

**在 `asio.cppm`（或对应子模块）的导出清单里加一行 `using`**。
**不要**在应用 TU 里 `#include <asio/...>` 绕过模块——module/header 混用会出问题。

## 已知约束（细节与出处见 `docs/asio-notes.md`）

- `ASIO_NO_DEPRECATED=1` 会让 `asio::basic_io_object` 整个消失
  → 模块里对应的 `using` 必须加同样的守卫。
- `asio::ssl::verify_peer` 是命名空间级 `const int`（内部链接），**不能**在模块里 re-export。
- `#include <asio/spawn.hpp>` 只在 `ASIO_HAS_BOOST_CONTEXT_FIBER` 下才有意义；
  `yield_context` **只**在 `spawn.hpp` 里声明。
