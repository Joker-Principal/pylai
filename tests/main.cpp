#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

import asio; // handler 返回 asio::awaitable<void> ⇒ 消费方自己 import asio（框架不 export import 它）
import pylai;

namespace {

// 声明写法：动词 + 路径住在**第一个形参**（`Get<P>` 既是路由声明、也是请求类型）；响应按引用传进来，
// 链式填状态、链尾 `flush()` 出网。注册入口是 `Builder::route_on(&app, &App::...)`。
using namespace pylai::route;

struct App
{
public:
	asio::awaitable<void> health(Get<"/health"> req, pylai::route::StringResponse& res);
	asio::awaitable<void> user(Get<"/users/{id}"> req, pylai::route::StringResponse& res);
	asio::awaitable<void> submit(Post<"/submit/{id}"> req, pylai::route::StringResponse& res);

	// 同一条路径、两个动词：树按 pattern 建、动词不参与，所以这一组必须靠「每 method 一棵树」才不互相吃。
	asio::awaitable<void> list_items(Get<"/items"> req, pylai::route::StringResponse& res);
	asio::awaitable<void> create_item(Post<"/items"> req, pylai::route::StringResponse& res);

	// HEAD 的钉子：`/both` 同时有 GET 与显式 `Head<>`（显式优先）；`/health` 只有 GET → HEAD 复用 GET。
	asio::awaitable<void> both(Get<"/both"> req, pylai::route::StringResponse& res);
	asio::awaitable<void> both_head(Head<"/both"> req, pylai::route::StringResponse& res);

	// 请求体：框架读到 headers 就交棒，体由 handler 自己拉（`co_await req.body()`，受 max_body_bytes 约束）。
	asio::awaitable<void> echo_body(Post<"/body"> req, pylai::route::StringResponse& res);
	// 只读一部分体就返回：e2e 用它验“未读体 ⇒ Connection: close”。
	asio::awaitable<void> peek_body(Post<"/peek"> req, pylai::route::StringResponse& res);

	// 长度未知的响应：handler 在**运行期间**分段推（响应侧 chunked），结束时补 trailer。
	asio::awaitable<void> stream(Get<"/stream"> req, pylai::route::UnknownLengthResponse& res);

	// 用 asio 的**组合操作**读体（两个通道满足 AsyncReadStream / AsyncWriteStream，见 tests/stream_concepts.cpp）。
	asio::awaitable<void> count_body(Post<"/count"> req, pylai::route::StringResponse& res);
};

asio::awaitable<void> App::health(Get<"/health">, pylai::route::StringResponse& res)
{
	co_await res.body("ok\n").flush(); // 链的最后一环 = flush：写到这一刻才碰 socket
}

asio::awaitable<void> App::user(Get<"/users/{id}"> req, pylai::route::StringResponse& res)
{
	co_await res.body(fmt::format("user id={}\n", req.param<int, "id">())).flush();
}

asio::awaitable<void> App::submit(Post<"/submit/{id}"> req, pylai::route::StringResponse& res)
{
	co_await res.body(fmt::format("submitted id={}\n", req.param<int, "id">())).flush();
}

asio::awaitable<void> App::list_items(Get<"/items">, pylai::route::StringResponse& res)
{
	co_await res.body("items\n").flush();
}

asio::awaitable<void> App::create_item(Post<"/items">, pylai::route::StringResponse& res)
{
	co_await res.body("created\n").flush();
}

asio::awaitable<void> App::both(Get<"/both">, pylai::route::StringResponse& res)
{
	co_await res.body("get\n").flush();
}

asio::awaitable<void> App::both_head(Head<"/both">, pylai::route::StringResponse& res)
{
	co_await res.body("explicit-head\n").flush(); // 长度与 GET 故意不同 → e2e 据此看出是谁应答的
}

// 请求体是**字节**（`std::vector<std::byte>`）；`as_text` 是“字节 → 文本”的显式转换。
asio::awaitable<void> App::echo_body(Post<"/body"> req, pylai::route::StringResponse& res)
{
	const std::vector<std::byte> body = co_await req.body();
	co_await res.body(fmt::format("body={}", pylai::http::as_text(body))).flush();
}

// 只读一小段体就返回（见声明处的说明）。
asio::awaitable<void> App::peek_body(Post<"/peek"> req, pylai::route::StringResponse& res)
{
	std::array<std::byte, 4> head{};
	const std::size_t n = co_await req.read_some(head);
	co_await res.body("peek=").body(std::span{head}.first(n)).flush();
}

// 长度未知：一块一块推。
asio::awaitable<void> App::stream(Get<"/stream">, pylai::route::UnknownLengthResponse& res)
{
	// 预告 `Trailer: X-Chunks`（必须在发头之前）；头 + 第一块由链尾的 flush 一起发出去
	if(co_await res.trailer("X-Chunks").body("chunk-a\n").flush()) co_return;
	if(co_await res.body("chunk-b\n").flush()) co_return;

	const std::array trailers{pylai::http::Header{"X-Chunks", "2"}};
	co_await res.finish(trailers);
}

// 用 asio 的组合操作读体：动态缓冲会一直长到 **eof**（= 请求体到尾，由门面把内部的 0 翻译过来）。
asio::awaitable<void> App::count_body(Post<"/count"> req, pylai::route::StringResponse& res)
{
	std::vector<std::byte> body;
	const auto [ec, n] = co_await asio::async_read(
		req, asio::dynamic_buffer(body), asio::as_tuple(asio::use_awaitable)
	);
	(void)n;
	co_await res.body(fmt::format("count={} eof={}\n", body.size(), ec == asio::error::eof)).flush();
}

} // namespace

int main()
{
	try
	{
		App app; // 必须比 server 活得久：路由里存的是 &app
		auto server = pylai::Server::builder()
		                  .bind("127.0.0.1", 18080)
			                  .max_body_bytes(1024) // 让 e2e 用 1 KiB 就能试到 413
			                  .route_on(&app, &App::health, &App::user, &App::submit, &App::list_items,
			                            &App::create_item, &App::both, &App::both_head, &App::echo_body, &App::peek_body,
			                            &App::stream, &App::count_body)
		                  .build();
		return server.run();
	}
	catch(const std::exception& e)
	{
		fmt::print(stderr, "app: fatal: {}\n", e.what());
		return 1;
	}
}
