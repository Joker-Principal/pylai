module;
#include <asio.hpp>
#if defined(ASIO_HAS_BOOST_CONTEXT_FIBER)
#include <asio/spawn.hpp>
#endif

export module asio;

// NOLINTBEGIN(misc-unused-using-decls)

export namespace asio {
    // Properties / execution traits
    using asio::prefer;
    using asio::query;
    using asio::require;
    using asio::require_concept;
    using asio::can_prefer;
    using asio::can_query;
    using asio::can_require;
    using asio::can_require_concept;
    using asio::is_nothrow_prefer;
    using asio::is_nothrow_query;
    using asio::is_nothrow_require;
    using asio::is_nothrow_require_concept;
    using asio::prefer_result;
    using asio::query_result;
    using asio::require_result;
    using asio::require_concept_result;

    // Core classes / templates / aliases
    using asio::any_completion_executor;
    using asio::any_io_executor;
    using asio::bad_executor;
    using asio::awaitable;
    // 应用侧要靠它特化 std::coroutine_traits（让带标记的返回类型能当协程返回类型）。
    // asio 的 `awaitable` 本身**没有** promise_type 成员，asio 自己也是用
    // `coroutine_traits<awaitable<T,Executor>, ...>` 指到它的（impl/awaitable.hpp:1082），
    // 所以这是拿到 promise 的唯一入口 → 必须导出（应用规矩：要用 asio 名字就往这里加一行）。
    using asio::detail::awaitable_frame;
    using asio::as_tuple_t;
    using asio::as_tuple;
    using asio::detached_t;
    using asio::detached;
    using asio::deferred_t;
    using asio::deferred;

    using asio::execution_context;
    using asio::executor;
    using asio::executor_arg_t;
    using asio::executor_arg;
    using asio::io_context;
    using asio::thread_pool;
    using asio::static_thread_pool;
    using asio::system_context;
    using asio::system_executor;
    using asio::yield_context;
    using asio::thread;

    using asio::any_completion_handler;
    using asio::any_completion_handler_allocator;
    using asio::append_t;
    using asio::prepend_t;
    using asio::async_completion;
#if !defined(ASIO_NO_DEPRECATED)
    // asio/basic_io_object.hpp (and thus asio::basic_io_object) is excluded
    // when ASIO_NO_DEPRECATED is defined.
    using asio::basic_io_object;
#endif
    using asio::basic_system_executor;
    using asio::basic_yield_context;
    using asio::cancel_after_t;
    using asio::cancel_at_t;
    using asio::cancellation_filter;
    using asio::cancellation_slot_binder;
    using asio::consign_t;
    using asio::disposition_traits;
    using asio::executor_binder;
    using asio::executor_work_guard;
    using asio::partial_allocator_binder;
    using asio::partial_as_tuple;
    using asio::partial_cancel_after;
    using asio::partial_cancel_after_timer;
    using asio::partial_cancel_at;
    using asio::partial_cancel_at_timer;
    using asio::partial_cancellation_slot_binder;
    using asio::partial_executor_binder;
    using asio::partial_immediate_executor_binder;
    using asio::partial_redirect_error;
    using asio::recycling_allocator;
    using asio::redirect_error_t;
    using asio::strand;
    using asio::use_awaitable_t;
    using asio::use_awaitable;
    using asio::use_future_t;
    using asio::use_future;

    // Core free functions
    using asio::append;
    using asio::asio_handler_is_continuation;
    using asio::async_compose;
    using asio::async_immediate;
    using asio::bind_cancellation_slot;
    using asio::bind_allocator;
    using asio::bind_executor;
    using asio::bind_immediate_executor;
    using asio::cancel_after;
    using asio::cancel_at;
    using asio::co_composed;
    using asio::co_spawn;
    using asio::composed;
    using asio::consign;
    using asio::defer;
    using asio::dispatch;
    using asio::get_associated_allocator;
    using asio::get_associated_cancellation_slot;
    using asio::get_associated_executor;
    using asio::get_associated_immediate_executor;
    using asio::make_strand;
    using asio::make_work_guard;
    using asio::post;
    using asio::prepend;
    using asio::redirect_error;
    using asio::spawn;
    using asio::throw_exception;
    using asio::to_exception_ptr;

    // Cancellation / traits
    using asio::cancellation_type;
    using asio::cancellation_slot;
    using asio::cancellation_state;
    using asio::cancellation_signal;
    using asio::enable_terminal_cancellation;
    using asio::async_initiate;
    using asio::async_result;
    using asio::associated_allocator;
    using asio::associated_cancellation_slot;
    using asio::associated_executor;
    using asio::associated_immediate_executor;
    using asio::associator;
    using asio::associated_executor_t;
    using asio::default_completion_token;
    using asio::completion_token_for;
    using asio::completion_signature_of;
    using asio::is_async_operation;
    using asio::is_executor;
    using asio::uses_executor;

    // Buffers
    using asio::streambuf;
    using asio::const_buffer;
    using asio::mutable_buffer;
    using asio::const_registered_buffer;
    using asio::mutable_registered_buffer;
    using asio::null_buffers;
    using asio::registered_buffer_id;
    using asio::basic_streambuf;
    using asio::buffer_registration;
    using asio::buffered_read_stream;
    using asio::buffered_stream;
    using asio::buffered_write_stream;
    using asio::buffers_iterator;
    using asio::dynamic_string_buffer;
    using asio::dynamic_vector_buffer;

    using asio::buffer;
    using asio::buffer_copy;
    using asio::buffer_size;
    using asio::buffer_sequence_begin;
    using asio::buffer_sequence_end;
    using asio::buffers_begin;
    using asio::buffers_end;
    using asio::dynamic_buffer;

    using asio::async_read;
    using asio::async_read_at;
    using asio::async_read_until;
    using asio::async_write;
    using asio::async_write_at;
    using asio::read;
    using asio::read_at;
    using asio::read_until;
    using asio::register_buffers;
    using asio::transfer_all;
    using asio::transfer_at_least;
    using asio::transfer_exactly;
    using asio::write;
    using asio::write_at;

    using asio::is_const_buffer_sequence;
    using asio::is_dynamic_buffer;
    using asio::is_dynamic_buffer_v1;
    using asio::is_dynamic_buffer_v2;
    using asio::is_match_condition;
    using asio::is_mutable_buffer_sequence;
    using asio::is_read_buffered;
    using asio::is_write_buffered;

    // Networking
    using asio::generic::datagram_protocol;
    using asio::generic::raw_protocol;
    using asio::generic::seq_packet_protocol;
    using asio::generic::stream_protocol;
    using asio::basic_datagram_socket;
    using asio::basic_raw_socket;
    using asio::basic_seq_packet_socket;
    using asio::basic_socket;
    using asio::basic_socket_acceptor;
    using asio::basic_socket_iostream;
    using asio::basic_socket_streambuf;
    using asio::basic_stream_socket;
    using asio::socket_base;
    using asio::async_connect;
    using asio::connect;

    // Timers
    using asio::high_resolution_timer;
    using asio::steady_timer;
    using asio::system_timer;
    using asio::basic_waitable_timer;
    using asio::wait_traits;

    // Serial / signal / files
    using asio::serial_port;
    using asio::serial_port_base;
    using asio::basic_serial_port;
    using asio::signal_set;
    using asio::basic_signal_set;
#if defined(ASIO_HAS_FILE)
    using asio::basic_file;
    using asio::basic_random_access_file;
    using asio::basic_stream_file;
    using asio::file_base;
    using asio::random_access_file;
    using asio::stream_file;
#endif
    using asio::basic_readable_pipe;
    using asio::basic_writable_pipe;
    using asio::readable_pipe;
    using asio::writable_pipe;
    using asio::connect_pipe;

    // Special values
    using asio::no_error;

    // Nested namespaces
    namespace ip {
        using asio::ip::tcp;
        using asio::ip::udp;
        using asio::ip::icmp;
        using asio::ip::address;
        using asio::ip::address_v4;
        using asio::ip::address_v4_iterator;
        using asio::ip::address_v4_range;
        using asio::ip::address_v6;
        using asio::ip::address_v6_iterator;
        using asio::ip::address_v6_range;
        using asio::ip::bad_address_cast;
        using asio::ip::network_v4;
        using asio::ip::network_v6;
        using asio::ip::resolver_base;
        using asio::ip::resolver_query_base;
        using asio::ip::v4_mapped_t;
        using asio::ip::basic_endpoint;
        using asio::ip::basic_resolver;
        using asio::ip::basic_resolver_entry;
        using asio::ip::basic_resolver_iterator;
        using asio::ip::basic_resolver_results;
        using asio::ip::basic_resolver_query;
        using asio::ip::host_name;
        using asio::ip::make_address;
        using asio::ip::make_address_v4;
        using asio::ip::make_address_v6;
        using asio::ip::make_network_v4;
        using asio::ip::make_network_v6;

        namespace multicast {
            using asio::ip::multicast::enable_loopback;
            using asio::ip::multicast::hops;
            using asio::ip::multicast::join_group;
            using asio::ip::multicast::leave_group;
            using asio::ip::multicast::outbound_interface;
        }

        namespace unicast {
            using asio::ip::unicast::hops;
        }
    }

    namespace generic {
        using asio::generic::basic_endpoint;
    }

    namespace local {
        using asio::local::seq_packet_protocol;
        using asio::local::stream_protocol;
        using asio::local::datagram_protocol;
        using asio::local::basic_endpoint;
        using asio::local::connect_pair;
    }

    namespace posix {
        using asio::posix::descriptor;
        using asio::posix::descriptor_base;
        using asio::posix::stream_descriptor;
        using asio::posix::basic_descriptor;
        using asio::posix::basic_stream_descriptor;
    }

    namespace windows {
#if defined(ASIO_HAS_WINDOWS_OBJECT_HANDLE)
        using asio::windows::basic_object_handle;
        using asio::windows::object_handle;
#endif
#if defined(ASIO_HAS_WINDOWS_RANDOM_ACCESS_HANDLE)
        using asio::windows::basic_overlapped_handle;
        using asio::windows::overlapped_handle;
        using asio::windows::basic_random_access_handle;
        using asio::windows::random_access_handle;
#endif
#if defined(ASIO_HAS_WINDOWS_OVERLAPPED_PTR)
        using asio::windows::overlapped_ptr;
#endif
#if defined (ASIO_HAS_WINDOWS_STREAM_HANDLE)
        using asio::windows::basic_stream_handle;
        using asio::windows::stream_handle;
#endif
    }

    namespace this_coro {
        using asio::this_coro::cancellation_state_t;
        using asio::this_coro::executor_t;
        using asio::this_coro::reset_cancellation_state;
        using asio::this_coro::throw_if_cancelled;
        using asio::this_coro::cancellation_state;
        using asio::this_coro::executor;
    }

    namespace execution {
        using asio::execution::any_executor;
        using asio::execution::bad_executor;
        using asio::execution::invocable_archetype;
        using asio::execution::is_executor;
        using asio::execution::allocator_t;
        using asio::execution::blocking_t;
        using asio::execution::blocking_adaptation_t;
        using asio::execution::context_t;
        using asio::execution::context_as_t;
        using asio::execution::mapping_t;
        using asio::execution::occupancy_t;
        using asio::execution::outstanding_work_t;
        using asio::execution::prefer_only;
        using asio::execution::relationship_t;
        using asio::execution::allocator;
        using asio::execution::blocking;
        using asio::execution::blocking_adaptation;
        using asio::execution::context;
        using asio::execution::context_as;
        using asio::execution::mapping;
        using asio::execution::occupancy;
        using asio::execution::outstanding_work;
        using asio::execution::relationship;
    }

    namespace error {
        using asio::error::basic_errors;
        using asio::error::netdb_errors;
        using asio::error::addrinfo_errors;
        using asio::error::misc_errors;
        // 普通 completion handler 会实例化 asio 内部 op（如 io_uring_socket_recv_op），
        // 那些模板体里把 asio::error::eof 之类的枚举赋给 std::error_code —— 依赖 ADL 找到
        // make_error_code 重载。不导出它们，TU 侧实例化就会报 "no matching function
        // for call to 'make_error_code'"。
        using asio::error::make_error_code;
    }

    namespace placeholders {
        using asio::placeholders::bytes_transferred;
        using asio::placeholders::endpoint;
        using asio::placeholders::error;
        using asio::placeholders::iterator;
        using asio::placeholders::results;
        using asio::placeholders::signal_number;
    }
}

namespace std {
    using std::coroutine_traits;
}

// NOLINTEND(misc-unused-using-decls)
