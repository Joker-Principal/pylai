module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <plf_hive.h>
#include <string>

export module pylai:server;

import asio;
import :config;
import :http;
import :route;

export namespace pylai {

class Server
{
public:
	class Builder
	{
	public:
		// 路由注册完之后只剩 `build()` —— 于是“第二个 route_on”根本编不过，
		// 跨组路由冲突也就不存在了。设置项要写在 route_on 之前。
		class Configured
		{
		public:
			Server build();

		private:
			friend class Builder;

			Configured(Settings settings, route::Router routes) :
				settings_(std::move(settings)),
				routes_(std::move(routes))
			{
			}

			Settings settings_;
			route::Router routes_;
		};

		Builder& bind(std::string address, std::uint16_t port);
		Builder& io_threads(std::size_t count);
		Builder& idle_timeout(std::chrono::seconds timeout);
		Builder& write_timeout(std::chrono::seconds timeout);
		Builder& max_requests_per_connection(std::size_t count);

		// 声明式注册：`builder.bind(...).route_on(&app, &App::health, &App::submit).build()`。
		// 动词 / 路径 / 形参表全从成员指针取（返回类型是 route::Endpoint<...>，形参里的 Param<> 按位置吃捕获）。
		template <class C, class... Pmf>
		[[nodiscard]] Configured route_on(C* self, Pmf... pmf)
		{
			(route::validate_route<Pmf>(), ...); // 单条：Param 个数 / 名字 / pattern 合法性
			route::validate_group<Pmf...>();	 // 组内：同一 method 下不得有两条路由争抢同一路径
			routes.set_routes(route::make_routes(self, pmf...));
			return Configured{std::move(settings), std::move(routes)};
		}

		Server build();

	private:
		Settings settings;
		route::Router routes;
	};

	static Builder builder();

	~Server();

	Server(const Server&) = delete;
	Server& operator=(const Server&) = delete;

	// 跑事件循环，直到 quit() 或 accept 循环结束。
	int run();

	// 请求停机：取消 accept 与全部在途会话。必须在 Server 所属线程上调用。
	void quit();

private:
	Server(Settings settings, route::Router routes);

	asio::io_context context;
	Settings settings;
	route::Router routes;

	asio::cancellation_signal accept_signal;
	plf::hive<asio::cancellation_signal> sessions;

	asio::awaitable<void> accept_loop();
};
} // namespace pylai
