#include <coroutine>
#include <cstdio>
#include <exception>
#include <string>

import pylai;

namespace {

// 位置注入写法：动词 + 路径住在**返回类型**位置，`Param<T[, name]>` 按位置吃第 N 个命名捕获。
// 注册入口是 `Builder::route_on(&app, &App::...)`，整包一次挂上去。
using namespace pylai::route;

struct App
{
	Get<"/health"> health(const pylai::http::Request& request);
	Get<"/users/{id}"> user(Param<int, "id"> id, const pylai::http::Request& request);
	Post<"/submit/{id}"> submit(Param<int, "id"> id, const pylai::http::Request& request);

	// 同一条路径、两个动词：树按 pattern 建、动词不参与，所以这一组必须靠「每 method 一棵树」才不互相吃。
	Get<"/items"> list_items(const pylai::http::Request& request);
	Post<"/items"> create_item(const pylai::http::Request& request);

	// HEAD 的钉子：`/both` 同时有 GET 与显式 `Head<>`（显式优先）；`/health` 只有 GET → HEAD 复用 GET。
	Get<"/both"> both(const pylai::http::Request& request);
	Head<"/both"> both_head(const pylai::http::Request& request);
};

Get<"/health"> App::health(const pylai::http::Request&)
{
	pylai::http::Response response;
	response.body = "ok\n";
	co_return response;
}

Get<"/users/{id}"> App::user(Param<int, "id"> id, const pylai::http::Request&)
{
	pylai::http::Response response;
	response.body = "user id=" + std::to_string(id.value) + "\n";
	co_return response;
}

Post<"/submit/{id}"> App::submit(Param<int, "id"> id, const pylai::http::Request&)
{
	pylai::http::Response response;
	response.body = "submitted id=" + std::to_string(id.value) + "\n";
	co_return response;
}

Get<"/items"> App::list_items(const pylai::http::Request&)
{
	pylai::http::Response response;
	response.body = "items\n";
	co_return response;
}

Post<"/items"> App::create_item(const pylai::http::Request&)
{
	pylai::http::Response response;
	response.body = "created\n";
	co_return response;
}

Get<"/both"> App::both(const pylai::http::Request&)
{
	pylai::http::Response response;
	response.body = "get\n";
	co_return response;
}

Head<"/both"> App::both_head(const pylai::http::Request&)
{
	pylai::http::Response response;
	response.body = "explicit-head\n"; // 长度与 GET 故意不同 → e2e 据此看出是谁应答的
	co_return response;
}

} // namespace

int main()
{
	try
	{
		App app; // 必须比 server 活得久：路由里存的是 &app
		auto server = pylai::Server::builder()
		                  .bind("127.0.0.1", 18080)
		                  .route_on(&app, &App::health, &App::user, &App::submit, &App::list_items,
			                            &App::create_item, &App::both, &App::both_head)
		                  .build();
		return server.run();
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "app: fatal: %s\n", e.what());
		return 1;
	}
}
