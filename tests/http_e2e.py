"""端到端验证：对真 Server 发原始 HTTP 请求，检查位置注入的路由注册是否生效。

用裸 socket 而不是 curl：curl 会读取环境变量，请求可能不按预期直连。
用法：./tests/run.sh（一条龙）；或先起 build-dev_llvm/tests/Debug/app，再 python3 tests/http_e2e.py
"""

import socket
import sys

HOST, PORT = "127.0.0.1", 18080


def request(method, target, body=b""):
    head = f"{method} {target} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nConnection: close\r\n"
    if body:
        head += f"Content-Length: {len(body)}\r\n"
    head += "\r\n"
    sock = socket.create_connection((HOST, PORT), timeout=5)
    sock.sendall(head.encode() + body)
    data = b""
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        data += chunk
    sock.close()
    return data.decode(errors="replace")


def split_body(raw):
    """响应体：头与体之间那个空行之后的部分（没找到分隔符就当没有体）。"""
    _, sep, body = raw.partition("\r\n\r\n")
    return body if sep else ""


# (method, target, 请求体, 期望状态码, 期望出现在响应里, 期望正文为空)
# 断言都带 \r\n 收尾，免得 "Allow: GET, HEAD" 命中 "Allow: GET, HEAD, POST"。
CASES = [
    ("GET", "/health", b"", 200, "ok", False),
    ("GET", "/users/42", b"", 200, "user id=42", False),          # Param<int,"id"> 位置注入
    ("POST", "/submit/7", b"x=1", 200, "submitted id=7", False),  # 同一组树里另一个动词
    ("GET", "/items", b"", 200, "items", False),                  # 同路径两个动词：GET 走 GET 那棵
    ("POST", "/items", b"", 200, "created", False),               # 同路径两个动词：POST 走 POST 那棵
    ("GET", "/nope", b"", 404, "404", False),                     # 路径不在
    ("GET", "/echo", b"", 404, "404", False),                     # 旧字符串 API 已移除 → 不再有这条路由
    ("GET", "/users/abc", b"", 200, "user id=0", False),          # 🔍 已知缺口：from_chars 失败静默给 0

    # 405 带 Allow；路径上有 GET 时隐含带出 HEAD（RFC 9110 §9.3.2）
    ("POST", "/health", b"", 405, "Allow: GET, HEAD\r\n", False),

    # HEAD：没写 Head<> 时复用 GET —— 无体，但 Content-Length 按 GET 算
    ("HEAD", "/health", b"", 200, "Content-Length: 3\r\n", True),   # GET 的体 "ok\n"
    ("HEAD", "/items", b"", 200, "Content-Length: 6\r\n", True),    # GET 的体 "items\n"
    ("HEAD", "/both", b"", 200, "Content-Length: 14\r\n", True),   # 显式 Head<> 优先（"explicit-head\n"）
    ("HEAD", "/nope", b"", 404, "404", True),

    # OPTIONS：没写 Options<> 时自动 204 + Allow
    ("OPTIONS", "/health", b"", 204, "Allow: GET, HEAD\r\n", True),
    ("OPTIONS", "/items", b"", 204, "Allow: GET, HEAD, POST\r\n", True),
    ("OPTIONS", "/both", b"", 204, "Allow: GET, HEAD\r\n", True),      # 有显式 HEAD → 不重复补
    ("OPTIONS", "/nope", b"", 404, "404", False),                       # 路径不在 → 404（不是 HEAD，正文照发）
]

failed = 0
for method, target, body, want_status, want_text, want_empty_body in CASES:
    try:
        raw = request(method, target, body)
    except OSError as exc:
        print(f"  ✗ {method} {target}: 连接失败 {exc}")
        failed += 1
        continue

    status_line = raw.split("\r\n", 1)[0]
    ok_status = f" {want_status} " in status_line
    ok_text = want_text in raw
    ok_body = (split_body(raw) == "") if want_empty_body else True
    mark = "✓" if (ok_status and ok_text and ok_body) else "✗"
    if mark == "✗":
        failed += 1
    extra = "" if ok_body else "  ← 正文非空"
    print(
        f"  {mark} {method:7} {target:14} -> {status_line!r} "
        f"(期望 {want_status}, 含 {want_text!r}){extra}"
    )

print(("FAILED: %d" % failed) if failed else "ALL PASS")
sys.exit(1 if failed else 0)
