#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <tuple>
#include <typeinfo>

import pylai;

namespace {

// 读侧：`http::BodyReader`（handler 通过 `req` 拿到的是同一个接口）
[[maybe_unused]] asio::awaitable<void> probe_read_stream(pylai::http::BodyReader& reader)
{
	std::byte buffer[8]{};
	co_await asio::async_read(reader, asio::buffer(buffer), asio::use_awaitable);
}

// 读侧：`route::Request`（handler 拿到的视图）持着同一个接口 ⇒ `asio::async_read(req, …)` 能直接吃它
[[maybe_unused]] asio::awaitable<void> probe_request_stream(pylai::route::Get<"/x">& request)
{
	std::byte buffer[8]{};
	co_await asio::async_read(request, asio::buffer(buffer), asio::use_awaitable);
}

// 写侧：`http::ResponseWriter`
[[maybe_unused]] asio::awaitable<void> probe_write_stream(pylai::http::ResponseWriter& writer)
{
	co_await asio::async_write(writer, asio::buffer("x"), asio::use_awaitable);
}

} // namespace
