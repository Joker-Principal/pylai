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
#include <system_error>
#include <type_traits>
#include <vector>

export module pylai:http;

import asio; // Request 的读体接口是协程（⇒ DAG 新增一条边，见 docs/modules.md）

export namespace pylai::http {

struct Header
{
	std::string name;
	std::string value;
};

// 字节 → 文本的**显式**转换（只换看法，不校验编码）；反方向用 `std::as_bytes`。
[[nodiscard]] inline std::string_view as_text(std::span<const std::byte> bytes) noexcept
{
	return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// 读体的句柄：实现在连接层（`Session`），框架与 handler 只认这个接口。
class BodyReader
{
public:
	BodyReader() = default;
	virtual ~BodyReader() = default;
	BodyReader(const BodyReader&) = delete;
	BodyReader& operator=(const BodyReader&) = delete;

	// 交付下一段请求体（chunked 的块边界已由 llhttp 剥掉）：写进 out，返回写入字节数；
	// 0 = 体已到尾。out 不得为空（否则 std::invalid_argument）；失败抛 BodyError。
	virtual asio::awaitable<std::size_t> read_some(std::span<std::byte> out) = 0;

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
// ⚠️ **体不在里面** —— 体由 handler 自己拉（`body()` / `read_some()`）。
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

	// 读完整个体（受 `max_body_bytes` 约束）；无体 ⇒ 空。
	[[nodiscard]] asio::awaitable<std::vector<std::byte>> body() const;
	// 增量读一段（大文件 / SSE 用）；返回 0 = 体结束。
	asio::awaitable<std::size_t> read_some(std::span<std::byte> out) const;
};

// 写响应时可能出的错（`std::error_code` + 自定义 category；空 error_code = 成功）。
enum class WriteErrc
{
	closed = 1, // 对端已断 / 写失败
	timed_out,  // 写超时
};

[[nodiscard]] const std::error_category& write_category() noexcept;
[[nodiscard]] std::error_code make_error_code(WriteErrc value) noexcept;

// 响应侧的写通道：擦除边界上**唯一能碰 socket** 的东西（实现在连接层 `Session`）。
class ResponseWriter
{
public:
	ResponseWriter() = default;
	virtual ~ResponseWriter() = default;
	ResponseWriter(const ResponseWriter&) = delete;
	ResponseWriter& operator=(const ResponseWriter&) = delete;

	// 写一段线上字节（带超时）。返回空 error_code = 成功；否则是 `WriteErrc` ⇒ 这个响应已经没救了。
	virtual asio::awaitable<std::error_code> write(std::span<const std::byte> bytes) = 0;

	// ⚠️ 必须在**发响应头之前**读，且只反映“此刻”：头一写出去就冻结，HEAD 抑制正文。
	[[nodiscard]] virtual bool keep_alive() const noexcept = 0;
	[[nodiscard]] virtual bool head_only() const noexcept = 0;
};

// 成帧：纯函数（不碰 socket）。`head_bytes` 返回**文本**（状态行 + 字段），chunk / 末块返回**字节**。
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
std::vector<std::byte> chunk_bytes(std::span<const std::byte> data);
std::vector<std::byte> last_chunk_bytes(std::span<const Header> trailers);

// 响应已经写到哪一步 —— 状态机的依据。顺序错的动作 ⇒ 抛 `std::logic_error`（消息里点名动作）。
enum class Stage : std::uint8_t
{
	config,	  // 头还没发出去：status / content_type / header / trailer 预告 都能改
	head_sent, // 头已发：上面那些都改不动了（写进线上的字节收不回来）
	finished,  // 已收尾：再写什么都不理（幂等）
};

// 顺序错误（头已发还改 status 之类）⇒ 抛 `std::logic_error`，消息里点名动作。
[[noreturn]] void throw_out_of_order(std::string_view action);

// 三种响应的公共面。链式 setter 返回**具体类型**（CRTP）⇒ `res.status(201).header("X-A", "1")` 可以混着链。
template <typename Derived>
class ResponseBase
{
public:
	ResponseBase(const ResponseBase&) = delete;
	ResponseBase& operator=(const ResponseBase&) = delete;
	ResponseBase(ResponseBase&&) noexcept = default;
	ResponseBase& operator=(ResponseBase&&) noexcept = default;

	[[nodiscard]] int status() const noexcept { return status_; }
	[[nodiscard]] std::string_view content_type() const noexcept { return content_type_; }
	[[nodiscard]] std::span<const Header> headers() const noexcept { return headers_; }

	Derived& status(int code)
	{
		require_config("status");
		status_ = code;
		return self();
	}

	Derived& content_type(std::string_view value)
	{
		require_config("content_type");
		content_type_ = value;
		return self();
	}

	Derived& header(std::string_view name, std::string_view value)
	{
		require_config("header");
		headers_.push_back({std::string{name}, std::string{value}});
		return self();
	}

	// 预告尾部要补哪些字段（只对 chunked 有意义；SHOULD 预告 —— RFC 9110 §6.6.2）。
	Derived& trailer(std::string_view name)
	{
		require_config("trailer");
		trailer_names_.emplace_back(name);
		return self();
	}

	// 框架在调 handler 之前注入写通道。
	void attach(ResponseWriter& writer) noexcept { out_ = &writer; }

protected:
	ResponseBase() = default;

	[[nodiscard]] Derived& self() noexcept { return static_cast<Derived&>(*this); }

	void require_config(std::string_view action) const
	{
		if(stage_ != Stage::config) throw_out_of_order(action);
	}

	void require_writer() const
	{
		if(out_ == nullptr) throw std::logic_error{"响应写通道未注入（只能由框架在调 handler 之前 attach）"};
	}

	// 把响应头发出去；framing 由派生类型给。成功 ⇒ head_sent。
	asio::awaitable<std::error_code> send_head(std::optional<std::uint64_t> content_length, bool chunked)
	{
		require_writer();

		std::vector<Header> hint; // `Trailer:` 预告只要名字，值在末块之后补
		hint.reserve(trailer_names_.size());
		for(const std::string& name : trailer_names_) hint.push_back({name, {}});

		const auto head = head_bytes(
			status_, content_type_, headers_, content_length, chunked, out_->keep_alive(), hint
		);
		const auto ec = co_await out_->write(std::as_bytes(std::span{head}));
		if(!ec) stage_ = Stage::head_sent;
		co_return ec;
	}

	int status_{200};
	std::string content_type_{};
	std::vector<Header> headers_;
	std::vector<std::string> trailer_names_;
	ResponseWriter* out_{nullptr};
	Stage stage_{Stage::config};
};

// 有体的响应：`body(块)` 只往内存追加，出网是 flush / finish 的事。
// 文本写法另给 `string_view` 重载；streaming = false 时头一发出就不许再追加。
template <typename Derived, bool streaming>
class BodyResponse : public ResponseBase<Derived>
{
public:
	using ResponseBase<Derived>::self;

	Derived& body(std::span<const std::byte> data)
	{
		require_appendable();
		pending_.insert(pending_.end(), data.begin(), data.end());
		return self();
	}

	// 文本当体（`res.body("ok\n")` 走这个，零拷贝）。
	Derived& body(std::string_view text) { return body(std::as_bytes(std::span{text})); }

	// 还没发出去的体（定长类型 = 整个体）。
	[[nodiscard]] std::span<const std::byte> body() const noexcept { return pending_; }

protected:
	BodyResponse() = default;

	std::vector<std::byte> pending_{};

private:
	void require_appendable() const
	{
		if constexpr(streaming)
		{
			if(this->stage_ == Stage::finished) throw_out_of_order("body");
		}
		else
		{
			this->require_config("body");
		}
	}
};

// 有体、长度已知：长度就是 `body()` 攒下的字节数 ⇒ `Content-Length` 定界。
struct StringResponse : BodyResponse<StringResponse, false>
{
	StringResponse() { content_type_ = "text/plain; charset=utf-8"; }

	// flush：发头 + 发体（HEAD 只发头）。定长体一次性发完 ⇒ flush 即收尾。
	asio::awaitable<std::error_code> flush();
	// 收尾（= flush，幂等）：给框架兜底与推式响应一个统一的形状。
	asio::awaitable<std::error_code> finish() { return flush(); }
};

// 无体响应。⚠️ 刻意**没有** `body(…)` —— 无体响应不接受体（编译期拦）。
// 204 / 304 / 1xx 不得带 `Content-Length`（RFC 9110 §8.6）；其它状态码要 `Content-Length: 0` 才能定界。
struct EmptyResponse : ResponseBase<EmptyResponse>
{
	EmptyResponse() { status_ = 204; }

	asio::awaitable<std::error_code> flush();
	asio::awaitable<std::error_code> finish() { return flush(); }
};

// 长度未知 ⇒ 响应侧 chunked：`body(块)` 攒进内存，`flush()` 的时候才作为一个块推出去。
struct UnknownLengthResponse : BodyResponse<UnknownLengthResponse, true>
{
	UnknownLengthResponse() { content_type_ = "text/plain; charset=utf-8"; }

	// flush：头没发就先发头，然后把攒着的字节作为一个块发出去。
	asio::awaitable<std::error_code> flush();
	// 收尾：flush + 末块 + trailer（幂等）。handler 不调的话由框架兜底。
	asio::awaitable<std::error_code> finish(std::span<const Header> trailers = {});
};

// 响应类型的判据（`route::make_arg` 据此区分“请求形参”与“响应形参”）：
// 框架要能给它注入写通道、能兜底收尾。
template <typename T>
concept IsResponse = requires(T& response, ResponseWriter& writer) {
	{ response.attach(writer) } -> std::same_as<void>;
	{ response.finish() } -> std::same_as<asio::awaitable<std::error_code>>;
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
	ParseStatus execute(std::span<const std::byte> data, std::size_t& consumed);

	// **体阶段**：载荷**直接写进 out**（最多 out.size() 字节，不缓存在 Parser 里）。
	struct BodyStep
	{
		std::size_t consumed{0};  // 本次消费的输入字节数
		std::size_t delivered{0}; // 写进 out 的载荷字节数
		bool finished{false};	  // 该请求的体（整个消息）已到尾
		bool failed{false};		  // 协议错误 —— error_reason() 里有原因
	};

	[[nodiscard]] BodyStep read_body(std::span<const std::byte> data, std::span<std::byte> out);

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

// `std::error_code{WriteErrc::closed}` 与 `ec == WriteErrc::closed` 需要这个特化。
template <>
struct std::is_error_code_enum<pylai::http::WriteErrc> : std::true_type
{
};
