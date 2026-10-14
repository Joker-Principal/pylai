module;

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

// 响应侧的写通道：擦除边界上**唯一能碰 socket** 的东西（实现在连接层 `Session`）。
// 为何是抽象基类：`route::Handler` 已经类型擦除，通道必须**非模板**（同 `BodyReader` 的理由）。
class ResponseWriter
{
public:
	ResponseWriter() = default;
	virtual ~ResponseWriter() = default;
	ResponseWriter(const ResponseWriter&) = delete;
	ResponseWriter& operator=(const ResponseWriter&) = delete;

	// 写一段线上字节（带超时）。false = 连接已经没了（写超时 / 对端断）⇒ 调用方应收工。
	virtual asio::awaitable<bool> write(std::string_view bytes) = 0;

	// ⚠️这两项要在**发响应头之前**读，且只反映“此刻”：
	// keep_alive —— 头一写出去就冻结（未读的请求体 ⇒ 只能 close）；head_only —— HEAD 抑制正文。
	[[nodiscard]] virtual bool keep_alive() const noexcept = 0;
	[[nodiscard]] virtual bool head_only() const noexcept = 0;
};

// 成帧：纯函数（不碰 socket）。与通道分开，是为了让“头部 / 块 / 末块”的字节能单独测。
// content_length 有值 → `Content-Length`；chunked → `Transfer-Encoding: chunked`；
// trailer_names 非空 → `Trailer:` 预告（RFC 9110 §6.6.2，SHOULD）。
std::string head_bytes(
	int status,
	std::string_view content_type,
	std::span<const Header> headers,
	std::optional<std::uint64_t> content_length,
	bool chunked,
	bool keep_alive,
	std::span<const Header> trailer_names
);
std::string chunk_bytes(std::string_view data);
std::string last_chunk_bytes(std::span<const Header> trailers);

// 一个有体的响应：头部 + 内存里的体，`Content-Length` 定界。
struct StringResponse
{
	int status{200};
	std::string content_type{"text/plain; charset=utf-8"};
	std::vector<Header> headers;
	std::string body;

	// 发给通道。false = 连接没了（不必再写）。HEAD 时正文不发，`Content-Length` 照旧（RFC 9110 §9.3.2）。
	asio::awaitable<bool> send(ResponseWriter& out) const;
};

// 无体响应。⚠️204 / 304 / 1xx **不得**带 `Content-Length`（RFC 9110 §8.6）；
// 其它状态码用 `EmptyResponse` 就是“零长度内容”，要写 `Content-Length: 0` 才能定界。
struct EmptyResponse
{
	int status{204};
	std::vector<Header> headers;

	asio::awaitable<bool> send(ResponseWriter& out) const;
};

// 长度未知 ⇒ 响应侧 chunked，由 handler 分段推。
// 它是三种响应类型里唯一**在 handler 运行期间就写线上字节**的：写通道由框架在调 handler 前注入。
struct UnknownLengthResponse
{
	int status{200};
	std::string content_type{"text/plain; charset=utf-8"};
	std::vector<Header> headers;

	// 预告尾部要补哪些字段（SHOULD）。必须在**第一次 write 之前**调，否则头已经发出去了。
	void declare_trailer(std::string_view name);

	// 推一块（第一次调用时把响应头发出去）。空串不产生块（空块就是终止块）。
	asio::awaitable<bool> write(std::string_view data);
	// 收尾：末块 + trailer。可重入（重复调无副作用）。
	asio::awaitable<bool> finish(std::span<const Header> trailers = {});

	// 统一入口：handler 没显式 finish 时由框架在这里收尾 ⇒ 三种响应类型的形状一致（`IsResponse`）。
	asio::awaitable<bool> send(ResponseWriter& writer);

	// 框架注入写通道；`attach` 的存在本身就是“这种响应需要通道”的判据。
	void attach(ResponseWriter& writer) noexcept { out = &writer; }

private:
	ResponseWriter* out{nullptr};
	bool started{false};
	bool finished{false};
	std::vector<std::string> trailer_names;
};

// 响应类型的判据（`route::make_arg` 据此区分“请求形参”与“响应形参”）。
template <typename T>
concept IsResponse = requires(T& response, ResponseWriter& writer) {
	{ response.send(writer) } -> std::same_as<asio::awaitable<bool>>;
};

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
