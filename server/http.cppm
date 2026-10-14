module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module pylai:http;

import asio; // Request 的读体接口是协程（⇒ DAG 新增一条边，见 docs/modules.md）

export namespace pylai::http {

struct Header
{
	std::string name;
	std::string value;
};

// 读体的句柄：实现在连接层（`Session`），框架与 handler 只认这个接口。
// 为何是抽象基类：`route::Handler` 已经类型擦除（`std::function<awaitable<Response>(const http::Request&)>`），
// 句柄必须**非模板**。⚠️ 擦除点合并之后这里会被重构掉（见 docs/http-server-plan.md §11.2 C）。
class BodyReader
{
public:
	BodyReader() = default;
	virtual ~BodyReader() = default;
	BodyReader(const BodyReader&) = delete;
	BodyReader& operator=(const BodyReader&) = delete;

	// 交付下一段请求体（chunked 的块边界已由 llhttp 剥掉）：写进 out，返回写入字节数；
	// 0 = 体已到尾。out 不得为空（否则 std::invalid_argument）；失败抛 BodyError。
	virtual asio::awaitable<std::size_t> read_some(std::span<char> out) = 0;

	// 本请求的体是否已读完 —— 框架据此判“未读体 ⇒ Connection: close”。
	[[nodiscard]] virtual bool exhausted() const noexcept = 0;
};

// 读体失败：`status != 0` ⇒ 连接层回这个状态码并关连接；`status == 0` ⇒ 对面已断/超时，只收工（无响应可发）。
class BodyError : public std::runtime_error
{
public:
	BodyError(int status, std::string message) : std::runtime_error{std::move(message)}, status_(status) {}
	[[nodiscard]] int status() const noexcept { return status_; }

private:
	int status_;
};

// 一个请求的语义视图：start line + headers。
// ⚠️ **体不在里面** —— 框架读到 headers 就交棒，体由 handler 自己拉（`body()` / `read_some()`）。
struct Request
{
	std::string method;	 // 例 "GET"
	std::string target;	 // 原始 request-target（未解码，含 query）
	std::string path;	 // target 里 '?' 之前的部分（未解码）
	std::string query;	 // '?' 之后（不含 '?'）；无 query 时为空
	std::string version; // 例 "HTTP/1.1"

	std::vector<Header> headers;

	// 读体句柄：框架在交棒前填（无体请求也会填，实现自己知道已到尾）。
	BodyReader* body_reader{nullptr};

	std::uint64_t content_length{0};
	bool has_content_length{false};
	bool has_query{false};
	bool chunked{false};
	bool keep_alive{false};

	// 这个请求有没有体（框架用来决定要不要让 llhttp 继续走完消息）。
	[[nodiscard]] bool has_body() const noexcept { return chunked || content_length > 0; }

	// 读完整个体（受 max_body_bytes 约束）；无体 ⇒ 空串。
	[[nodiscard]] asio::awaitable<std::string> body() const;
	// 增量读一段（大文件 / SSE 用）；返回 0 = 体结束。
	asio::awaitable<std::size_t> read_some(std::span<char> out) const;
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

// 把响应写成线上字节。keep_alive 决定 Connection 头；head_only 抑制正文
// （HEAD 请求：Content-Length 仍按 response.body 报，但不发体）。
std::string serialize(const Response& response, bool keep_alive, bool head_only);

enum class ParseStatus
{
	incomplete,		  // 需要更多字节（连头部都还没完）
	headers_complete, // 头部解析完 —— 框架在此交棒；consumed = 该请求头部占用的字节数
	message_complete, // 整个消息（含体）走完 —— 停在这里，摘掉 consumed 后 resume() 再解析下一个
	too_large,		  // 声明的 Content-Length 超上限（解析已就地中止，不会进路由）
	error,			  // 协议错误
};

class Parser
{
public:
	// max_body_bytes：请求体上限。声明的 Content-Length 超上限 ⇒ execute() 返回 too_large。
	explicit Parser(std::size_t max_body_bytes);
	~Parser();

	Parser(Parser&&) noexcept;
	Parser& operator=(Parser&&) noexcept;
	Parser(const Parser&) = delete;
	Parser& operator=(const Parser&) = delete;

	// **头部阶段**：喂字节直到 headers_complete（无体请求会一路走到 message_complete）。
	// 返回后调用方应从缓冲区头部摘掉 consumed 字节。
	ParseStatus execute(const char* data, std::size_t length, std::size_t& consumed);

	// **体阶段**：载荷**直接写进 out**（最多 out.size() 字节，不缓存在 Parser 里）。
	struct BodyStep
	{
		std::size_t consumed{0};  // 本次消费的输入字节数
		std::size_t delivered{0}; // 写进 out 的载荷字节数
		bool finished{false};	  // 该请求的体（整个消息）已到尾
		bool failed{false};		  // 协议错误 —— error_reason() 里有原因
	};

	[[nodiscard]] BodyStep read_body(const char* data, std::size_t length, std::span<char> out);

	// 解除 headers_complete / message_complete 处的暂停（下一次 execute 前调用）。
	void resume();

	const Request& current() const;
	Request& current(); // 交棒前，框架要往里填 body_reader

	// 仅在 execute() 返回 error 时有意义。
	std::string_view error_reason() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace pylai::http
