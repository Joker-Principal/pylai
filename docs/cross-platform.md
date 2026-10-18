# 跨平台支持（Linux · Windows · macOS）

> 目标：**同一套源码，三个平台都能编、都能跑**；平台差异只准出现在**构建配置**里，不准散进 `server/` / `standalone/` 的实现。
> 状态标记：**✅ 已验证** · **🟡 倾向** · **🔍 待调研 / 未验证** · **❓ 未定**。

## 1. 后端矩阵

asio 自己会按平台选后端；我们**不定义**跨平台的宏，只在 Linux 上打开 io_uring。

| 平台 | asio 后端 | 我们额外做的 | 状态 |
|---|---|---|---|
| Linux（含 WSL2） | **io_uring** | `ASIO_HAS_IO_URING=1` + `ASIO_HAS_IO_URING_AS_DEFAULT=1`，链接 `liburing` | ✅ 日常验证（构建 + 30 条 e2e） |
| Windows | **IOCP**（默认） | 链接 `ws2_32` / `mswsock`；`/utf-8 /permissive- /Zc:preprocessor` | 🔍 配置已分叉，**未在真机验证** |
| macOS | **kqueue**（默认） | 无 | 🔍 未验证 |

## 2. 分叉点在哪（只有三处）

| 位置 | 做什么 | 判据 |
|---|---|---|
| 根 `CMakeLists.txt` | `PkgConfig` 与 `liburing` 只在 **Linux** 查找；MSVC 加 `/utf-8`（源码注释是中文 UTF-8）与 `/Zc:preprocessor`（modules 前提） | `CMAKE_SYSTEM_NAME STREQUAL "Linux"` / `MSVC` |
| `standalone/CMakeLists.txt` | 按平台拼 `ASIO_PLATFORM_LIBS`；io_uring 的两个宏只在 Linux 定义 | 同上 + `WIN32` |
| `vcpkg.json` | `liburing` 声明 `"platform": "linux"`（该端口自带 `"supports": "linux"`，不门控会让 Windows 上的 configure 直接失败） | vcpkg manifest 的 platform 表达式 |

**代码里没有 `#ifdef _WIN32`** —— `server/` 用的 `asio::signal_set` 本身就是跨平台的：
Windows 上 Ctrl+C 走 `SIGINT`，`SIGTERM` 不会被触发（不需要特判，只是那条路走不到）。

## 3. 在 Windows 上构建

### 3.1 前置

- **MSVC 14.34+**（VS 2022 17.4+；VS 2026 亦可）。CMake 的 C++ modules 支持以此为下限。
- **Ninja Multi-Config**（VS 自带的 `ninja.exe` 即可）。
- **vcpkg**：clone 后 `bootstrap-vcpkg.bat`，默认 triplet 是 `x64-windows`。
- CMake **≥ 3.28**（仓库里 `cmake_minimum_required` 保持 3.28）。

⚠️ 用 Ninja 生成器时 `cl.exe` 必须在 `PATH` 里 ⇒ 从 **“x64 Native Tools Command Prompt”** / **Developer PowerShell** 启动。

### 3.2 预设：每台机器自备 `CMakeUserPresets.json`

预设的一般约定（tracked 只管公共部分、两份文件不能重名、不能跨文件继承）→ `maintainers.md` §1.2。下面是 Windows 的具体写法。

```json
{
  "version": 8,
  "configurePresets": [
    {
      "name": "toolchain",
      "hidden": true,
      "toolchainFile": "C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": { "VCPKG_TARGET_TRIPLET": "x64-windows" }
    },
    { "name": "msvc", "hidden": true, "cacheVariables": { "CMAKE_CXX_COMPILER": "cl" } },
    { "name": "dev_msvc", "inherits": [ "base", "toolchain", "msvc" ] }
  ],
  "buildPresets": [
    { "name": "debug_msvc", "inherits": [ "base_debug" ], "configurePreset": "dev_msvc" }
  ]
}
```

### 3.3 构建与验证

```bat
cmake --preset dev_msvc
cmake --build --preset debug_msvc
python tests\run.py
```

产物在 `build-dev_msvc\tests\Debug\app.exe`；`tests/run.py` 会自动找到它（也接受 `--exe` 指定）。

## 4. 已知风险

| 风险 | 说明 |
|---|---|
| **deducing this** | `server/route.cppm` 的响应链式 setter 用了 C++23 显式对象参数（`auto&& status(this Self&& self, int)`）。MSVC 需 17.10+，且历史上对转发/推导有限制 ⇒ Windows 首次构建最可能在这里栽 |
| **IOCP 与 io_uring 语义差异** | 取消、关闭、定时器的时序不同。`server/` 没有依赖 io_uring 特有行为，但**唯一护栏就是 e2e**（30 条：pipeline / 半包 / chunked / 413 / 未读体关闭…） |
| **SIGTERM** | Windows 上不会触发；优雅停机靠 Ctrl+C（`SIGINT`）。不要据此写“收到 SIGTERM”的测试 |
| **控制台编码** | `http_e2e.py` 已把 stdout 切到 UTF-8；服务器自身日志若是中文，Windows 控制台可能显示乱码（不影响断言） |
| **静态/动态 CRT** | 若改用 `x64-windows-static`，`vcpkg` 与 MSVC 运行库必须一致（`CMAKE_MSVC_RUNTIME_LIBRARY`），否则链接期报 LNK2038 |

## 5. 复核口径

- Linux：`cmake --build --preset debug_llvm` 退出码 0，`python tests/run.py` 输出 `ALL PASS`。
- Windows / macOS 一旦实测，把上面矩阵里的 🔍 换成 ✅ 并记进 `docs/changelog.md`（写明编译器版本与命令）。
