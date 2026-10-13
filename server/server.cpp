module;

#include <coroutine>
#include <cassert>
#include <csignal>
#include <cstddef>
#include <exception>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

module pylai;

import asio;
import :config;
import :http;
import :log;
import :route;
import :server;
import :session;

namespace pylai {

namespace {
constexpr int max_consecutive_accept_errors = 5;
}

Server::Builder& Server::Builder::bind(std::string address, std::uint16_t port)
{
	settings.bind_address = std::move(address);
	settings.port = port;
	return *this;
}

Server::Builder& Server::Builder::io_threads(std::size_t count)
{
	settings.io_threads = count;
	return *this;
}

Server::Builder& Server::Builder::idle_timeout(std::chrono::seconds timeout)
{
	settings.idle_timeout = timeout;
	return *this;
}

Server::Builder& Server::Builder::write_timeout(std::chrono::seconds timeout)
{
	settings.write_timeout = timeout;
	return *this;
}

Server::Builder& Server::Builder::max_requests_per_connection(std::size_t count)
{
	settings.max_requests_per_connection = count;
	return *this;
}

Server Server::Builder::build() { return Server(std::move(settings), std::move(routes)); }

Server Server::Builder::Configured::build() { return Server(std::move(settings_), std::move(routes_)); }

Server::Builder Server::builder() { return Builder{}; }

Server::Server(Settings settings, route::Router routes) : settings(std::move(settings)), routes(std::move(routes))
{
	assert(settings.io_threads == 1 && "Server 目前要求 io_threads == 1（见 docs/asio-notes.md）");
}

Server::~Server() = default;

int Server::run()
{
	log::init();

	asio::signal_set signals(context, SIGINT, SIGTERM);
	signals.async_wait([this](std::error_code ec, int signal_number) {
		if(ec) return;
		log::info("signal {}, shutting down", signal_number);
		quit();
	});

	asio::co_spawn(context, accept_loop(), [this, &signals](std::exception_ptr ep) {
		if(ep)
		{
			try
			{
				std::rethrow_exception(ep);
			}
			catch(const std::exception& e)
			{
				log::critical("fatal: {}", e.what());
			}
			catch(...)
			{
				log::critical("fatal: unknown exception");
			}
		}
		std::error_code ignored;
		signals.cancel(ignored);
		context.stop();
	});

	std::vector<std::thread> threads;
	const std::size_t extra = settings.io_threads > 1 ? settings.io_threads - 1 : 0;
	threads.reserve(extra);
	for(std::size_t i = 0; i < extra; ++i) threads.emplace_back([this] { context.run(); });

	context.run();

	for(auto& thread : threads) thread.join();

	log::info("stopped");
	return 0;
}

void Server::quit()
{
	accept_signal.emit(asio::cancellation_type::all);
	for(auto& signal : sessions) signal.emit(asio::cancellation_type::all);
}

asio::awaitable<void> Server::accept_loop()
{
	auto& ioc = context;
	const auto& cfg = settings;

	asio::ip::tcp::acceptor acceptor(ioc, {asio::ip::make_address(cfg.bind_address), cfg.port});

	const auto local = acceptor.local_endpoint();
	log::info("listening on {}:{} (io threads: {})", local.address().to_string(), local.port(), cfg.io_threads);

	int consecutive_errors = 0;
	for(;;)
	{
		auto [ec, socket] = co_await acceptor.async_accept(
			asio::bind_cancellation_slot(accept_signal.slot(), asio::as_tuple(asio::use_awaitable))
		);

		if(ec == asio::error::operation_aborted)
		{
			log::info("accept cancelled, shutting down");
			co_return;
		}
		if(ec)
		{
			log::err("accept error: {}", ec.message());
			if(++consecutive_errors >= max_consecutive_accept_errors)
			{
				log::err("too many consecutive accept errors");
				context.stop();
				co_return;
			}
			continue;
		}
		consecutive_errors = 0;

		const auto handle = sessions.emplace();

		asio::co_spawn(
			ioc, Session::handle(settings, routes, std::move(socket)),
			asio::bind_cancellation_slot(handle->slot(), [this, handle](std::exception_ptr ep) {
				if(ep)
				{
					try
					{
						std::rethrow_exception(ep);
					}
					catch(const std::exception& e)
					{
						log::err("session escaped: {}", e.what());
					}
					catch(...)
					{
						log::err("session escaped: unknown exception");
					}
				}

				// 延后一拍：本回调可能正由该 signal 的取消路径调用，handler 尚未退栈。
				asio::post(context, [this, handle] { sessions.erase(handle); });
			})
		);
	}
}

} // namespace pylai
