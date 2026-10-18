"""端到端验证的跨平台入口：找产物 → 起服务器 → 等端口 → 跑断言 → 收工。

用法：
    python tests/run.py                  # 自动找 build-*/tests/<Config>/app[.exe]
    python tests/run.py --exe <path>     # 指定产物
    python tests/run.py --port 18080     # 指定端口（默认 18080）

退出码 = http_e2e.py 的退出码；找不到产物或端口被占用返回非 0。
"""

from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PORT = 18080
APP_NAMES = ("app.exe", "app")


def find_app() -> Path | None:
    """在 build-*/tests/<Config>/ 下找产物，多份取最新的一份。"""
    candidates: list[Path] = []
    for build in sorted(ROOT.glob("build-*")):
        for name in APP_NAMES:
            candidates.extend(build.glob(f"tests/*/{name}"))
    if not candidates:
        return None
    # Debug 优先（与 build presets 的默认配置一致），同配置内取最新
    candidates.sort(key=lambda p: (p.parts[-2] != "Debug", -p.stat().st_mtime))
    return candidates[0]


def port_is_free(host: str, port: int) -> bool:
    """有人在这个地址上 LISTEN 才算被占用。

    不用 bind 探测：服务端（asio）默认开 SO_REUSEADDR，而刚关掉的连接会留在 TIME_WAIT，
    bind 探测会把“能起来”误判成“被占用”。
    """
    try:
        with socket.create_connection((host, port), timeout=0.2):
            return False
    except OSError:
        return True


def wait_until_listening(host: str, port: int, proc: subprocess.Popen[bytes], timeout: float = 5.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:  # 服务器先退了，别干等
            return False
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description="起服务器并跑端到端断言")
    parser.add_argument("--exe", type=Path, help="服务器可执行文件（默认自动查找）")
    parser.add_argument("--host", default="127.0.0.1", help="监听 / 连接的地址")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="监听 / 连接的端口")
    args = parser.parse_args()

    app = args.exe or find_app()
    if app is None or not app.is_file():
        print("找不到服务器产物 —— 先跑 cmake --build，或用 --exe 指定", file=sys.stderr)
        return 1
    if not port_is_free(args.host, args.port):
        print(f"端口 {args.port} 已被占用，先让它空出来（或用 --port 换一个）", file=sys.stderr)
        return 1

    print(f"启动 {app}（{args.host}:{args.port}）", flush=True)
    proc = subprocess.Popen([str(app), str(args.port)], cwd=ROOT)
    try:
        if not wait_until_listening(args.host, args.port, proc):
            print("服务器没能开始监听（5s 超时或提前退出）", file=sys.stderr)
            return 1
        env = os.environ.copy()
        env["PYLAI_E2E_HOST"] = args.host
        env["PYLAI_E2E_PORT"] = str(args.port)
        return subprocess.call([sys.executable, str(Path(__file__).with_name("http_e2e.py"))], cwd=ROOT, env=env)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    sys.exit(main())
