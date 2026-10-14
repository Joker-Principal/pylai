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

// 读体失败的码 —— `value()` 直接就是该回的 HTTP 状态码（`closed` = 对面断了，无响应可发）。
// 给“用 asio 组合操作读体”的 handler 一个 `error_code` 形态（异常路径仍是 `BodyError`）。
enum class BodyErrc : int
{
	closed = 0,      // 对面已断 / 超时 ⇒ 没法应答
	too_large = 413, // 超过 max_body_bytes
	bad_request = 400,
};

[[nodiscard]] const std::error_category& body_category() noexcept;
[[nodiscard]] std::error_code make_error_code(BodyErrc value) noexcept;

// 读体失败的异常：`status != 0` ⇒ 连接层回这个状态码并关连接；`status == 0` ⇒ 对面已断/超时，只收工。
class BodyError : public std::runtime_error
{
public:
	BodyError(int status, std::string message) : std::runtime_error{std::move(message)}, status_(status) {}
	[[nodiscard]] int status() const noexcept { return status_; }

private:
	int status_;
};

// 读体的句柄：实现在连接层（`Session`），框架与 handler 只认这个接口。
// 它满足 asio 的 **`AsyncReadStream`**：`get_executor()` + `async_read_some(序列, token)`
// ⇒ `asio::async_read` / `async_read_until` 之类的组合操作可以直接吃它。
class BodyReader
{
public:
	using executor_type = asio::any_io_executor;

	BodyReader() = default;
	virtual ~BodyReader() = default;
	BodyReader(const BodyReader&) = delete;
	BodyReader& operator=(const BodyReader&) = delete;

	// 交付下一段请求体（chunked 的块边界已由 llhttp 剥掉）：写进 out，返回写入字节数；
	// 0 = 体已到尾。out 不得为空（否则 std::invalid_argument）；失败抛 BodyError。
	virtual asio::awaitable<std::size_t> read_some(std::span<std::byte> out) = 0;

	[[nodiscard]] virtual asio::any_io_executor get_executor() noexcept = 0;

	// ⚠️ completion token 是模板 ⇒ 不可能虚化；门面内联在这里，虚实现仍是上面的 `read_some`。
	// 只填**第一段非空** buffer：概念只要求“写了 ≥1 字节”，组合操作会自己循环。
	// 内部用 0 表示“体到尾”，门面翻译成 asio 语义的 `eof`。
	template <typename MutableBufferSequence, typename CompletionToken>
	auto async_read_some(const MutableBufferSequence& buffers, CompletionToken&& token)
	{
		return asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
			asio::co_composed<void(std::error_code, std::size_t)>(
				[](auto state, BodyReader* self, MutableBufferSequence buffers) -> void {
					auto it = asio::buffer_sequence_begin(buffers);
					const auto end = asio::buffer_sequence_end(buffers);
					while(it != end && it->size() == 0) ++it;
					if(it == end)
					{
						co_return {asio::error::invalid_argument, std::size_t{0}};
					}

					const auto span = std::span{static_cast<std::byte*>(it->data()), it->size()};
					try
					{
						// `co_composed` 只接受**异步操作** ⇒ 用 `co_spawn(…, deferred)` 把虚接口的
						// 协程包成操作对象（不额外分配：协程帧在调 `read_some` 时就建好了）。
						const std::size_t n =
							co_await asio::co_spawn(self->get_executor(), self->read_some(span), asio::deferred);
						if(n == 0) co_return {asio::error::eof, std::size_t{0}};
						co_return {std::error_code{}, n};
					}
					catch(const BodyError& e)
					{
						co_return {make_error_code(static_cast<BodyErrc>(e.status())), std::size_t{0}};
					}
				},
				*this
			),
			token,
			this,
			buffers
		);
	}
};

// 把读通道拉干：反复 `read_some` 直到 0（上限 / 超时由通道实现自己管）。
// `reserve_hint` 只影响预留（有 `Content-Length` 时传它，少几次扩容）。
[[nodiscard]] asio::awaitable<std::vector<std::byte>> read_body(BodyReader& in, std::uint64_t reserve_hint = 0);

// 一个请求的报文数据：start line + headers。**纯数据** —— 不带宽读能力。
// ⚠️ 体也不在里面：体由 handler 走**读通道**（`BodyReader`）自己拉；
//    "用哪个通道"由 `route::Request`（视图）持有，报文容器不必知道。
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

	// 这个请求有没有体（框架用来决定要不要让 llhttp 继续走完消息）。
	[[nodiscard]] bool has_body() const noexcept { return chunked || content_length > 0; }
};

// 解析到哪一步（`RequestParser::execute` 的返回）。
enum class ParseStatus
{
	incomplete,		  // 需要更多字节（连头部都还没完）
	headers_complete, // 头部解析完 —— 框架在此交棒；consumed = 该请求头部占用的字节数
	message_complete, // 整个消息（含体）走完 —— 停在这里，摘掉 consumed 后 resume() 再解析下一个
	too_large,		  // 声明的 Content-Length 超上限（解析已就地中止，不会进路由）
	error,			  // 协议错误
};

// 请求解析器：把字节流变成 `Request`（llhttp 的持有者）。
// ⚠️ 它只是“事务状态”的一件：请求侧的解析进度；整条事务（含响应状态）是 `Context`。
class RequestParser
{
public:
	// max_body_bytes：请求体上限。声明的 Content-Length 超上限 ⇒ execute() 返回 too_large。
	explicit RequestParser(std::size_t max_body_bytes);
	~RequestParser();

	RequestParser(RequestParser&&) noexcept;
	RequestParser& operator=(RequestParser&&) noexcept;
	RequestParser(const RequestParser&) = delete;
	RequestParser& operator=(const RequestParser&) = delete;

	// **头部阶段**：喂字节直到 headers_complete（无体请求会一路走到 message_complete）。
	// 返回后调用方应从缓冲区头部摘掉 consumed 字节。
	ParseStatus execute(std::span<const std::byte> data, std::size_t& consumed);

	// **体阶段**：载荷**直接写进 out**（最多 out.size() 字节，不缓存在 RequestParser 里）。
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
	Request& current(); // 当前这条请求的报文数据（解析完 start line + headers 后有效）

	// 仅在 execute() 返回 error 时有意义。
	std::string_view error_reason() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
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
// 它满足 asio 的 **`AsyncWriteStream`**：`get_executor()` + `async_write_some(序列, token)`
// ⇒ `asio::async_write` 之类的组合操作、以及任何接受写流的东西可以直接吃它。
class ResponseWriter
{
public:
	using executor_type = asio::any_io_executor;

	ResponseWriter() = default;
	virtual ~ResponseWriter() = default;
	ResponseWriter(const ResponseWriter&) = delete;
	ResponseWriter& operator=(const ResponseWriter&) = delete;

	// 写一段线上字节（带超时）。返回空 error_code = 成功；否则是 `WriteErrc` ⇒ 这个响应已经没救了。
	virtual asio::awaitable<std::error_code> write(std::span<const std::byte> bytes) = 0;

	// ⚠️ 必须在**发响应头之前**读，且只反映“此刻”：头一写出去就冻结，HEAD 抑制正文。
	[[nodiscard]] virtual bool keep_alive() const noexcept = 0;
	[[nodiscard]] virtual bool head_only() const noexcept = 0;

	[[nodiscard]] virtual asio::any_io_executor get_executor() noexcept = 0;

	// `async_write_some` 允许“一次写完整块”（比概念要求的“写一部分”更强）；
	// 单段零拷贝，多段才聚合（多段是少数）。
	template <typename ConstBufferSequence, typename CompletionToken>
	auto async_write_some(const ConstBufferSequence& buffers, CompletionToken&& token)
	{
		return asio::async_initiate<CompletionToken, void(std::error_code, std::size_t)>(
			asio::co_composed<void(std::error_code, std::size_t)>(
				[](auto state, ResponseWriter* self, ConstBufferSequence buffers) -> void {
					const std::size_t total = asio::buffer_size(buffers);
					if(total == 0) co_return {std::error_code{}, std::size_t{0}};

					const auto begin = asio::buffer_sequence_begin(buffers);
					std::vector<std::byte> joined;
					std::span<const std::byte> bytes;
					if(begin->size() == total)
					{
						bytes = {static_cast<const std::byte*>(begin->data()), total};
					}
					else
					{
						joined.resize(total);
						asio::buffer_copy(asio::buffer(joined), buffers);
						bytes = joined;
					}

					const auto ec = co_await asio::co_spawn(self->get_executor(), self->write(bytes), asio::deferred);
					co_return {ec, ec ? std::size_t{0} : total};
				},
				*this
			),
			token,
			this,
			buffers
		);
	}
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

// 响应侧的状态：链式 setter 写的就是这里；它住在 `Context` 里（不是响应对象里）。
struct ResponseState
{
	int status{200};
	std::string content_type;
	std::vector<Header> headers;
	std::vector<std::string> trailer_names; // 预告尾部要补哪些字段（只对 chunked 有意义）
	std::vector<std::byte> pending;		   // 攒着还没出网的体
	Stage stage{Stage::config};

	// 清成初始态，但**保留缓冲容量**（复用 Context 的意义就在这里）。
	void reset() noexcept
	{
		status = 200;
		content_type.clear();
		headers.clear();
		trailer_names.clear();
		pending.clear();
		stage = Stage::config;
	}
};

// 一条请求 / 响应事务的**状态**（不做 IO）：
//   请求侧 —— llhttp 解析进度 + 已解析的报文数据（`RequestParser`）
//   响应侧 —— status / headers / trailer 预告 / 待写体 / 写到哪一步（`ResponseState`）
// ⚠️ 字节进出全由通道实现（`Session`）做；连接层每请求准备一份，可复用 ⇒ 缓冲容量跨请求保留。
// ⚠️ **连接级**的东西（未消费字节、keep-alive 判定、超时）不在这里。
class Context
{
public:
	using BodyStep = RequestParser::BodyStep;

	explicit Context(std::size_t max_body_bytes);

	// ═══ 请求状态 ═══
	[[nodiscard]] ParseStatus execute(std::span<const std::byte> data, std::size_t& consumed);
	[[nodiscard]] BodyStep read_body(std::span<const std::byte> data, std::span<std::byte> out);
	void resume();
	[[nodiscard]] const Request& request() const;
	[[nodiscard]] Request& request();
	[[nodiscard]] std::string_view error_reason() const;

	// ═══ 响应状态 ═══
	[[nodiscard]] ResponseState& response() noexcept { return response_; }
	[[nodiscard]] const ResponseState& response() const noexcept { return response_; }

private:
	RequestParser parser_;
	ResponseState response_;
};

// 三种响应的公共面。链式 setter 返回**具体类型**（CRTP）⇒ `res.status(201).header("X-A", "1")` 可以混着链。
template <typename Derived>
class ResponseBase
{
public:
	ResponseBase(const ResponseBase&) = delete;
	ResponseBase& operator=(const ResponseBase&) = delete;
	ResponseBase(ResponseBase&&) noexcept = default;
	ResponseBase& operator=(ResponseBase&&) noexcept = default;

	[[nodiscard]] int status() const { return state().status; }
	[[nodiscard]] std::string_view content_type() const { return state().content_type; }
	[[nodiscard]] std::span<const Header> headers() const { return state().headers; }

	Derived& status(int code)
	{
		require_config("status");
		state().status = code;
		return self();
	}

	Derived& content_type(std::string_view value)
	{
		require_config("content_type");
		state().content_type = value;
		return self();
	}

	Derived& header(std::string_view name, std::string_view value)
	{
		require_config("header");
		state().headers.push_back({std::string{name}, std::string{value}});
		return self();
	}

	// 预告尾部要补哪些字段（只对 chunked 有意义；SHOULD 预告 —— RFC 9110 §6.6.2）。
	Derived& trailer(std::string_view name)
	{
		require_config("trailer");
		state().trailer_names.emplace_back(name);
		return self();
	}

	// 框架在调 handler 之前注入：状态的家（`Context`）+ 写通道。
	// ⚠️ 顺带把上一次请求的响应状态清干净（Context 跨请求复用）⇒ **配置必须在 attach 之后**。
	void attach(Context& context, ResponseWriter& writer) noexcept
	{
		ctx_ = &context;
		out_ = &writer;
		context.response().reset();
		self().apply_defaults();
	}

	// 各响应类型的初始状态（`EmptyResponse` 默认 204、文本类型默认 `text/plain` …）；框架 attach 时叫它。
	void apply_defaults() noexcept {}

protected:
	ResponseBase() = default;

	[[nodiscard]] Derived& self() noexcept { return static_cast<Derived&>(*this); }

	// 状态就在 `Context` 里 —— 响应对象只是它的一个薄门面。
	[[nodiscard]] ResponseState& state()
	{
		if(ctx_ == nullptr) throw std::logic_error{"响应状态还没就位：只能由框架 attach(Context&, ResponseWriter&) 之后用"};
		return ctx_->response();
	}

	[[nodiscard]] const ResponseState& state() const
	{
		if(ctx_ == nullptr) throw std::logic_error{"响应状态还没就位：只能由框架 attach(Context&, ResponseWriter&) 之后用"};
		return ctx_->response();
	}

	void require_config(std::string_view action) const
	{
		if(state().stage != Stage::config) throw_out_of_order(action);
	}

	void require_writer() const
	{
		if(out_ == nullptr) throw std::logic_error{"响应写通道未注入（只能由框架在调 handler 之前 attach）"};
	}

	// 把响应头发出去；framing 由派生类型给。成功 ⇒ head_sent。
	asio::awaitable<std::error_code> send_head(std::optional<std::uint64_t> content_length, bool chunked)
	{
		require_writer();
		auto& s = state();

		std::vector<Header> hint; // `Trailer:` 预告只要名字，值在末块之后补
		hint.reserve(s.trailer_names.size());
		for(const std::string& name : s.trailer_names) hint.push_back({name, {}});

		const auto head = head_bytes(
			s.status, s.content_type, s.headers, content_length, chunked, out_->keep_alive(), hint
		);
		const auto ec = co_await out_->write(std::as_bytes(std::span{head}));
		if(!ec) s.stage = Stage::head_sent;
		co_return ec;
	}

protected:
	// 成员放末尾；派生类型（`StringResponse::flush` 等）要直接用它发头 / 发体 ⇒ protected 而非 private。
	Context* ctx_{nullptr};		   // 状态的家（框架 attach 时给）
	ResponseWriter* out_{nullptr}; // 写通道（同上）
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
		auto& pending = this->state().pending;
		pending.insert(pending.end(), data.begin(), data.end());
		return self();
	}

	// 文本当体（`res.body("ok\n")` 走这个，零拷贝）。
	Derived& body(std::string_view text) { return body(std::as_bytes(std::span{text})); }

	// 还没发出去的体（定长类型 = 整个体）。
	[[nodiscard]] std::span<const std::byte> body() const { return this->state().pending; }

protected:
	BodyResponse() = default;

private:
	void require_appendable() const
	{
		if constexpr(streaming)
		{
			if(this->state().stage == Stage::finished) throw_out_of_order("body");
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
	void apply_defaults() noexcept { state().content_type = "text/plain; charset=utf-8"; }

	// flush：发头 + 发体（HEAD 只发头）。定长体一次性发完 ⇒ flush 即收尾。
	asio::awaitable<std::error_code> flush();
	// 收尾（= flush，幂等）：给框架兜底与推式响应一个统一的形状。
	asio::awaitable<std::error_code> finish() { return flush(); }
};

// 无体响应。⚠️ 刻意**没有** `body(…)` —— 无体响应不接受体（编译期拦）。
// 204 / 304 / 1xx 不得带 `Content-Length`（RFC 9110 §8.6）；其它状态码要 `Content-Length: 0` 才能定界。
struct EmptyResponse : ResponseBase<EmptyResponse>
{
	void apply_defaults() noexcept { state().status = 204; }

	asio::awaitable<std::error_code> flush();
	asio::awaitable<std::error_code> finish() { return flush(); }
};

// 长度未知 ⇒ 响应侧 chunked：`body(块)` 攒进内存，`flush()` 的时候才作为一个块推出去。
struct UnknownLengthResponse : BodyResponse<UnknownLengthResponse, true>
{
	void apply_defaults() noexcept { state().content_type = "text/plain; charset=utf-8"; }

	// flush：头没发就先发头，然后把攒着的字节作为一个块发出去。
	asio::awaitable<std::error_code> flush();
	// 收尾：flush + 末块 + trailer（幂等）。handler 不调的话由框架兜底。
	asio::awaitable<std::error_code> finish(std::span<const Header> trailers = {});
};

// 响应类型的判据（`route::make_arg` 据此区分“请求形参”与“响应形参”）：
// 框架要能给它注入 Context + 写通道、能兜底收尾。
template <typename T>
concept IsResponse = requires(T& response, Context& context, ResponseWriter& writer) {
	{ response.attach(context, writer) } -> std::same_as<void>;
	{ response.finish() } -> std::same_as<asio::awaitable<std::error_code>>;
};

} // namespace pylai::http

// `std::error_code{WriteErrc::closed}` 与 `ec == WriteErrc::closed` 需要这个特化。
template <>
struct std::is_error_code_enum<pylai::http::WriteErrc> : std::true_type
{
};

template <>
struct std::is_error_code_enum<pylai::http::BodyErrc> : std::true_type
{
};
