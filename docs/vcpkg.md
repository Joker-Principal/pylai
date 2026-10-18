# vcpkg（工作区**之外**）

vcpkg 根目录不在本工作区里，但工作时要经常查它。

| 项 | 路径 |
|---|---|
| vcpkg 根 | `<vcpkg 根>` |
| 内置端口目录 | `<vcpkg 根>/ports/` |
| vcpkg 工具链 | `<vcpkg 根>/scripts/buildsystems/vcpkg.cmake`（配置在 `CMakeUserPresets.json`，该文件 **gitignored**） |
| 本仓库安装结果 | `build-<preset>/vcpkg_installed/<triplet>/`（Linux `x64-linux`，Windows `x64-windows`） |
| 本仓库的 overlay ports | `ports/` —— 见 `ports/README.md` |

**triplet 由机器本地的 `CMakeUserPresets.json` 决定**（`VCPKG_TARGET_TRIPLET`）；不写则用 vcpkg 对本机的默认值。
平台相关的依赖在 `vcpkg.json` 里用 `"platform"` 表达式门控（`liburing` 只声明 `linux`），详见 `docs/cross-platform.md`。

## 最常用的动作：查"某库有没有端口、版本是多少"

```bash
ls <vcpkg 根>/ports | grep -i <关键词>
cat <vcpkg 根>/ports/<name>/vcpkg.json
```

这是**最快最准**的途径（比搜网页可靠），也是本仓库查版本的唯一口径 —— **以 `status` / `ports/*/vcpkg.json` 的实测为准**，不靠记忆。

## overlay port 机制

vcpkg 解析包名时，**overlay port 优先于注册表**。
本仓库通过 `vcpkg-configuration.json` 的 `"overlay-ports": ["./ports"]` 注册，
所以 `ports/asio/` 会覆盖内置的 `asio` 端口，`ports/jk/` 则提供注册表里没有的 `jk`。

## 与本仓库的其它交互点

- `vcpkg.json`：声明依赖与特性（目前：`jk`、`asio[spawn,ssl]`、`boost-asio[spawn,ssl]`、`openssl`、`liburing`(**仅 linux**)、`llhttp`）。
- **没有** `builtin-baseline` → 未启用版本约束模式，走"当前可用端口"。
- vcpkg **不会**导出 `PKG_CONFIG_PATH` → 用 pkg-config 的库要自己指
  （见根 `CMakeLists.txt` 里 liburing 的写法；该分支只在 Linux 上生效）。
- 安装的东西放在 `${CMAKE_BINARY_DIR}/vcpkg_installed/`，**不是**全局 `installed/`。

## 更新 vcpkg 根（✅ 2026-10-12 已验证流程）

```bash
cd <vcpkg 根>
git pull --ff-only
./bootstrap-vcpkg.sh
```

⚠️ **registry 更新后不 bootstrap 工具，configure 会直接报**：
`vcpkg-tools.json: document schema version 2 is not supported by this version of vcpkg`。

- 更新后依赖版本会随 registry 变化；2026-10-12 更新后：boost 1.90.0 → **1.92.0**、
  openssl 3.6.2 → **3.6.5**、liburing → **2.15**（重装耗时约 3 min，走 binary cache）。
- overlay port（`ports/asio` 1.38.2）不受 registry 更新影响（优先级更高）。
- 更新后必须重新 configure（触发重装）+ build 验证；曾因升级暴露 `ssl.cppm` 导出内部链接常量的问题。
