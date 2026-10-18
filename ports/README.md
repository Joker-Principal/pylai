# ports/ —— vcpkg overlay ports

## 这个目录的职责

存放**自建的 vcpkg 端口**，用于覆盖 / 补充 vcpkg 内置注册表里的端口。
根目录 `vcpkg-configuration.json` 里的 `"overlay-ports": ["./ports"]` 把它接进来。

**什么时候才需要往这里加东西**：想在 vcpkg 里用某个**内置端口没有 / 版本太旧**的库时。

## 现在有什么

- `asio/` —— 把 asio 固定到 **1.38.2**。
  原因：vcpkg **内置**的 `asio` 端口停在 **1.32.0**，且特性名与我们要的不一致。
- `jk/` —— 把 [jk](https://github.com/Joker-Principal/jk) 打成端口（钉 tag **v2.0.0**）。
  这样它与其它依赖一样由 vcpkg 装进 triplet，构建时不再 FetchContent 拉源码。
  ⚠️ 改版本时三处要一起改：`portfile.cmake` 的 `REF` / `SHA512`，以及 `vcpkg.json` 的 `version`。

| 文件 | 作用 |
|---|---|
| `vcpkg.json` | 版本 + 特性（`spawn→boost-context`、`ssl→openssl`、`regex→boost-regex`） |
| `portfile.cmake` | 下载源码（校验 SHA512）→ CMake 安装 |
| `CMakeLists.txt` / `asio-config.cmake` | 提供 `asio::asio` target 与 pkg-config |

## 怎么加一个新端口

1. 建 `ports/<name>/`，写 `vcpkg.json` + `portfile.cmake`；
2. 若上游源码布局特殊（例如 asio 1.38 把 `asio.pc.in` 挪到了根目录、
   旧的 `asio/` 目录只剩符号链接），就要**自带一份 `CMakeLists.txt`**，别照抄内置端口；
3. 在 `vcpkg.json` 里引用端口名即可——overlay **优先于**注册表。

## 注意事项

- 端口改动后需要重新 configure；`vcpkg install` 会重新校验 hash。
- 判断"某库有没有端口、版本多少"，**先查本机 vcpkg 目录**（`docs/vcpkg.md`），比搜网页可靠。
- 端口文件也属于"基础设施"，**不要**把应用逻辑写进 `portfile.cmake` 之外的任何地方。
