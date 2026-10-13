module;
#include <asio/experimental/parallel_group.hpp>
#include <asio/experimental/as_single.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/experimental/use_coro.hpp>
#include <asio/experimental/promise.hpp>
#include <asio/experimental/use_promise.hpp>

export module asio.experimental;

// NOLINTBEGIN(misc-unused-using-decls)
export namespace asio {
    namespace experimental {
        // `co_await (a || b)` / `(a && b)` —— 协程并发的组合子。
        // 使用方仍需 `using namespace asio::experimental::awaitable_operators;`
        // （ADL 不会主动进入这个嵌套命名空间）。
        namespace awaitable_operators {
            using asio::experimental::awaitable_operators::operator&&;
            using asio::experimental::awaitable_operators::operator||;
        }
        using asio::experimental::as_single_t;
        using asio::experimental::basic_channel;
        using asio::experimental::basic_concurrent_channel;
        using asio::experimental::channel_traits;
        using asio::experimental::coro;
        using asio::experimental::parallel_group;
        using asio::experimental::promise;
        using asio::experimental::ranged_parallel_group;
        using asio::experimental::use_coro_t;
        using asio::experimental::use_promise_t;
        using asio::experimental::make_parallel_group;
        using asio::experimental::wait_for_all;
        using asio::experimental::wait_for_one;
        using asio::experimental::wait_for_one_error;
        using asio::experimental::wait_for_one_success;
        using asio::experimental::as_single;
        using asio::experimental::use_coro;
        using asio::experimental::use_promise;
    }
}
// NOLINTEND(misc-unused-using-decls)
