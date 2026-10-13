module;

#include <array>
#include <cstddef>
#include <string>

export module pylai:session;

import asio;
import :config;
import :http;
import :route;

export namespace pylai {

class Session
{
public:
	// 连接协程入口：静态入口 + 协程帧持有连接状态。
	static asio::awaitable<void> handle(const Settings& settings, const route::Router& routes, asio::ip::tcp::socket socket);

private:
	Session(const Settings& settings, const route::Router& routes, asio::ip::tcp::socket socket, std::string remote);

	asio::awaitable<void> run();						// keep-alive 主循环
	asio::awaitable<bool> read_request();				// 解析出一个请求 → true
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
};

} // namespace pylai
