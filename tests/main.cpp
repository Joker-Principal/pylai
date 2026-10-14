#include <array>
#include <coroutine>
#include <cstdio>
#include <exception>
#include <string>

#include <fmt/format.h>

import asio; // handler 返回 asio::awaitable<void> ⇒ 消费方自己 import asio（框架不 export import 它）
import pylai;

namespace {

// 声明写法：动词 + 路径住在**第一个形参**（`Get<P>` 既是路由声明、也是请求类型），
// 响应按**引用**传进来：链式填状态，链尾的 `flush()` 才把它发出去（写由 handler 驱动）。
// 注册入口是 `Builder::route_on(&app, &App::...)`，整包一次挂上去。
using namespace pylai::route;

struct App
{
	asio::awaitable<void> health(Get<"/health"> req, pylai::http::StringResponse& res);
	asio::awaitable<void> user(Get<"/users/{id}"> req, pylai::http::StringResponse& res);
	asio::awaitable<void> submit(Post<"/submit/{id}"> req, pylai::http::StringResponse& res);

	// 同一条路径、两个动词：树按 pattern 建、动词不参与，所以这一组必须靠「每 method 一棵树」才不互相吃。
	asio::awaitable<void> list_items(Get<"/items"> req, pylai::http::StringResponse& res);
	asio::awaitable<void> create_item(Post<"/items"> req, pylai::http::StringResponse& res);

	// HEAD 的钉子：`/both` 同时有 GET 与显式 `Head<>`（显式优先）；`/health` 只有 GET → HEAD 复用 GET。
	asio::awaitable<void> both(Get<"/both"> req, pylai::http::StringResponse& res);
	asio::awaitable<void> both_head(Head<"/both"> req, pylai::http::StringResponse& res);

	// 请求体：框架读到 headers 就交棒，体由 handler 自己拉（`co_await req.body()`）。
	asio::awaitable<void> echo_body(Post<"/body"> req, pylai::http::StringResponse& res);
	// 只读一部分体就返回：e2e 用它验“未读体 ⇒ Connection: close”。
	asio::awaitable<void> peek_body(Post<"/peek"> req, pylai::http::StringResponse& res);

	// 长度未知的响应：handler 在**运行期间**分段推（响应侧 chunked），结束时补 trailer。
	asio::awaitable<void> stream(Get<"/stream"> req, pylai::http::UnknownLengthResponse& res);
};

asio::awaitable<void> App::health(Get<"/health">, pylai::http::StringResponse& res)
{
	co_await res.body("ok\n").flush(); // 链的最后一环 = flush：写到这一刻才碰 socket
}

asio::awaitable<void> App::user(Get<"/users/{id}"> req, pylai::http::StringResponse& res)
{
	co_await res.body(fmt::format("user id={}\n", req.param<int, "id">())).flush();
}

asio::awaitable<void> App::submit(Post<"/submit/{id}"> req, pylai::http::StringResponse& res)
{
	co_await res.body(fmt::format("submitted id={}\n", req.param<int, "id">())).flush();
}

asio::awaitable<void> App::list_items(Get<"/items">, pylai::http::StringResponse& res)
{
	co_await res.body("items\n").flush();
}

asio::awaitable<void> App::create_item(Post<"/items">, pylai::http::StringResponse& res)
{
	co_await res.body("created\n").flush();
}

asio::awaitable<void> App::both(Get<"/both">, pylai::http::StringResponse& res)
{
	co_await res.body("get\n").flush();
}

asio::awaitable<void> App::both_head(Head<"/both">, pylai::http::StringResponse& res)
{
	co_await res.body("explicit-head\n").flush(); // 长度与 GET 故意不同 → e2e 据此看出是谁应答的
}

// 请求体：框架读到 headers 就交棒，体由 handler 自己拉（`co_await req.body()`，受 max_body_bytes 约束）。
asio::awaitable<void> App::echo_body(Post<"/body"> req, pylai::http::StringResponse& res)
{
	const std::string body = co_await req.body();
	co_await res.body("body=" + body).flush();
}

// 只读一小段体就返回 —— e2e 用它验“未读体 ⇒ Connection: close”。
asio::awaitable<void> App::peek_body(Post<"/peek"> req, pylai::http::StringResponse& res)
{
	std::array<char, 4> head{};
	const std::size_t n = co_await req.read_some(head);
	co_await res.body("peek=" + std::string{head.data(), n}).flush();
}

// 长度未知：一块一块推 —— 每次 `flush()` 才把攒下的字节发出去（写由 handler 驱动）。
asio::awaitable<void> App::stream(Get<"/stream">, pylai::http::UnknownLengthResponse& res)
{
	// 预告 `Trailer: X-Chunks`（必须在发头之前）→ 头 + 第一块由链尾的 flush 一起发出去
	if(co_await res.trailer("X-Chunks").body("chunk-a\n").flush()) co_return;
	if(co_await res.body("chunk-b\n").flush()) co_return; // 第二块

	const std::array trailers{pylai::http::Header{"X-Chunks", "2"}};
	co_await res.finish(trailers); // 末块 + trailer
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
			                            &App::stream)
		                  .build();
		return server.run();
	}
	catch(const std::exception& e)
	{
		fmt::print(stderr, "app: fatal: {}\n", e.what());
		return 1;
	}
}
