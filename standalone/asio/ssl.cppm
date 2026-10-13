module;
#include <asio/ssl.hpp>

export module asio.ssl;

// NOLINTBEGIN(misc-unused-using-decls)
export namespace asio {
    namespace ssl {
        using asio::ssl::context;
        using asio::ssl::context_base;
        using asio::ssl::host_name_verification;
        using asio::ssl::stream;
        using asio::ssl::stream_base;
        using asio::ssl::verify_context;
        using asio::ssl::verify_mode;
    }
}
// NOLINTEND(misc-unused-using-decls)
