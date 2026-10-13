module;

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>

#include <spdlog/spdlog.h>

module pylai;

import asio;
import asio.experimental;
import jk;
import :config;
import :http;
import :log;
import :route;
import :session;

namespace pylai {

namespace {

http::Response error_response(int status)
{
	http::Response response;
	response.status = status;
	return response;
}

} // namespace

Session::Session(const Settings& settings, const route::Router& routes, asio::ip::tcp::socket socket, std::string remote) :
	settings(settings),
	routes(routes),
	socket(std::move(socket)),
	remote(std::move(remote))
{}

asio::awaitable<void> Session::handle(const Settings& settings, const route::Router& routes, asio::ip::tcp::socket socket)
{
	std::error_code ec;
	const auto endpoint = socket.remote_endpoint(ec);
	std::string remote = "<unknown>";
	if(!ec)
	{
		remote = endpoint.address().to_string();
		remote += ':';
		remote += std::to_string(endpoint.port());
	}

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

asio::awaitable<void> Session::run()
{
	for(std::size_t served = 0;; ++served)
	{
		if(!co_await read_request()) co_return;

		const auto& req = parser.current();
		const bool keep_alive = req.keep_alive && (served + 1) < settings.max_requests_per_connection;

		auto response = co_await routes.dispatch(req);
		if(!co_await write(http::serialize(response, keep_alive))) co_return;

		if(!keep_alive) co_return;
	}
}

asio::awaitable<bool> Session::read_request()
{
	for(;;)
	{
		if(buffered > 0)
		{
			std::size_t consumed = 0;
			const auto status = parser.execute(buffer.data(), buffered, consumed);

			if(status == http::ParseStatus::complete)
			{
				consume(consumed);
				parser.resume();
				co_return true;
			}

			if(status == http::ParseStatus::error)
			{
				log::warn("[{}] bad request: {}", remote, parser.error_reason());
				co_await write(http::serialize(error_response(400), false));
				co_return false;
			}
		}

		if(buffered == buffer.size())
		{
			log::warn("[{}] request header too large", remote);
			co_await write(http::serialize(error_response(431), false));
			co_return false;
		}

		if(!co_await fill_buffer()) co_return false;
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

asio::awaitable<bool> Session::write(std::string_view text)
{
	using namespace asio::experimental::awaitable_operators;

	asio::steady_timer timer(socket.get_executor(), settings.write_timeout);

	const auto result = co_await (
		timer.async_wait(asio::as_tuple(asio::use_awaitable))
		|| asio::async_write(socket, asio::buffer(text.data(), text.size()), asio::as_tuple(asio::use_awaitable))
	);

	if(result.index() == 0)
	{
		log::warn("[{}] write timeout", remote);
		co_return false;
	}

	const auto& [ec, written] = std::get<1>(result);
	(void)written;
	if(ec)
	{
		throw std::system_error(ec, "write");
	}
	co_return true;
}

void Session::consume(std::size_t n)
{
	const auto remaining = buffered - n;
	if(remaining > 0 && n > 0) std::memmove(buffer.data(), buffer.data() + n, remaining);
	buffered = remaining;
}

} // namespace pylai
