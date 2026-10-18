# tests/ —— 端到端验证与示例应用

## 这个目录的职责

**唯一职责：拿 `server/` 的框架跑起来，验证它真的能服务。**

| 文件 | 作用 |
|---|---|
| `main.cpp` | 示例应用 `App`：声明 `/health`、`/users/{id}`、`/submit/{id}` 等路由，并启动服务器 |
| `stream_concepts.cpp` | 编译期护栏：两侧通道必须满足 asio 的 `AsyncReadStream` / `AsyncWriteStream` —— 拿组合操作去消费它们，缺一项就编不过 |
| `http_e2e.py` | 对运行中的服务器发原始 HTTP 请求，断言状态码与响应体 |
| `run.py` | 串起来：找产物 → 查端口 → 起服务器 → 跑 `http_e2e.py` → 收工（**跨平台、唯一入口**） |

产物可执行 target **`app`**（`build-dev_llvm/tests/Debug/app`）。

## 怎么跑

```bash
cmake --build --preset debug_llvm
python tests/run.py     # 起服务器 + 发请求 + 收工；退出码非 0 表示有用例失败
```

`run.py` 不依赖 bash（Windows 上直接 `python tests\run.py`）：自动在 `build-*/tests/<Config>/app[.exe]`
里找最新产物，也接受 `--exe <path>` / `--port <n>`。

手动跑（要自己看服务器日志时）：

```bash
./build-dev_llvm/tests/Debug/app 18080   # 参数 = 端口，省略则 18080
PYLAI_E2E_PORT=18080 python tests/http_e2e.py   # 另开一个终端
```

⚠️ **别用 curl**：它会读取环境变量，请求可能不按预期直连本机服务器。
`http_e2e.py` 走裸 socket 就是为了躲开这一点。
