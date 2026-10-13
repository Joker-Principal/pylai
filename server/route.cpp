module;

#include <coroutine>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

module pylai;

import asio;
import :http;
import :route;

namespace pylai::route {

namespace {

http::Response text_response(int status, std::string body)
{
	http::Response response;
	response.status = status;
	response.body = std::move(body);
	return response;
}

} // namespace

Router::Router() = default;
Router::~Router() = default;

void Router::set_routes(std::vector<VerbRoutes> routes) { methods = std::move(routes); }

asio::awaitable<http::Response> Router::dispatch(const http::Request& request) const
{
	std::vector<Capture> captures;

	// 快路径：只试请求动词那棵树。
	for(const VerbRoutes& route : methods)
	{
		if(route.verb != request.method) continue;

		const std::uint32_t slot = route.match(request.path, captures);
		if(slot < route.handlers.size()) co_return co_await route.handlers[slot](request);
	}

	// 该路径在别的动词上存在吗？有 → 405，没有 → 404。
	for(const VerbRoutes& route : methods)
		if(route.match(request.path, captures) != k_no_route) co_return text_response(405, "405 Method Not Allowed\n");

	co_return text_response(404, "404 Not Found\n");
}

} // namespace pylai::route
