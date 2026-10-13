# tests/ —— 端到端验证与示例应用

## 这个目录的职责

**唯一职责：拿 `server/` 的框架跑起来，验证它真的能服务。**

| 文件 | 作用 |
|---|---|
| `main.cpp` | 示例应用 `App`：声明 `/health`、`/users/{id}`、`/submit/{id}` 等路由，并启动服务器 |
| `route_static.cpp` | 路由那套的**编译期用例**（`static_assert`）：pattern 语法 / 匹配语义 / 冲突规则 / DSL 与 `param()` |
| `http_e2e.py` | 对运行中的服务器发原始 HTTP 请求，断言状态码与响应体 |
| `run.sh` | 串起来：查端口 → 起服务器 → 跑 `http_e2e.py` → 收工 |

产物可执行 target **`app`**（`build-dev_llvm/tests/Debug/app`）。

> `route_static.cpp` 不参与运行 —— `static_assert` 在**编译期**就求值完了，**构建过 = 用例过**。
> 它只用 `import pylai;` 的**导出面**（消费方视角），顺带验收“该导出的都导出了”。
> ⚠️ “必须编不过”的失败路径不在这里（编译期拦不住的、或拦下的错误文案）→ 人工探针，见 `docs/http-server-plan.md` §11.2。

## 为什么应用不在 `server/`

`server/` 只提供 **HTTP server 框架**（静态库 `pylai`），不含任何业务路由：
- 框架不该替使用者决定有哪些路由；
- 示例应用得**真的跑起来**才验证得了框架，所以它天然属于这里。

将来它会 [herodotus](https://github.com/Joker-Principal/herodotus) 的 `clio` 取代 ——
那时代替 `App` 的会是真正的应用，而不是示例。

## 怎么跑

```bash
cmake --build --preset debug_llvm
./tests/run.sh          # 起服务器 + 发请求 + 收工；退出码非 0 表示有用例失败
```

手动跑（要自己看服务器日志时）：

```bash
./build-dev_llvm/tests/Debug/app    # 监听 127.0.0.1:18080
python3 tests/http_e2e.py           # 另开一个终端
```

⚠️ **别用 curl**：它会读取环境变量，请求可能不按预期直连本机服务器。
`http_e2e.py` 走裸 socket 就是为了躲开这一点。
