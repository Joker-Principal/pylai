module;

#include <algorithm>
#include <coroutine>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

module pylai;

import asio;
import :http;
import :route;

namespace pylai::route {

namespace {

// 字节版（体本来是字节）；文本写法另给一个 `string_view` 重载 —— 与 `res.body(...)` 同一个策略。
[[nodiscard]] http::StringResponse text_response(int status, std::span<const std::byte> body)
{
	http::StringResponse response;
	response.status(status).body(body);
	return response;
}

[[nodiscard]] http::StringResponse text_response(int status, std::string_view body)
{
	return text_response(status, std::as_bytes(std::span{body}));
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

		if(route.verb == "HEAD") has_head = true;
		// 先按注册顺序摊平，最后一次性 join —— HEAD 得紧跟在 GET 后面。
		verbs.push_back(route.verb);
	}

	// 补 HEAD：GET 的隐含搭档，位置紧跟 GET（RFC 9110 §9.3.2）。放在了摊平之后，
	// 是因为只有看完整条清单才知道有没有显式 `Head<>`。
	if(!has_head)
	{
		const auto get = std::find(verbs.begin(), verbs.end(), "GET");
		if(get != verbs.end()) verbs.insert(get + 1, "HEAD");
	}

	return fmt::format("{}", fmt::join(verbs, ", "));
}

} // namespace

Router::Router() = default;
Router::~Router() = default;

void Router::set_routes(std::vector<VerbRoutes> routes) { methods = std::move(routes); }

asio::awaitable<void> Router::dispatch(const http::Request& request, http::ResponseWriter& writer) const
{
	std::vector<Capture> captures;

	// 快路径：只试请求动词那棵树。
	for(const VerbRoutes& route : methods)
	{
		if(route.verb != request.method) continue;

		const std::uint32_t slot = route.match(request.path, captures);
		if(slot < route.handlers.size())
		{
			co_await route.handlers[slot](request, writer);
			co_return;
		}
	}

	// 没写 `Head<>` 时，HEAD 落到同路径的 GET 上（正文由响应类型按 HEAD 抑制）。
	if(request.method == "HEAD")
	{
		for(const VerbRoutes& route : methods)
		{
			if(route.verb != "GET") continue;

			const std::uint32_t slot = route.match(request.path, captures);
			if(slot < route.handlers.size())
			{
				co_await route.handlers[slot](request, writer);
				co_return;
			}
		}
	}

	// 该路径在别的动词上存在吗？有 → 405 + `Allow`；OPTIONS 有 → 自动应答。
	const std::string allow = allowed_methods(methods, request.path);
	if(allow.empty())
	{
		auto response = text_response(404, "404 Not Found\n");
		response.attach(writer);
		co_await response.finish();
		co_return;
	}

	if(request.method == "OPTIONS")
	{
		http::EmptyResponse response;
		response.attach(writer);
		response.header("Allow", allow);
		co_await response.finish();
		co_return;
	}

	auto response = text_response(405, "405 Method Not Allowed\n");
	response.header("Allow", allow);
	response.attach(writer);
	co_await response.finish();
}

} // namespace pylai::route
