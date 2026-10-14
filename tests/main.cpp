#include <array>
#include <coroutine>
#include <cstdio>
#include <exception>
#include <string>

import asio; // handler 返回 asio::awaitable<void> ⇒ 消费方自己 import asio（框架不 export import 它）
import pylai;

namespace {

// 声明写法：动词 + 路径住在**第一个形参**（`Get<P>` 既是路由声明、也是请求类型），
// 响应按**引用**传进来由 handler 填，handler 返回 `asio::awaitable<void>`。
// 注册入口是 `Builder::route_on(&app, &App::...)`，整包一次挂上去。
using namespace pylai::route;

struct App
{
	asio::awaitable<void> health(Get<"/health"> req, pylai::http::Response& res);
	asio::awaitable<void> user(Get<"/users/{id}"> req, pylai::http::Response& res);
	asio::awaitable<void> submit(Post<"/submit/{id}"> req, pylai::http::Response& res);

	// 同一条路径、两个动词：树按 pattern 建、动词不参与，所以这一组必须靠「每 method 一棵树」才不互相吃。
	asio::awaitable<void> list_items(Get<"/items"> req, pylai::http::Response& res);
	asio::awaitable<void> create_item(Post<"/items"> req, pylai::http::Response& res);

	// HEAD 的钉子：`/both` 同时有 GET 与显式 `Head<>`（显式优先）；`/health` 只有 GET → HEAD 复用 GET。
	asio::awaitable<void> both(Get<"/both"> req, pylai::http::Response& res);
	asio::awaitable<void> both_head(Head<"/both"> req, pylai::http::Response& res);

	// 请求体：框架读到 headers 就交棒，体由 handler 自己拉（`co_await req.body()`）。
	asio::awaitable<void> echo_body(Post<"/body"> req, pylai::http::Response& res);
	// 只读一部分体就返回：e2e 用它验“未读体 ⇒ Connection: close”。
	asio::awaitable<void> peek_body(Post<"/peek"> req, pylai::http::Response& res);
};

asio::awaitable<void> App::health(Get<"/health">, pylai::http::Response& res)
{
	res.body = "ok\n";
	co_return;
}

asio::awaitable<void> App::user(Get<"/users/{id}"> req, pylai::http::Response& res)
{
	res.body = "user id=" + std::to_string(req.param<int, "id">()) + "\n";
	co_return;
}

asio::awaitable<void> App::submit(Post<"/submit/{id}"> req, pylai::http::Response& res)
{
	res.body = "submitted id=" + std::to_string(req.param<int, "id">()) + "\n";
	co_return;
}

asio::awaitable<void> App::list_items(Get<"/items">, pylai::http::Response& res)
{
	res.body = "items\n";
	co_return;
}

asio::awaitable<void> App::create_item(Post<"/items">, pylai::http::Response& res)
{
	res.body = "created\n";
	co_return;
}

asio::awaitable<void> App::both(Get<"/both">, pylai::http::Response& res)
{
	res.body = "get\n";
	co_return;
}

asio::awaitable<void> App::both_head(Head<"/both">, pylai::http::Response& res)
{
	res.body = "explicit-head\n"; // 长度与 GET 故意不同 → e2e 据此看出是谁应答的
	co_return;
}

// 请求体：框架读到 headers 就交棒，体由 handler 自己拉（`co_await req.body()`，受 max_body_bytes 约束）。
asio::awaitable<void> App::echo_body(Post<"/body"> req, pylai::http::Response& res)
{
	const std::string body = co_await req.body();
	res.body = "body=" + body;
	co_return;
}

// 只读一小段体就返回 —— e2e 用它验“未读体 ⇒ Connection: close”。
asio::awaitable<void> App::peek_body(Post<"/peek"> req, pylai::http::Response& res)
{
	std::array<char, 4> head{};
	const std::size_t n = co_await req.read_some(head);
	res.body = "peek=" + std::string{head.data(), n};
	co_return;
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
			                            &App::create_item, &App::both, &App::both_head, &App::echo_body, &App::peek_body)
		                  .build();
		return server.run();
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "app: fatal: %s\n", e.what());
		return 1;
	}
}
