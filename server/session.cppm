module;

#include <array>
#include <cstddef>
#include <span>
#include <string>

export module pylai:session;

import asio;
import :config;
import :http;
import :route;

export namespace pylai {

// 一条连接。
// ⚠️ 它同时是 `http::BodyReader` 的实现：框架读到 headers 就交棒，handler 通过 `req.body()`
// 回来从这个对象拉字节（见 docs/http-server-plan.md §4.1.2）。
class Session : public http::BodyReader
{
public:
	// 连接协程入口：静态入口 + 协程帧持有连接状态。
	static asio::awaitable<void> handle(const Settings& settings, const route::Router& routes, asio::ip::tcp::socket socket);

	// http::BodyReader：把下一段体交给 handler（实现见 session.cpp）。
	[[nodiscard]] asio::awaitable<std::size_t> read_some(std::span<char> out) override;
	[[nodiscard]] bool exhausted() const noexcept override { return body_finished; }

private:
	Session(const Settings& settings, const route::Router& routes, asio::ip::tcp::socket socket, std::string remote);

	asio::awaitable<void> run();						// keep-alive 主循环
	asio::awaitable<bool> read_headers();				 // 解析到“头部完成”（交棒点） → true
	asio::awaitable<bool> finish_message();				 // 让 llhttp 走完当前消息（停在下一个请求之前）
	asio::awaitable<bool> fill_buffer();				// 带超时读一次
	asio::awaitable<bool> write(std::string_view text); // 带超时写

	void consume(std::size_t n); // 从缓冲区头部摘掉已消费的 n 字节

	const Settings& settings;
	const route::Router& routes;
	asio::ip::tcp::socket socket;
	std::string remote;
	http::Parser parser;

	std::array<char, 16 * 1024> buffer{};
	std::size_t buffered{0}; // buffer[0, buffered) 是尚未消费的字节

	std::size_t body_read{0}; // 本请求已交付给 handler 的体字节数（chunked 的上限判定用）
	bool body_finished{true}; // 本请求的体是否已读完（无体请求在交棒前就置 true）
	bool message_done{false}; // llhttp 是否已停在 message_complete
};

} // namespace pylai
