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
		Builder& bind(std::string address, std::uint16_t port);
		Builder& io_threads(std::size_t count);
		Builder& idle_timeout(std::chrono::seconds timeout);
		Builder& write_timeout(std::chrono::seconds timeout);
		Builder& max_requests_per_connection(std::size_t count);

		// 声明式注册：`builder.route_on(this, &User::health, &User::submit)`。
		// 动词 / 路径 / 形参表全从成员指针取（返回类型是 route::Endpoint<...>，形参里的 Param<> 按位置吃捕获）。
		// 整包做成**一组**（一棵编译期树 + 槽位表），因为树是定容的、运行期加不了单条路由。
		template <class C, class... Pmf>
		Builder& route_on(C* self, Pmf... pmf)
		{
			(route::validate_route<Pmf>(), ...); // 编译期校验：Param 个数 / 名字 / pattern 合法性
			routes.add_group(route::make_group(self, pmf...));
			return *this;
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
