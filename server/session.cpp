module;
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstring>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>

#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <asio/experimental/awaitable_operators.hpp>

module pylai;

import jk;
import :config;
import :http;
import :log;
import :route;
import :session;

namespace pylai {

asio::awaitable<void> Session::handle(const Settings& settings, const route::Router& routes, asio::ip::tcp::socket socket)
{
	std::error_code ec;
	const auto endpoint = socket.remote_endpoint(ec);
	std::string remote = "<unknown>";
	if(!ec) remote = fmt::format("{}:{}", endpoint.address().to_string(), endpoint.port());

	Session session(settings, routes, std::move(socket), std::move(remote));
	log::info("[{}] connected", session.remote);

	try
	{
		co_await session.run();
	}
	catch(const std::system_error& e)
	{
		if(e.code() == asio::error::operation_aborted)
		{
			log::info("[{}] cancelled", session.remote);
			co_return;
		}
		log::err("[{}] error: {}", session.remote, e.what());
	}
	catch(const std::exception& e)
	{
		log::err("[{}] error: {}", session.remote, e.what());
	}

	std::error_code ignored;
	session.socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
	log::info("[{}] disconnected", session.remote);
}

Session::Session(const Settings& settings, const route::Router& routes, asio::ip::tcp::socket socket, std::string remote) :
	settings(settings),
	routes(routes),
	socket(std::move(socket)),
	remote(std::move(remote)),
	ctx(settings.max_body_bytes)
{}

asio::awaitable<void> Session::run()
{
	for(std::size_t served = 0;; ++served)
	{
		if(!co_await read_headers()) co_return;

		auto& req = ctx.request();
		body_read = 0;
		body_finished = !req.has_body();

		want_keep_alive = req.keep_alive && (served + 1) < settings.max_requests_per_connection;
		head_request = req.method == "HEAD";
		head_written = false;
		keep_alive_decided = want_keep_alive;
		write_failed = false;

		bool body_failed = false;
		int body_status = 0;

		try
		{
			co_await routes.dispatch(ctx, *this, *this); // 状态 + 读通道 + 写通道都是本连接的
		}
		catch(const http::BodyError& e)
		{
			body_failed = true;
			body_status = e.status();
			log::warn("[{}] {}", remote, e.what());
		}

		if(body_failed)
		{
			if(body_status != 0) co_await reply_error(body_status);
			co_return;
		}

		if(write_failed) co_return;

		// 留下没读完的体（handler 没读 / 只读了一半）⇒ 不替客户端 drain，直接收工。
		if(!body_finished) log::info("[{}] request body left unread; closing", remote);

		if(!keep_alive()) co_return;

		if(!co_await finish_message()) co_return;
	}
}

// 错误应答：不再保持连接（体大半没读完），发一个最小的字符串响应。
asio::awaitable<void> Session::reply_error(int status)
{
	want_keep_alive = false;

	route::StringResponse response;
	response.attach(ctx, *this);
	response.status(status);
	co_await response.finish();
}

// 解析到"头部完成"（交棒点）。返回后 parser 已 resume，handler 可以拉体。
asio::awaitable<bool> Session::read_headers()
{
	for(;;)
	{
		if(buffered > 0)
		{
			std::size_t consumed = 0;
			const auto status = ctx.execute(std::span{buffer}.first(buffered), consumed);

			switch(status)
			{
			case http::ParseStatus::headers_complete:
				consume(consumed);
				ctx.resume();
				co_return true;
			case http::ParseStatus::message_complete: // 无体请求可能一路走完（防御）
				consume(consumed);
				message_done = true;
				co_return true;
			case http::ParseStatus::too_large:
				log::warn("[{}] request body over {} bytes", remote, settings.max_body_bytes);
				co_await reply_error(413);
				co_return false;
			case http::ParseStatus::error:
				log::warn("[{}] bad request: {}", remote, ctx.error_reason());
				co_await reply_error(400);
				co_return false;
			default:
				break;
			}
		}

		if(buffered == buffer.size())
		{
			log::warn("[{}] request header too large", remote);
			co_await reply_error(431);
			co_return false;
		}

		if(!co_await fill_buffer()) co_return false;
	}
}

// 让 llhttp 把当前消息走完（读完体的请求已停在 message_complete）。返回前 resume。
asio::awaitable<bool> Session::finish_message()
{
	while(!message_done)
	{
		std::size_t consumed = 0;
		const auto status = ctx.execute(std::span{buffer}.first(buffered), consumed);
		if(consumed > 0) consume(consumed);

		switch(status)
		{
		case http::ParseStatus::message_complete:
			message_done = true;
			break;
		case http::ParseStatus::error:
		case http::ParseStatus::too_large:
			log::warn("[{}] bad request: {}", remote, ctx.error_reason());
			co_return false;
		case http::ParseStatus::headers_complete:
			log::warn("[{}] unexpected parse state while finishing the message", remote);
			co_return false;
		default:
			if(buffered == buffer.size()) co_return false;
			if(!co_await fill_buffer()) co_return false;
			break;
		}
	}

	ctx.resume();
	message_done = false;
	co_return true;
}

// http::BodyReader 的实现：把 llhttp 的体载荷拉出来交给 handler。
// 每次最多交付 out.size() 字节 —— 这同时把“每次喂给 llhttp 的输入窗口”限死了。
asio::awaitable<std::size_t> Session::read_some(std::span<std::byte> out)
{
	if(out.empty()) throw std::invalid_argument{"Session::read_some: empty buffer"};

	for(;;)
	{
		if(body_finished) co_return 0;

		if(buffered > 0)
		{
			const auto step = ctx.read_body(std::span{buffer}.first(buffered), out);

			if(step.failed) throw http::BodyError{400, std::string{ctx.error_reason()}};

			consume(step.consumed);
			if(body_read + step.delivered > settings.max_body_bytes)
				throw http::BodyError{
					413, fmt::format("request body over {} bytes", settings.max_body_bytes)
				};

			body_read += step.delivered;
			body_finished = step.finished;
			message_done = step.finished;

			if(step.delivered > 0 || body_finished) co_return step.delivered;
			continue; // 窗口里全是 chunk 头 ⇒ 再拉一次
		}

		if(buffered == buffer.size()) throw http::BodyError{400, "request body stalled"};

		// 对面断了或空闲超时 ⇒ 体读不完了（此刻无法再应答，交给连接层收工）
		if(!co_await fill_buffer()) throw http::BodyError{0, "connection ended before the request body was complete"};
	}
}

asio::awaitable<bool> Session::fill_buffer()
{
	using namespace asio::experimental::awaitable_operators;

	asio::steady_timer timer(socket.get_executor(), settings.idle_timeout);

	const auto result = co_await (
		timer.async_wait(asio::as_tuple(asio::use_awaitable))
		|| socket.async_read_some(
			asio::buffer(buffer.data() + buffered, buffer.size() - buffered), asio::as_tuple(asio::use_awaitable)
		)
	);

	if(result.index() == 0)
	{
		log::info("[{}] idle timeout", remote);
		co_return false;
	}

	const auto& [ec, bytes] = std::get<1>(result);
	if(ec)
	{
		if(ec == asio::error::eof) co_return false;
		throw std::system_error(ec, "read");
	}
	if(bytes == 0) co_return false;

	buffered += bytes;
	co_return true;
}

asio::awaitable<std::error_code> Session::write(std::span<const std::byte> bytes)
{
	if(!head_written)
	{
		head_written = true;
		keep_alive_decided = want_keep_alive && body_finished;
	}

	using namespace asio::experimental::awaitable_operators;

	asio::steady_timer timer(socket.get_executor(), settings.write_timeout);

	const auto result = co_await (
		timer.async_wait(asio::as_tuple(asio::use_awaitable))
		|| asio::async_write(socket, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable))
	);

	if(result.index() == 0)
	{
		log::warn("[{}] write timeout", remote);
		write_failed = true;
		co_return http::make_error_code(http::WriteErrc::timed_out);
	}

	const auto& [ec, written] = std::get<1>(result);
	(void)written;
	if(ec)
	{
		log::warn("[{}] write failed: {}", remote, ec.message());
		write_failed = true;
		co_return http::make_error_code(http::WriteErrc::closed);
	}

	co_return std::error_code{};
}

bool Session::keep_alive() const noexcept
{
	// 头一写出去就冻结：那时头部已经把 Connection 的值写死在线上，而“未读请求体”
	// 要到 handler 返回时才知道。
	return head_written ? keep_alive_decided : (want_keep_alive && body_finished);
}

void Session::consume(std::size_t n)
{
	const auto remaining = buffered - n;
	if(remaining > 0 && n > 0) std::memmove(buffer.data(), buffer.data() + n, remaining);
	buffered = remaining;
}

} // namespace pylai
