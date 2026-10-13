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

void Router::add_group(RouteGroup&& group) { groups.push_back(std::move(group)); }

asio::awaitable<http::Response> Router::dispatch(const http::Request& request) const
{
	// 一组一棵树；路径命中但动词不符 → 先记下 405，继续找（别组可能同路径带该动词）
	std::vector<Capture> captures;
	bool path_known = false;

	for(const RouteGroup& group : groups)
	{
		const std::uint32_t slot = group.match(request.path, captures);
		if(slot == k_no_route) continue;

		path_known = true;
		if(slot < group.handlers.size() && group.verbs[slot] == request.method)
			co_return co_await group.handlers[slot](request);
	}

	if(path_known) co_return text_response(405, "405 Method Not Allowed\n");
	co_return text_response(404, "404 Not Found\n");
}

} // namespace pylai::route
