module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

export module pylai:config;

export namespace pylai {

struct Settings
{
	std::string bind_address{"127.0.0.1"};
	std::uint16_t port{18080};

	std::size_t io_threads{1};

	std::chrono::seconds idle_timeout{30};
	std::chrono::seconds write_timeout{30};

	std::size_t max_requests_per_connection{1000};

	// 请求体上限：超过即中止解析 → 413 + 关连接。Stage 0 取保守固定值，防无界内存。
	std::size_t max_body_bytes{1024 * 1024};
};

} // namespace pylai
