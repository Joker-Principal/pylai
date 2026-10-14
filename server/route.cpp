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

// 观测钩子的收尾：RAII ⇒ `dispatch` 的**每个出口**（含 404 / 405 / 早返回）都算“离开”。
struct DispatchGuard
{
	Observer* observer;
	http::Context* ctx;
	~DispatchGuard()
	{
		if(observer != nullptr) observer->on_request_end(*ctx);
	}
};

// 字节版；文本写法另给 `string_view` 重载（与 `res.body(…)` 同一个策略）。
// ⚠️ 状态住在 Context 里 ⇒ 必须**先 attach 再配置**（框架造响应一律这个次序）。
[[nodiscard]] StringResponse text_response(
	http::Context& ctx, http::ResponseWriter& writer, int status, std::span<const std::byte> body
)
{
	StringResponse response;
	response.attach(ctx, writer);
	response.status(status).body(body);
	return response;
}

[[nodiscard]] StringResponse text_response(
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

void Router::set_observer(Observer* observer) noexcept { observer_ = observer; }

asio::awaitable<void> Router::dispatch(
	http::Context& ctx, http::BodyReader& in, http::ResponseWriter& writer
) const
{
	// 观测钩子的**边界**：进入 / 离开各一次 —— 放在这里才能覆盖 404 / 405 / OPTIONS 这些没进 handler 的请求。
	// ⚠️ 无观察者时只剩一次可预测的分支（plan §4.1.4）。
	const DispatchGuard guard{observer_, &ctx};
	if(observer_ != nullptr) observer_->on_request_begin(ctx);

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
		EmptyResponse response;
		response.attach(ctx, writer);
		response.header("Allow", allow);
		co_await response.finish();
		co_return;
	}

	auto response = text_response(ctx, writer, 405, "405 Method Not Allowed\n");
	response.header("Allow", allow);
	co_await response.finish();
}

// ═══ 响应门面的出网实现 ═══
// 状态全在 `http::Context` 里；这里只做“把状态按 framing 策略发给写通道”。
// ⚠️ HEAD 的正文抑制（`head_only()`）与“头一发就冻结 keep_alive”都在写通道实现侧。

asio::awaitable<std::error_code> StringResponse::flush()
{
	auto& s = state();
	if(s.stage == http::Stage::finished) co_return std::error_code{};
	require_writer();

	if(s.stage == http::Stage::config)
	{
		const auto ec = co_await send_head(std::uint64_t{s.pending.size()}, false);
		if(ec) co_return ec;
	}

	// HEAD 照发头（`Content-Length` 按 GET 的体算），正文由这里抑制（RFC 9110 §9.3.2）。
	if(out_->head_only() || s.pending.empty())
	{
		s.stage = http::Stage::finished;
		co_return std::error_code{};
	}

	const auto ec = co_await out_->write(s.pending);
	if(!ec) s.stage = http::Stage::finished;
	co_return ec;
}

asio::awaitable<std::error_code> EmptyResponse::flush()
{
	auto& s = state();
	if(s.stage == http::Stage::finished) co_return std::error_code{};

	// 204 / 304 / 1xx 是“无内容”，不得带 Content-Length（RFC 9110 §8.6）；
	// 其它状态码只是“零长度内容”，必须写 Content-Length: 0 才能定界。
	const bool no_content = s.status == 204 || s.status == 304 || (s.status >= 100 && s.status < 200);
	const auto ec = co_await send_head(no_content ? std::nullopt : std::optional<std::uint64_t>{0}, false);
	if(!ec) s.stage = http::Stage::finished;
	co_return ec;
}

asio::awaitable<std::error_code> UnknownLengthResponse::flush()
{
	auto& s = state();
	if(s.stage == http::Stage::finished) co_return std::error_code{};
	require_writer();

	if(s.stage == http::Stage::config)
	{
		const auto ec = co_await send_head(std::nullopt, true);
		if(ec) co_return ec;
	}

	// 空载荷不产生块："0\r\n\r\n" 是终止块，不是“空块”—— 那是 finish() 的事。
	if(s.pending.empty() || out_->head_only()) co_return std::error_code{};

	const auto ec = co_await out_->write(http::chunk_bytes(s.pending));
	s.pending.clear();
	co_return ec;
}

asio::awaitable<std::error_code> UnknownLengthResponse::finish(std::span<const http::Header> trailers)
{
	auto& s = state();
	if(s.stage == http::Stage::finished) co_return std::error_code{};

	const auto flushed = co_await flush();
	if(flushed) co_return flushed;

	if(out_->head_only()) // HEAD：不应有体，也不发终止块
	{
		s.stage = http::Stage::finished;
		co_return std::error_code{};
	}

	const auto ec = co_await out_->write(http::last_chunk_bytes(trailers));
	if(!ec) s.stage = http::Stage::finished;
	co_return ec;
}

} // namespace pylai::route
