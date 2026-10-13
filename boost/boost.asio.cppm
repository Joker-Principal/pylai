module;
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/detail/handler_cont_helpers.hpp>

export module boost.asio;

export namespace boost {
    namespace asio {
        // Properties / execution traits
        using boost::asio::prefer;
        using boost::asio::query;
        using boost::asio::require;
        using boost::asio::require_concept;
        using boost::asio::can_prefer;
        using boost::asio::can_query;
        using boost::asio::can_require;
        using boost::asio::can_require_concept;
        using boost::asio::is_nothrow_prefer;
        using boost::asio::is_nothrow_query;
        using boost::asio::is_nothrow_require;
        using boost::asio::is_nothrow_require_concept;
        using boost::asio::prefer_result;
        using boost::asio::query_result;
        using boost::asio::require_result;
        using boost::asio::require_concept_result;

        // Core classes / templates / aliases
        using boost::asio::any_completion_executor;
        using boost::asio::any_io_executor;
        using boost::asio::bad_executor;
        using boost::asio::awaitable;
        using boost::asio::as_tuple_t;
        using boost::asio::as_tuple;
        using boost::asio::detached_t;
        using boost::asio::detached;
        using boost::asio::deferred_t;
        using boost::asio::deferred;

        using boost::asio::execution_context;
        using boost::asio::executor;
        using boost::asio::executor_arg_t;
        using boost::asio::executor_arg;
        using boost::asio::io_context;
        using boost::asio::thread_pool;
        using boost::asio::static_thread_pool;
        using boost::asio::system_context;
        using boost::asio::system_executor;
        using boost::asio::yield_context;
        using boost::asio::thread;

        using boost::asio::any_completion_handler;
        using boost::asio::any_completion_handler_allocator;
        using boost::asio::append_t;
        using boost::asio::prepend_t;
        using boost::asio::async_completion;
        using boost::asio::basic_io_object;
        using boost::asio::basic_system_executor;
        using boost::asio::basic_yield_context;
        using boost::asio::cancel_after_t;
        using boost::asio::cancel_at_t;
        using boost::asio::cancellation_filter;
        using boost::asio::cancellation_slot_binder;
        using boost::asio::consign_t;
        using boost::asio::disposition_traits;
        using boost::asio::executor_binder;
        using boost::asio::executor_work_guard;
        using boost::asio::partial_allocator_binder;
        using boost::asio::partial_as_tuple;
        using boost::asio::partial_cancel_after;
        using boost::asio::partial_cancel_after_timer;
        using boost::asio::partial_cancel_at;
        using boost::asio::partial_cancel_at_timer;
        using boost::asio::partial_cancellation_slot_binder;
        using boost::asio::partial_executor_binder;
        using boost::asio::partial_immediate_executor_binder;
        using boost::asio::partial_redirect_error;
        using boost::asio::recycling_allocator;
        using boost::asio::redirect_error_t;
        using boost::asio::strand;
        using boost::asio::use_awaitable_t;
        using boost::asio::use_awaitable;
        using boost::asio::use_future_t;
        using boost::asio::use_future;

        // Core free functions
        using boost::asio::append;
        using boost::asio::asio_handler_is_continuation;
        using boost::asio::async_compose;
        using boost::asio::async_immediate;
        using boost::asio::bind_cancellation_slot;
        using boost::asio::bind_allocator;
        using boost::asio::bind_executor;
        using boost::asio::bind_immediate_executor;
        using boost::asio::cancel_after;
        using boost::asio::cancel_at;
        using boost::asio::co_composed;
        using boost::asio::co_spawn;
        using boost::asio::composed;
        using boost::asio::consign;
        using boost::asio::defer;
        using boost::asio::dispatch;
        using boost::asio::get_associated_allocator;
        using boost::asio::get_associated_cancellation_slot;
        using boost::asio::get_associated_executor;
        using boost::asio::get_associated_immediate_executor;
        using boost::asio::make_strand;
        using boost::asio::make_work_guard;
        using boost::asio::post;
        using boost::asio::prepend;
        using boost::asio::redirect_error;
        using boost::asio::spawn;
        using boost::asio::throw_exception;
        using boost::asio::to_exception_ptr;

        // Cancellation / traits
        using boost::asio::cancellation_type;
        using boost::asio::cancellation_slot;
        using boost::asio::cancellation_state;
        using boost::asio::cancellation_signal;
        using boost::asio::enable_terminal_cancellation;
        using boost::asio::async_initiate;
        using boost::asio::async_result;
        using boost::asio::associated_allocator;
        using boost::asio::associated_cancellation_slot;
        using boost::asio::associated_executor;
        using boost::asio::associated_immediate_executor;
        using boost::asio::associator;
        using boost::asio::associated_executor_t;
        using boost::asio::default_completion_token;
        using boost::asio::completion_token_for;
        using boost::asio::completion_signature_of;
        using boost::asio::is_async_operation;
        using boost::asio::is_executor;
        using boost::asio::uses_executor;

        // Buffers
        using boost::asio::streambuf;
        using boost::asio::const_buffer;
        using boost::asio::mutable_buffer;
        using boost::asio::const_registered_buffer;
        using boost::asio::mutable_registered_buffer;
        using boost::asio::null_buffers;
        using boost::asio::registered_buffer_id;
        using boost::asio::basic_streambuf;
        using boost::asio::buffer_registration;
        using boost::asio::buffered_read_stream;
        using boost::asio::buffered_stream;
        using boost::asio::buffered_write_stream;
        using boost::asio::buffers_iterator;
        using boost::asio::dynamic_string_buffer;
        using boost::asio::dynamic_vector_buffer;

        using boost::asio::buffer;
        using boost::asio::buffer_copy;
        using boost::asio::buffer_size;
        using boost::asio::buffer_sequence_begin;
        using boost::asio::buffer_sequence_end;
        using boost::asio::buffers_begin;
        using boost::asio::buffers_end;
        using boost::asio::dynamic_buffer;

        using boost::asio::async_read;
        using boost::asio::async_read_at;
        using boost::asio::async_read_until;
        using boost::asio::async_write;
        using boost::asio::async_write_at;
        using boost::asio::read;
        using boost::asio::read_at;
        using boost::asio::read_until;
        using boost::asio::register_buffers;
        using boost::asio::transfer_all;
        using boost::asio::transfer_at_least;
        using boost::asio::transfer_exactly;
        using boost::asio::write;
        using boost::asio::write_at;

        using boost::asio::is_const_buffer_sequence;
        using boost::asio::is_dynamic_buffer;
        using boost::asio::is_dynamic_buffer_v1;
        using boost::asio::is_dynamic_buffer_v2;
        using boost::asio::is_match_condition;
        using boost::asio::is_mutable_buffer_sequence;
        using boost::asio::is_read_buffered;
        using boost::asio::is_write_buffered;

        // Networking
        using boost::asio::generic::datagram_protocol;
        using boost::asio::generic::raw_protocol;
        using boost::asio::generic::seq_packet_protocol;
        using boost::asio::generic::stream_protocol;
        using boost::asio::basic_datagram_socket;
        using boost::asio::basic_raw_socket;
        using boost::asio::basic_seq_packet_socket;
        using boost::asio::basic_socket;
        using boost::asio::basic_socket_acceptor;
        using boost::asio::basic_socket_iostream;
        using boost::asio::basic_socket_streambuf;
        using boost::asio::basic_stream_socket;
        using boost::asio::socket_base;
        using boost::asio::async_connect;
        using boost::asio::connect;

        // Timers
        using boost::asio::deadline_timer;
        using boost::asio::high_resolution_timer;
        using boost::asio::steady_timer;
        using boost::asio::system_timer;
        using boost::asio::basic_deadline_timer;
        using boost::asio::basic_waitable_timer;
        using boost::asio::time_traits;
        using boost::asio::wait_traits;

        // Serial / signal / files
        using boost::asio::serial_port;
        using boost::asio::serial_port_base;
        using boost::asio::basic_serial_port;
        using boost::asio::signal_set;
        using boost::asio::basic_signal_set;
        using boost::asio::basic_file;
        using boost::asio::basic_random_access_file;
        using boost::asio::basic_readable_pipe;
        using boost::asio::basic_stream_file;
        using boost::asio::basic_writable_pipe;
        using boost::asio::file_base;
        using boost::asio::random_access_file;
        using boost::asio::readable_pipe;
        using boost::asio::stream_file;
        using boost::asio::writable_pipe;
        using boost::asio::connect_pipe;

        // Special values
        using boost::asio::no_error;

        // Nested namespaces
        namespace ip {
            using boost::asio::ip::tcp;
            using boost::asio::ip::udp;
            using boost::asio::ip::icmp;
            using boost::asio::ip::address;
            using boost::asio::ip::address_v4;
            using boost::asio::ip::address_v4_iterator;
            using boost::asio::ip::address_v4_range;
            using boost::asio::ip::address_v6;
            using boost::asio::ip::address_v6_iterator;
            using boost::asio::ip::address_v6_range;
            using boost::asio::ip::bad_address_cast;
            using boost::asio::ip::network_v4;
            using boost::asio::ip::network_v6;
            using boost::asio::ip::resolver_base;
            using boost::asio::ip::resolver_query_base;
            using boost::asio::ip::v4_mapped_t;
            using boost::asio::ip::basic_endpoint;
            using boost::asio::ip::basic_resolver;
            using boost::asio::ip::basic_resolver_entry;
            using boost::asio::ip::basic_resolver_iterator;
            using boost::asio::ip::basic_resolver_results;
            using boost::asio::ip::basic_resolver_query;
            using boost::asio::ip::host_name;
            using boost::asio::ip::make_address;
            using boost::asio::ip::make_address_v4;
            using boost::asio::ip::make_address_v6;
            using boost::asio::ip::make_network_v4;
            using boost::asio::ip::make_network_v6;

            namespace multicast {
                using boost::asio::ip::multicast::enable_loopback;
                using boost::asio::ip::multicast::hops;
                using boost::asio::ip::multicast::join_group;
                using boost::asio::ip::multicast::leave_group;
                using boost::asio::ip::multicast::outbound_interface;
            }

            namespace unicast {
                using boost::asio::ip::unicast::hops;
            }
        }

        namespace generic {
            using boost::asio::generic::basic_endpoint;
        }

        namespace local {
            using boost::asio::local::seq_packet_protocol;
            using boost::asio::local::stream_protocol;
            using boost::asio::local::datagram_protocol;
            using boost::asio::local::basic_endpoint;
            using boost::asio::local::connect_pair;
        }

        namespace posix {
            using boost::asio::posix::descriptor;
            using boost::asio::posix::descriptor_base;
            using boost::asio::posix::stream_descriptor;
            using boost::asio::posix::basic_descriptor;
            using boost::asio::posix::basic_stream_descriptor;
        }

        namespace windows {
            using boost::asio::windows::object_handle;
            using boost::asio::windows::overlapped_handle;
            using boost::asio::windows::overlapped_ptr;
            using boost::asio::windows::random_access_handle;
            using boost::asio::windows::stream_handle;
            using boost::asio::windows::basic_object_handle;
            using boost::asio::windows::basic_overlapped_handle;
            using boost::asio::windows::basic_random_access_handle;
            using boost::asio::windows::basic_stream_handle;
        }

        namespace this_coro {
            using boost::asio::this_coro::cancellation_state_t;
            using boost::asio::this_coro::executor_t;
            using boost::asio::this_coro::reset_cancellation_state;
            using boost::asio::this_coro::throw_if_cancelled;
            using boost::asio::this_coro::cancellation_state;
            using boost::asio::this_coro::executor;
        }

        namespace execution {
            using boost::asio::execution::any_executor;
            using boost::asio::execution::bad_executor;
            using boost::asio::execution::invocable_archetype;
            using boost::asio::execution::is_executor;
            using boost::asio::execution::allocator_t;
            using boost::asio::execution::blocking_t;
            using boost::asio::execution::blocking_adaptation_t;
            using boost::asio::execution::context_t;
            using boost::asio::execution::context_as_t;
            using boost::asio::execution::mapping_t;
            using boost::asio::execution::occupancy_t;
            using boost::asio::execution::outstanding_work_t;
            using boost::asio::execution::prefer_only;
            using boost::asio::execution::relationship_t;
            using boost::asio::execution::allocator;
            using boost::asio::execution::blocking;
            using boost::asio::execution::blocking_adaptation;
            using boost::asio::execution::context;
            using boost::asio::execution::context_as;
            using boost::asio::execution::mapping;
            using boost::asio::execution::occupancy;
            using boost::asio::execution::outstanding_work;
            using boost::asio::execution::relationship;
        }

        namespace error {
            using boost::asio::error::basic_errors;
            using boost::asio::error::netdb_errors;
            using boost::asio::error::addrinfo_errors;
            using boost::asio::error::misc_errors;
        }

        namespace placeholders {
            using boost::asio::placeholders::bytes_transferred;
            using boost::asio::placeholders::endpoint;
            using boost::asio::placeholders::error;
            using boost::asio::placeholders::iterator;
            using boost::asio::placeholders::results;
            using boost::asio::placeholders::signal_number;
        }

        namespace ssl {
            using boost::asio::ssl::context;
            using boost::asio::ssl::context_base;
            using boost::asio::ssl::host_name_verification;
            using boost::asio::ssl::stream;
            using boost::asio::ssl::stream_base;
            using boost::asio::ssl::verify_mode;
            using boost::asio::ssl::verify_context;

            namespace error {
                using boost::asio::ssl::error::stream_errors;
            }
        }

        namespace experimental {
            using boost::asio::experimental::as_single_t;
            using boost::asio::experimental::basic_channel;
            using boost::asio::experimental::basic_concurrent_channel;
            using boost::asio::experimental::channel_traits;
            using boost::asio::experimental::coro;
            using boost::asio::experimental::parallel_group;
            using boost::asio::experimental::promise;
            using boost::asio::experimental::ranged_parallel_group;
            using boost::asio::experimental::use_coro_t;
            using boost::asio::experimental::use_promise_t;
            using boost::asio::experimental::make_parallel_group;
            using boost::asio::experimental::wait_for_all;
            using boost::asio::experimental::wait_for_one;
            using boost::asio::experimental::wait_for_one_error;
            using boost::asio::experimental::wait_for_one_success;
            using boost::asio::experimental::as_single;
            using boost::asio::experimental::use_coro;
            using boost::asio::experimental::use_promise;
        }
    }
}
export namespace boost_asio_handler_cont_helpers {
    using boost_asio_handler_cont_helpers::is_continuation;
}
namespace std {
    export template <typename T, typename Executor, typename... Args>
    struct coroutine_traits<boost::asio::awaitable<T, Executor>, Args...>;
}
