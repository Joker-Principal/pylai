module;

#include <memory>
#include <string>
#include <string_view>
#include <utility>

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
	std::string pending_field;
	std::string pending_value;
	std::string reason;

	static Impl* self(llhttp_t* parser) noexcept { return static_cast<Impl*>(parser->data); }

	static int on_message_begin(llhttp_t* parser)
	{
		self(parser)->request = Request{};
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

	static int on_headers_complete(llhttp_t* parser)
	{
		auto* owner = self(parser);
		owner->request.method = llhttp_method_name(static_cast<llhttp_method_t>(parser->method));
		owner->request.version =
			std::string{"HTTP/"} + std::to_string(parser->http_major) + "." + std::to_string(parser->http_minor);
		owner->request.content_length = parser->content_length;
		owner->request.has_content_length = (parser->flags & F_CONTENT_LENGTH) != 0;
		owner->request.chunked = (parser->flags & F_CHUNKED) != 0;
		return 0;
	}

	static int on_message_complete(llhttp_t* parser)
	{
		auto* owner = self(parser);
		// keep-alive 的判定要等整个消息走完才可靠（Connection 头 + 版本 + 定界方式）。
		owner->request.keep_alive = llhttp_should_keep_alive(parser) != 0;

		// target 到这时才完整（on_url 可能被分多次调用）。
		const auto& target = owner->request.target;
		const auto separator = target.find('?');
		if(separator == std::string::npos)
		{
			owner->request.path = target;
		}
		else
		{
			owner->request.path = target.substr(0, separator);
			owner->request.query = target.substr(separator + 1);
			owner->request.has_query = true;
		}

		return HPE_PAUSED; // 停下来，把控制权交回 execute() 的调用方
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
			s.on_message_complete = &on_message_complete;
			return s;
		}();
		return value;
	}
};

Parser::Parser() : impl(std::make_unique<Impl>())
{
	llhttp_init(&impl->handle, HTTP_REQUEST, &Impl::settings());
	impl->handle.data = impl.get();
}

Parser::~Parser() = default;
Parser::Parser(Parser&&) noexcept = default;
Parser& Parser::operator=(Parser&&) noexcept = default;

ParseStatus Parser::execute(const char* data, std::size_t length, std::size_t& consumed)
{
	consumed = 0;
	const llhttp_errno_t err = llhttp_execute(&impl->handle, data, length);

	if(err == HPE_OK) return ParseStatus::incomplete; // 输入耗尽，但消息还没完

	if(err == HPE_PAUSED)
	{
		// PAUSED 时 error_pos 指向消息结束后的位置（实测），相减即本请求字节数。
		const char* position = llhttp_get_error_pos(&impl->handle);
		consumed = position != nullptr ? static_cast<std::size_t>(position - data) : length;
		return ParseStatus::complete;
	}

	impl->reason = llhttp_get_error_reason(&impl->handle);
	return ParseStatus::error;
}

void Parser::resume() { llhttp_resume(&impl->handle); }

const Request& Parser::current() const { return impl->request; }

std::string_view Parser::error_reason() const { return impl->reason; }

std::string serialize(const Response& response, bool keep_alive)
{
	std::string out;
	out.reserve(response.body.size() + 128);

	out += "HTTP/1.1 ";
	out += std::to_string(response.status);
	out += ' ';
	out += status_text(response.status);
	out += "\r\n";

	if(!response.content_type.empty())
	{
		out += "Content-Type: ";
		out += response.content_type;
		out += "\r\n";
	}

	for(const auto& header : response.headers)
	{
		out += header.name;
		out += ": ";
		out += header.value;
		out += "\r\n";
	}

	out += "Content-Length: ";
	out += std::to_string(response.body.size());
	out += "\r\nConnection: ";
	out += keep_alive ? "keep-alive" : "close";
	out += "\r\n\r\n";
	out += response.body;
	return out;
}

} // namespace pylai::http
