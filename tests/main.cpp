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

} // namespace

int main()
{
	try
	{
		App app; // 必须比 server 活得久：路由里存的是 &app
		auto server = pylai::Server::builder()
		                  .bind("127.0.0.1", 18080)
		                  .route_on(&app, &App::health, &App::user, &App::submit, &App::list_items,
		                            &App::create_item)
		                  .build();
		return server.run();
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "app: fatal: %s\n", e.what());
		return 1;
	}
}
