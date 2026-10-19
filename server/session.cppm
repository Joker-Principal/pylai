module;
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

export module pylai:session;

import :config;
import :http;
import :route;

export namespace pylai {

// 一条连接。
// ⚠️ 它同时是两侧擦除接口的实现（`http::BodyReader` + `http::ResponseWriter`）——
// 两个方向的字节都只能从它过，所以超时 / 取消绕不过。
class Session : public http::BodyReader, public http::ResponseWriter
{
public:
	// 连接协程入口：静态入口 + 协程帧持有连接状态。
	static asio::awaitable<void> handle(const Settings& settings, const route::Router& routes, asio::ip::tcp::socket socket);

public:
	// http::BodyReader：把下一段体交给 handler（实现见 session.cpp）。
	[[nodiscard]] asio::awaitable<std::size_t> read_some(std::span<std::byte> out) override;

public:
	// http::ResponseWriter：带超时写一段字节（实现见 session.cpp）。空 error_code = 成功。
	[[nodiscard]] asio::awaitable<std::error_code> write(std::span<const std::byte> bytes) override;

public:
	// http::BodyReader / http::ResponseWriter：两侧的字节通道。
	[[nodiscard]] asio::any_io_executor get_executor() noexcept override { return socket.get_executor(); }

public:
	// 响应类型在**发响应头之前**读这两个值（头一写出去就冻结 / HEAD 抑制正文）。
	[[nodiscard]] bool keep_alive() const noexcept override;
	[[nodiscard]] bool head_only() const noexcept override { return head_request; }

private:
	Session(const Settings& settings, const route::Router& routes, asio::ip::tcp::socket socket, std::string remote);

private:
	asio::awaitable<void> run();					 // keep-alive 主循环
	asio::awaitable<bool> read_headers();		 // 解析到“头部完成”（交棒点） → true
	asio::awaitable<bool> finish_message();		 // 让 llhttp 走完当前消息（停在下一个请求之前）
	asio::awaitable<bool> fill_buffer();		 // 带超时读一次
	asio::awaitable<void> reply_error(int status); // 错误应答：强制 close + 发一个最小的字符串响应

	void consume(std::size_t n); // 从缓冲区头部摘掉已消费的 n 字节

	const Settings& settings;
	const route::Router& routes;
	asio::ip::tcp::socket socket;
	std::string remote;
	http::Context ctx; // 每请求的事务状态（解析进度 + 报文数据 + 响应状态；跨请求复用）

	std::array<std::byte, 16 * 1024> buffer{};
	std::size_t buffered{0}; // buffer[0, buffered) 是尚未消费的字节

	std::size_t body_read{0}; // 本请求已交付给 handler 的体字节数（chunked 的上限判定用）
	bool body_finished{true}; // 本请求的体是否已读完（无体请求在交棒前就置 true）
	bool message_done{false}; // llhttp 是否已停在 message_complete

	bool want_keep_alive{false};    // 本请求“本该”保持连接（还要再看体读没读完）
	bool head_request{false};        // 本请求是 HEAD
	bool head_written{false};       // 本请求的响应头是否已发出
	bool keep_alive_decided{false}; // 发头那一刻冻结下来的 keep_alive
	bool write_failed{false};       // 写失败过（超时 / 对端断）⇒ 主循环收工
};

} // namespace pylai
