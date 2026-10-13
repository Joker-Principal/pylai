module;

#include <coroutine>
#include <cstdint>
#include <string>
#include <string_view>
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

// 一条 path 上已注册的动词（按注册顺序，逗号分隔），给 405 / OPTIONS 的 `Allow` 用。
// 有 GET 而没有显式 HEAD 时补上 HEAD —— 它是 GET 的隐含搭档（RFC 9110 §9.3.2）。
[[nodiscard]] std::string allowed_methods(const std::vector<VerbRoutes>& methods, std::string_view path)
{
	std::vector<std::string_view> verbs;
	std::vector<Capture> scratch;
	bool has_head = false;

	for(const VerbRoutes& route : methods)
	{
		if(route.match(path, scratch) == k_no_route) continue;

		verbs.push_back(route.verb);
		has_head = has_head || route.verb == "HEAD";
	}

	std::string out;
	for(const std::string_view verb : verbs)
	{
		if(!out.empty()) out += ", ";
		out += verb;
		if(verb == "GET" && !has_head) out += ", HEAD";
	}
	return out;
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

	// 没写 `Head<>` 时，HEAD 落到同路径的 GET 上（正文由 serialize() 按 HEAD 抑制）。
	if(request.method == "HEAD")
	{
		for(const VerbRoutes& route : methods)
		{
			if(route.verb != "GET") continue;

			const std::uint32_t slot = route.match(request.path, captures);
			if(slot < route.handlers.size()) co_return co_await route.handlers[slot](request);
		}
	}

	// 该路径在别的动词上存在吗？有 → 405 + `Allow`；OPTIONS 有 → 自动应答。
	const std::string allow = allowed_methods(methods, request.path);
	if(allow.empty()) co_return text_response(404, "404 Not Found\n");

	if(request.method == "OPTIONS")
	{
		http::Response response;
		response.status = 204;
		response.content_type.clear(); // 204 无体，别声称有
		response.headers.push_back({"Allow", allow});
		co_return response;
	}

	http::Response response = text_response(405, "405 Method Not Allowed\n");
	response.headers.push_back({"Allow", allow});
	co_return response;
}

} // namespace pylai::route
