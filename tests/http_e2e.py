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


# (请求, 期望状态码, 期望 body 出现在响应里)
CASES = [
    ("GET", "/health", b"", 200, "ok"),
    ("GET", "/users/42", b"", 200, "user id=42"),          # Param<int,"id"> 位置注入
    ("POST", "/submit/7", b"x=1", 200, "submitted id=7"),  # 同一组树里另一个动词
    ("POST", "/health", b"", 405, "405"),                  # 路径在、动词不符
    ("GET", "/items", b"", 200, "items"),                  # 同路径两个动词：GET 走 GET 那棵
    ("POST", "/items", b"", 200, "created"),               # 同路径两个动词：POST 走 POST 那棵
    ("GET", "/nope", b"", 404, "404"),                     # 路径不在
    ("GET", "/echo", b"", 404, "404"),                     # 旧字符串 API 已移除 → 不再有这条路由
    ("GET", "/users/abc", b"", 200, "user id=0"),          # 🔍 已知缺口：from_chars 失败静默给 0
]

failed = 0
for method, target, body, want_status, want_text in CASES:
    try:
        raw = request(method, target, body)
    except OSError as exc:
        print(f"  ✗ {method} {target}: 连接失败 {exc}")
        failed += 1
        continue

    status_line = raw.split("\r\n", 1)[0]
    ok_status = f" {want_status} " in status_line
    ok_text = want_text in raw
    mark = "✓" if (ok_status and ok_text) else "✗"
    if mark == "✗":
        failed += 1
    print(f"  {mark} {method:4} {target:14} -> {status_line!r} (期望 {want_status}, body 含 {want_text!r})")

print(("FAILED: %d" % failed) if failed else "ALL PASS")
sys.exit(1 if failed else 0)
