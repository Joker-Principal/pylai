module;

#include <algorithm>
#include <array>
#include <charconv>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llhttp.h>

module pylai;

import :http;

namespace pylai::http {

namespace {

std::string_view status_text(int status)
{
	switch(status)
	{
		case 200: return "OK";
		case 201: return "Created";
		case 204: return "No Content";
		case 301: return "Moved Permanently";
		case 302: return "Found";
		case 304: return "Not Modified";
		case 400: return "Bad Request";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 408: return "Request Timeout";
		case 413: return "Content Too Large";
		case 431: return "Request Header Fields Too Large";
		case 500: return "Internal Server Error";
		case 501: return "Not Implemented";
		case 503: return "Service Unavailable";
		default: return "";
	}
}

} // namespace

struct Parser::Impl
{
	llhttp_t handle{};
	Request request;
	std::size_t max_body_bytes{0};
	std::string pending_field;
	std::string pending_value;
	std::string reason;

	// 体阶段的交付去处（由 read_body 每次设好；载荷不缓存在这里）。
	std::span<char> sink{};
	std::size_t delivered{0};

	bool paused_on_headers{false};
	bool paused_on_message{false};
	bool over_limit{false};
	bool finished{false};

	static Impl* self(llhttp_t* parser) noexcept { return static_cast<Impl*>(parser->data); }

	static int on_message_begin(llhttp_t* parser)
	{
		auto* owner = self(parser);
		owner->request = Request{};
		owner->finished = false;
		owner->over_limit = false;
		return 0;
	}

	static int on_url(llhttp_t* parser, const char* at, std::size_t length)
	{
		self(parser)->request.target.append(at, length);
		return 0;
	}

	static int on_header_field(llhttp_t* parser, const char* at, std::size_t length)
	{
		self(parser)->pending_field.append(at, length);
		return 0;
	}

	static int on_header_value(llhttp_t* parser, const char* at, std::size_t length)
	{
		self(parser)->pending_value.append(at, length);
		return 0;
	}

	static int on_header_value_complete(llhttp_t* parser)
	{
		auto* owner = self(parser);
		owner->request.headers.push_back({std::move(owner->pending_field), std::move(owner->pending_value)});
		owner->pending_field.clear();
		owner->pending_value.clear();
		return 0;
	}

	// 头部阶段在这里停：**keep_alive 与 path/query 都搬到这里算**（依据见 docs/http-server-plan.md §4.1.2 E）。
	static int on_headers_complete(llhttp_t* parser)
	{
		auto* owner = self(parser);
		auto& req = owner->request;

		req.method = llhttp_method_name(static_cast<llhttp_method_t>(parser->method));
		req.version =
			std::string{"HTTP/"} + std::to_string(parser->http_major) + "." + std::to_string(parser->http_minor);
		req.content_length = parser->content_length;
		req.has_content_length = (parser->flags & F_CONTENT_LENGTH) != 0;
		req.chunked = (parser->flags & F_CHUNKED) != 0;

		// keep-alive：对请求而言它只看 Connection 头与版本，两者此时都已定型。
		req.keep_alive = llhttp_should_keep_alive(parser) != 0;

		// target 到这时已完整（on_url 可能被分多次调用）。
		const auto& target = req.target;
		const auto separator = target.find('?');
		if(separator == std::string::npos)
		{
			req.path = target;
		}
		else
		{
			req.path = target.substr(0, separator);
			req.query = target.substr(separator + 1);
			req.has_query = true;
		}

		// 声明的长度直接超上限 ⇒ 不进路由（比“边收边判”更早）。
		if(req.has_content_length && req.content_length > owner->max_body_bytes)
		{
			owner->over_limit = true;
			return HPE_USER;
		}

		owner->paused_on_headers = true;
		return HPE_PAUSED; // 把控制权交回 execute()，框架在此交棒
	}

	// chunked 的块边界由 llhttp 剥掉，这里拿到的是**载荷** —— 直接写进 reader 给的缓冲。
	static int on_body(llhttp_t* parser, const char* at, std::size_t length)
	{
		auto* owner = self(parser);
		if(length > owner->sink.size() - owner->delivered) // 窗口已夹到 sink 大小，理论上不会发生
		{
			owner->reason = "internal: body chunk larger than the reader buffer";
			return HPE_USER;
		}

		std::memcpy(owner->sink.data() + owner->delivered, at, length);
		owner->delivered += length;
		return 0;
	}

	static int on_message_complete(llhttp_t* parser)
	{
		auto* owner = self(parser);
		owner->finished = true;
		owner->paused_on_message = true;
		return HPE_PAUSED; // 停在下一个请求之前
	}

	static const llhttp_settings_t& settings()
	{
		static const llhttp_settings_t value = [] {
			llhttp_settings_t s;
			llhttp_settings_init(&s);
			s.on_message_begin = &on_message_begin;
			s.on_url = &on_url;
			s.on_header_field = &on_header_field;
			s.on_header_value = &on_header_value;
			s.on_header_value_complete = &on_header_value_complete;
			s.on_headers_complete = &on_headers_complete;
			s.on_body = &on_body;
			s.on_message_complete = &on_message_complete;
			return s;
		}();
		return value;
	}
};

Parser::Parser(std::size_t max_body_bytes) : impl(std::make_unique<Impl>())
{
	impl->max_body_bytes = max_body_bytes;
	llhttp_init(&impl->handle, HTTP_REQUEST, &Impl::settings());
	impl->handle.data = impl.get();
}

Parser::~Parser() = default;
Parser::Parser(Parser&&) noexcept = default;
Parser& Parser::operator=(Parser&&) noexcept = default;

ParseStatus Parser::execute(const char* data, std::size_t length, std::size_t& consumed)
{
	impl->paused_on_headers = false;
	impl->paused_on_message = false;
	impl->sink = {};
	impl->delivered = 0;

	consumed = 0;
	const llhttp_errno_t err = llhttp_execute(&impl->handle, data, length);

	if(err == HPE_OK) return ParseStatus::incomplete; // 输入耗尽，但消息还没完

	if(impl->over_limit)
	{
		// 头部阶段就发现声明的长度超限：on_headers_complete 主动中断了。
		// ⚠️ 这时 llhttp 报的是 `HPE_CB_headers_complete`（不是 HPE_USER），所以按标志位判。
		impl->reason = "request body exceeds the configured limit";
		return ParseStatus::too_large;
	}

	if(err == HPE_PAUSED)
	{
		// PAUSED 时 error_pos 指向停下的位置（实测），相减即本次消费掉的字节数。
		const char* position = llhttp_get_error_pos(&impl->handle);
		consumed = position != nullptr ? static_cast<std::size_t>(position - data) : length;
		return impl->paused_on_message ? ParseStatus::message_complete : ParseStatus::headers_complete;
	}

	impl->reason = llhttp_get_error_reason(&impl->handle);
	return ParseStatus::error;
}

Parser::BodyStep Parser::read_body(const char* data, std::size_t length, std::span<char> out)
{
	BodyStep step;
	impl->sink = out;
	impl->delivered = 0;
	impl->paused_on_headers = false;
	impl->paused_on_message = false;

	// 夹到 out.size()：llhttp 交给 on_body 的载荷是输入的子区间 ⇒ 保证它一定放得下。
	const std::size_t window = std::min(length, out.size());
	const llhttp_errno_t err = llhttp_execute(&impl->handle, data, window);
	step.delivered = impl->delivered;
	impl->sink = {};

	if(err == HPE_OK)
	{
		step.consumed = window;
		return step;
	}

	if(err == HPE_PAUSED)
	{
		// 体阶段只会在 message_complete 处停（下一个请求的头部不会再进到本次调用里）。
		const char* position = llhttp_get_error_pos(&impl->handle);
		step.consumed = position != nullptr ? static_cast<std::size_t>(position - data) : window;
		step.finished = impl->paused_on_message;
		return step;
	}

	step.failed = true;
	impl->reason = (err == HPE_USER) ? impl->reason : std::string{llhttp_get_error_reason(&impl->handle)};
	return step;
}

void Parser::resume() { llhttp_resume(&impl->handle); }

const Request& Parser::current() const { return impl->request; }
Request& Parser::current() { return impl->request; }

std::string_view Parser::error_reason() const { return impl->reason; }

asio::awaitable<std::string> Request::body() const
{
	std::string out;
	if(body_reader == nullptr) co_return out;
	if(has_content_length) out.reserve(static_cast<std::size_t>(content_length));

	std::array<char, 8 * 1024> chunk{};
	for(;;)
	{
		const std::size_t n = co_await body_reader->read_some(chunk);
		if(n == 0) break;
		out.append(chunk.data(), n);
	}

	co_return out;
}

asio::awaitable<std::size_t> Request::read_some(std::span<char> out) const
{
	if(body_reader == nullptr) co_return 0;
	co_return co_await body_reader->read_some(out);
}

namespace {

void append_header(std::string& out, std::string_view name, std::string_view value)
{
	out += name;
	out += ": ";
	out += value;
	out += "\r\n";
}

// 16 进制块长（小写）+ CRLF + 载荷 + CRLF（RFC 9112 §7.1）。
void append_chunk(std::string& out, std::string_view data)
{
	char size[16];
	const auto result = std::to_chars(size, size + sizeof(size), data.size(), 16);
	out.append(size, static_cast<std::size_t>(result.ptr - size));
	out += "\r\n";
	out += data;
	out += "\r\n";
}

} // namespace

std::string head_bytes(
	int status,
	std::string_view content_type,
	std::span<const Header> headers,
	std::optional<std::uint64_t> content_length,
	bool chunked,
	bool keep_alive,
	std::span<const Header> trailer_names
)
{
	std::string out;
	out.reserve(256);

	out += "HTTP/1.1 ";
	out += std::to_string(status);
	out += ' ';
	out += status_text(status);
	out += "\r\n";

	if(!content_type.empty()) append_header(out, "Content-Type", content_type);
	if(content_length) append_header(out, "Content-Length", std::to_string(*content_length));
	if(chunked) append_header(out, "Transfer-Encoding", "chunked");

	if(!trailer_names.empty())
	{
		out += "Trailer: ";
		for(std::size_t i = 0; i < trailer_names.size(); ++i)
		{
			if(i != 0) out += ", ";
			out += trailer_names[i].name;
		}
		out += "\r\n";
	}

	for(const auto& header : headers) append_header(out, header.name, header.value);
	append_header(out, "Connection", keep_alive ? "keep-alive" : "close");

	out += "\r\n"; // 头部结束
	return out;
}

std::string chunk_bytes(std::string_view data)
{
	std::string out;
	out.reserve(data.size() + 16);
	append_chunk(out, data);
	return out;
}

std::string last_chunk_bytes(std::span<const Header> trailers)
{
	std::string out;
	out.reserve(64);
	append_chunk(out, ""); // 终止块：0 CRLF
	for(const auto& trailer : trailers) append_header(out, trailer.name, trailer.value);
	out += "\r\n"; // trailer 段结束（无 trailer 时就是终止块后那个空行）
	return out;
}

namespace {

class WriteCategoryImpl : public std::error_category
{
public:
	[[nodiscard]] const char* name() const noexcept override { return "pylai.http.write"; }

	[[nodiscard]] std::string message(int code) const override
	{
		switch(static_cast<WriteErrc>(code))
		{
			case WriteErrc::closed: return "connection closed";
			case WriteErrc::timed_out: return "write timed out";
		}
		return "unknown write error";
	}
};

} // namespace

const std::error_category& write_category() noexcept
{
	static const WriteCategoryImpl instance;
	return instance;
}

std::error_code make_error_code(WriteErrc value) noexcept
{
	return {static_cast<int>(value), write_category()};
}

void throw_out_of_order(std::string_view action)
{
	throw std::logic_error{
		"响应写入顺序错误：`" + std::string{action} + "` 不能在响应头发出（或收尾）之后再调用"
	};
}

asio::awaitable<std::error_code> StringResponse::flush()
{
	if(stage_ == Stage::finished) co_return std::error_code{};
	require_writer();

	if(stage_ == Stage::config)
	{
		const auto ec = co_await send_head(std::uint64_t{pending_.size()}, false);
		if(ec) co_return ec;
	}

	// HEAD 照发头（`Content-Length` 按 GET 的体算），正文由这里抑制（RFC 9110 §9.3.2）。
	if(out_->head_only() || pending_.empty())
	{
		stage_ = Stage::finished;
		co_return std::error_code{};
	}

	const auto ec = co_await out_->write(pending_);
	if(!ec) stage_ = Stage::finished;
	co_return ec;
}

asio::awaitable<std::error_code> EmptyResponse::flush()
{
	if(stage_ == Stage::finished) co_return std::error_code{};

	// 204 / 304 / 1xx 是“无内容”，不得带 Content-Length（RFC 9110 §8.6）；
	// 其它状态码只是“零长度内容”，必须写 Content-Length: 0 才能定界。
	const bool no_content = status_ == 204 || status_ == 304 || (status_ >= 100 && status_ < 200);
	const auto ec = co_await send_head(no_content ? std::nullopt : std::optional<std::uint64_t>{0}, false);
	if(!ec) stage_ = Stage::finished;
	co_return ec;
}

asio::awaitable<std::error_code> UnknownLengthResponse::flush()
{
	if(stage_ == Stage::finished) co_return std::error_code{};
	require_writer();

	if(stage_ == Stage::config)
	{
		const auto ec = co_await send_head(std::nullopt, true);
		if(ec) co_return ec;
	}

	// 空载荷不产生块："0\r\n\r\n" 是终止块，不是“空块”—— 那是 finish() 的事。
	if(pending_.empty() || out_->head_only()) co_return std::error_code{};

	const auto ec = co_await out_->write(chunk_bytes(pending_));
	pending_.clear();
	co_return ec;
}

asio::awaitable<std::error_code> UnknownLengthResponse::finish(std::span<const Header> trailers)
{
	if(stage_ == Stage::finished) co_return std::error_code{};

	const auto flushed = co_await flush();
	if(flushed) co_return flushed;

	if(out_->head_only()) // HEAD：不应有体，也不发终止块
	{
		stage_ = Stage::finished;
		co_return std::error_code{};
	}

	const auto ec = co_await out_->write(last_chunk_bytes(trailers));
	if(!ec) stage_ = Stage::finished;
	co_return ec;
}

} // namespace pylai::http
