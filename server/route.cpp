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

// 字节版；文本写法另给 `string_view` 重载（与 `res.body(…)` 同一个策略）。
// ⚠️ 状态住在 Context 里 ⇒ 必须**先 attach 再配置**（框架造响应一律这个次序）。
[[nodiscard]] http::StringResponse text_response(
	http::Context& ctx, http::ResponseWriter& writer, int status, std::span<const std::byte> body
)
{
	http::StringResponse response;
	response.attach(ctx, writer);
	response.status(status).body(body);
	return response;
}

[[nodiscard]] http::StringResponse text_response(
	http::Context& ctx, http::ResponseWriter& writer, int status, std::string_view body
)
{
	return text_response(ctx, writer, status, std::as_bytes(std::span{body}));
}

// 一条 path 上已注册的动词（按注册顺序，逗号分隔），给 405 / OPTIONS 的 `Allow` 用。
// 有 GET 而没有显式 HEAD 时补上 HEAD —— 它是 GET 的隐含搭档（RFC 9110 §9.3.2）。
[[nodiscard]] std::string allowed_methods(const std::vector<VerbRoutes>& methods, std::string_view path)
{
	std::vector<std::string_view> verbs;
	bool has_head = false;

	for(const VerbRoutes& route : methods)
	{
		if(!route.matched(path)) continue;

		if(route.verb == "HEAD") has_head = true;
		verbs.push_back(route.verb);
	}

	// 只有看完整条清单才知道有没有显式 `Head<>` ⇒ 补 HEAD 放在摊平之后。
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

asio::awaitable<void> Router::dispatch(
	http::Context& ctx, http::BodyReader& in, http::ResponseWriter& writer
) const
{
	const http::Request& request = ctx.request();

	// 快路径：只试请求动词那个单元（匹配与调用在同一处，命中后捕获直接交给 handler）。
	for(const VerbRoutes& route : methods)
	{
		if(route.verb != request.method) continue;
		if(co_await route.invoke(ctx, in, writer)) co_return;
	}

	// 没写 `Head<>` 时，HEAD 落到同路径的 GET 上（正文由响应类型按 HEAD 抑制）。
	if(request.method == "HEAD")
	{
		for(const VerbRoutes& route : methods)
		{
			if(route.verb != "GET") continue;
			if(co_await route.invoke(ctx, in, writer)) co_return;
		}
	}

	// 该路径在别的动词上存在吗？有 → 405 + `Allow`；OPTIONS 有 → 自动应答。
	const std::string allow = allowed_methods(methods, request.path);
	if(allow.empty())
	{
		auto response = text_response(ctx, writer, 404, "404 Not Found\n");
		co_await response.finish();
		co_return;
	}

	if(request.method == "OPTIONS")
	{
		http::EmptyResponse response;
		response.attach(ctx, writer);
		response.header("Allow", allow);
		co_await response.finish();
		co_return;
	}

	auto response = text_response(ctx, writer, 405, "405 Method Not Allowed\n");
	response.header("Allow", allow);
	co_await response.finish();
}

} // namespace pylai::route
