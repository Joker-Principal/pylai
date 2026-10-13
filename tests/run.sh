#!/usr/bin/env bash
# 端到端验证：起服务器 → 发请求断言 → 收工。退出码 = http_e2e.py 的退出码。
#
# 用裸 socket 而不是 curl：curl 会读取环境变量，请求可能不按预期直连。
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

APP="build-dev_llvm/tests/Debug/app"
PORT=18080

if [[ ! -x $APP ]]; then
	echo "找不到 $APP —— 先跑 cmake --build --preset debug_llvm" >&2
	exit 1
fi

if ss -ltn 2>/dev/null | grep -q ":$PORT "; then
	echo "端口 $PORT 已被占用，先让它空出来" >&2
	exit 1
fi

"$APP" &
server_pid=$!
cleanup() {
	kill "$server_pid" 2>/dev/null
	wait "$server_pid" 2>/dev/null
}
trap cleanup EXIT

# 等端口就绪（最多 5s）
for _ in $(seq 50); do
	ss -ltn 2>/dev/null | grep -q ":$PORT " && break
	sleep 0.1
done

python3 tests/http_e2e.py
