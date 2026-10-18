"""端到端验证：对真 Server 发原始 HTTP 请求，检查解析 / 路由 / 应答是否符合预期。

用裸 socket 而不是 curl：curl 会读取环境变量，请求可能不按预期直连。
用法：python tests/run.py（一条龙，跨平台）；或先起产物，再 python tests/http_e2e.py
地址与端口由 PYLAI_E2E_HOST / PYLAI_E2E_PORT 指定，默认 127.0.0.1:18080。
"""

import os
import socket
import sys

# Windows 控制台默认不是 UTF-8 ⇒ 断言里的中文会乱码或直接抛 UnicodeEncodeError
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = os.environ.get("PYLAI_E2E_HOST", "127.0.0.1")
PORT = int(os.environ.get("PYLAI_E2E_PORT", "18080"))


def request(method, target, body=b"", chunked=False):
    head = f"{method} {target} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nConnection: close\r\n"
    payload = b""
    if chunked:
        # 每 3 字节一块：既试 chunked，也试"块边界与读边界不一致"
        head += "Transfer-Encoding: chunked\r\n"
        pieces = [body[i : i + 3] for i in range(0, len(body), 3)]
        payload = b"".join(b"%x\r\n%s\r\n" % (len(p), p) for p in pieces) + b"0\r\n\r\n"
    elif body:
        head += f"Content-Length: {len(body)}\r\n"
        payload = body
    head += "\r\n"

    sock = socket.create_connection((HOST, PORT), timeout=5)
    sock.sendall(head.encode() + payload)
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


# (method, target, 请求体, 期望状态码, 期望出现在响应里, 期望正文为空[, chunked=True])
# 断言都带 \r\n 收尾，免得 "Allow: GET, HEAD" 命中 "Allow: GET, HEAD, POST"。
CASES = [
    ("GET", "/health", b"", 200, "ok", False),
    ("GET", "/users/42", b"", 200, "user id=42", False),          # req.param<int,"id">()
    ("POST", "/submit/7", b"x=1", 200, "submitted id=7", False),  # 同一组树里另一个动词（且不读体 ⇒ 会 Connection: close）
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

    # 请求体：handler 用 `co_await req.body()` / `read_some()` 自己拉（框架只读到 headers）
    ("POST", "/body", b"hello", 200, "body=hello", False),
    ("POST", "/body", b"", 200, "body=", False),                                  # 空体
    ("POST", "/body", b"chunked-payload", 200, "body=chunked-payload", False, True),  # Transfer-Encoding: chunked
    ("POST", "/body", b"x" * 1025, 413, "413", False),                        # 测试程序把上限设为 1024 → 413
    # 长度未知（chunked）⇒ 头部阶段判不出来，只能由读体侧计数拦下（走 BodyError 那条路）
    ("POST", "/body", b"y" * 1025, 413, "413", False, True),

    # 用 asio 的组合操作读体（`asio::async_read` + 动态缓冲）—— 验证通道是 AsyncReadStream，
    # 且“体到尾”在门面侧被翻译成 asio 语义的 `eof`。
    ("POST", "/count", b"hello", 200, "count=5 eof=true", False),
]

def check(method, target, body, want_status, want_text, want_empty_body, chunked=False):
    """发一个请求并断言；返回是否通过（同时打印一行）。"""
    try:
        raw = request(method, target, body, chunked)
    except OSError as exc:
        print(f"  ✗ {method} {target}: 连接失败 {exc}")
        return False

    status_line = raw.split("\r\n", 1)[0]
    ok_status = f" {want_status} " in status_line
    ok_text = want_text in raw
    ok_body = (split_body(raw) == "") if want_empty_body else True
    ok = ok_status and ok_text and ok_body
    extra = "" if ok_body else "  ← 正文非空"
    print(
        f"  {'✓' if ok else '✗'} {method:7} {target:14} -> {status_line!r} "
        f"(期望 {want_status}, 含 {want_text!r}){extra}"
    )
    return ok


def check_pipeline():
    """同一连接上连发两个请求（第一个带体）：体的字节不能被算进下一个请求。"""
    sock = socket.create_connection((HOST, PORT), timeout=5)
    sock.sendall(
        b"POST /body HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc"
        b"GET /health HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n"
    )
    raw = b""
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        raw += chunk
    sock.close()

    text = raw.decode(errors="replace")
    got = text.count("HTTP/1.1 200")
    ok = got == 2 and "body=abc" in text and "ok" in text
    print(f"  {'✓' if ok else '✗'} {'pipeline':7} {'(两个请求)':14} -> 收到 {got} 个 200（期望 2）")
    return ok


def check_close_header():
    """请求带 Connection: close ⇒ 响应头里也要写 close。

    现有用例都靠“读到 EOF”隐式验证连接关了，但没人断言这个头（见 http-server-plan.md §4.1.2 D）。
    """
    raw = request("GET", "/health")
    head = raw.partition("\r\n\r\n")[0]
    ok = "Connection: close" in head
    print(f"  {'✓' if ok else '✗'} {'close头':7} {'/health':14} -> 响应头{'有' if ok else '没有'} Connection: close")
    return ok


def check_unread_body(method, target, body, want_status, label):
    """体没读完（handler 不读 / 只读一半）⇒ 响应写 Connection: close，并且连接真的被关。"""
    sock = socket.create_connection((HOST, PORT), timeout=5)
    head = f"{method} {target} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nContent-Length: {len(body)}\r\n\r\n"
    sock.sendall(head.encode() + body)

    raw = b""
    closed = False
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                closed = True
                break
            raw += chunk
    except TimeoutError:
        pass  # 服务器没关连接 ⇒ closed 保持 False
    sock.close()

    text = raw.decode(errors="replace")
    head_only = text.partition("\r\n\r\n")[0]
    status_ok = f"HTTP/1.1 {want_status}" in text
    ok = status_ok and "Connection: close" in head_only and closed
    print(
        f"  {'✓' if ok else '✗'} {label:7} {target:14} -> "
        f"{text.splitlines()[0] if text else '（无响应）'!r}"
        f" (期望 {want_status} + Connection: close + 连接关闭；实际 close头="
        f"{'有' if 'Connection: close' in head_only else '无'}, 连接{'关' if closed else '未关'})"
    )
    return ok


def decode_chunked(raw_body):
    """解 chunked：返回（载荷, 终止块之后的 trailer 字段行）。"""
    payload = b""
    trailers = []
    pos = 0
    while True:
        end = raw_body.index(b"\r\n", pos)
        size = int(raw_body[pos:end].split(b";")[0], 16)
        pos = end + 2
        if size == 0:
            head = raw_body[pos:].split(b"\r\n\r\n", 1)[0]
            trailers = [line for line in head.split(b"\r\n") if line]
            break
        payload += raw_body[pos : pos + size]
        pos += size + 2  # 跳过载荷与它后面的 CRLF
    return payload, trailers


def check_stream():
    """长度未知的响应：响应侧 chunked，并在末块之后补 trailer。

    依据：RFC 9112 §7.1.2（`chunked-body = *chunk last-chunk trailer-section CRLF`）、
    RFC 9110 §6.6.2（预告用 `Trailer:`）。
    """
    raw = request("GET", "/stream")
    head, _, body = raw.partition("\r\n\r\n")
    payload, trailers = decode_chunked(body.encode())
    ok = (
        "Transfer-Encoding: chunked" in head
        and "Trailer: X-Chunks" in head
        and payload == b"chunk-a\nchunk-b\n"
        and trailers == [b"X-Chunks: 2"]
    )
    print(
        f"  {'✓' if ok else '✗'} {'GET':7} {'/stream':14} -> "
        f"chunked={'是' if 'Transfer-Encoding: chunked' in head else '否'}, "
        f"预告={'有' if 'Trailer: X-Chunks' in head else '无'}, "
        f"载荷={payload!r}, trailer={trailers!r}"
    )
    return ok


def check_pipeline_after_stream():
    """chunked 响应的末块之后，连接还能继续用 —— 即末块 / trailer 的成帧确实收尾了。"""
    sock = socket.create_connection((HOST, PORT), timeout=5)
    sock.sendall(
        b"GET /stream HTTP/1.1\r\nHost: h\r\n\r\n"
        b"GET /health HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n"
    )
    raw = b""
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        raw += chunk
    sock.close()

    ok = raw.count(b"HTTP/1.1 200") == 2 and b"chunk-b" in raw and b"ok\n" in raw
    print(
        f"  {'✓' if ok else '✗'} {'pipeline':7} {'(stream+health)':14} -> "
        f"收到 {raw.count(b'HTTP/1.1 200')} 个 200（期望 2，且 chunked 体后能接上第二个响应）"
    )
    return ok


def check_no_content_length_on_204():
    """204 不得带 Content-Length（RFC 9110 §8.6）—— 旧实现在 204 上也写了 `Content-Length: 0`。"""
    raw = request("OPTIONS", "/health")
    head = raw.partition("\r\n\r\n")[0]
    ok = " 204 " in head.splitlines()[0] and "Content-Length" not in head
    print(
        f"  {'✓' if ok else '✗'} {'OPTIONS':7} {'/health':14} -> "
        f"{head.splitlines()[0]!r}（期望 204 且无 Content-Length）"
    )
    return ok


failed = (
    [check(*case) for case in CASES].count(False)
    + (0 if check_pipeline() else 1)
    + (0 if check_close_header() else 1)
    + (0 if check_unread_body("POST", "/health", b"ignored-body", 405, "unread") else 1)
    + (0 if check_unread_body("POST", "/peek", b"0123456789", 200, "halfread") else 1)
    + (0 if check_stream() else 1)
    + (0 if check_pipeline_after_stream() else 1)
    + (0 if check_no_content_length_on_204() else 1)
)

print(("FAILED: %d" % failed) if failed else "ALL PASS")
sys.exit(1 if failed else 0)
