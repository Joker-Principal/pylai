module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module pylai:http;

export namespace pylai::http {

struct Header
{
	std::string name;
	std::string value;
};

// 一个请求的语义视图。Stage 0 只填"分派与响应"所需的最小集合。
struct Request
{
	std::string method;	 // 例 "GET"
	std::string target;	 // 原始 request-target（未解码，含 query）
	std::string path;	 // target 里 '?' 之前的部分（未解码）
	std::string query;	 // '?' 之后（不含 '?'）；无 query 时为空
	std::string version; // 例 "HTTP/1.1"

	std::vector<Header> headers;

	std::uint64_t content_length{0};
	bool has_content_length{false};
	bool has_query{false};
	bool chunked{false};
	bool keep_alive{false};
};

// 要写回去的响应。状态行与 Content-Length / Connection 由 serialize() 生成，
// 其余（Content-Type、附加头）由 handler 提供；content_type 为空则不发送该头。
struct Response
{
	int status{200};
	std::string content_type{"text/plain; charset=utf-8"};
	std::vector<Header> headers;
	std::string body;
};

// 把响应写成线上字节。keep_alive 决定 Connection 头。
std::string serialize(const Response& response, bool keep_alive);

enum class ParseStatus
{
	incomplete, // 需要更多字节
	complete,	// 已解析出一个完整请求（consumed = 本请求占用的字节数）
	error,		// 协议错误
};

class Parser
{
public:
	Parser();
	~Parser();

	Parser(Parser&&) noexcept;
	Parser& operator=(Parser&&) noexcept;
	Parser(const Parser&) = delete;
	Parser& operator=(const Parser&) = delete;

	// 喂字节。返回 complete 时 `consumed` 是这一个请求占用的字节数，
	// 调用方应从缓冲区头部摘掉这 consumed 字节，然后调用 resume()。
	ParseStatus execute(const char* data, std::size_t length, std::size_t& consumed);

	// 一个请求处理完后调用（解 PAUSED），让 Parser 可以解析下一个。
	void resume();

	const Request& current() const;

	// 仅在 execute() 返回 error 时有意义。
	std::string_view error_reason() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace pylai::http
