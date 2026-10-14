// 编译期护栏：两侧字节通道必须满足 asio 的 `AsyncReadStream` / `AsyncWriteStream`。
//
// 怎么验的：直接拿 asio 的**组合操作**去消费它们 —— `asio::async_read` / `asio::async_write` 会用到
// 概念的每一个要求（`get_executor()` + `async_read_some` / `async_write_some`）；
// 只要缺一个，这里就编不过（比写断言更硬：断言也是抄概念，这里是让 asio 自己去查）。
//
// ⚠️ 这些函数**不被调用**，只要求编得过 —— 因为通道是抽象类，运行期只有 `Session` 自己实现它们。
#include <coroutine>
#include <cstddef>

import asio;
import pylai;

namespace {

// 读侧：`http::BodyReader`（handler 通过 `req` 拿到的是同一个接口）
[[maybe_unused]] asio::awaitable<void> probe_read_stream(pylai::http::BodyReader& reader)
{
	std::byte buffer[8]{};
	co_await asio::async_read(reader, asio::buffer(buffer), asio::use_awaitable);
}

// 读侧：`http::Request` 转发同一套接口 ⇒ `asio::async_read(req, …)` 能直接吃它
[[maybe_unused]] asio::awaitable<void> probe_request_stream(pylai::http::Request& request)
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
