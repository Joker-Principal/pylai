module;

#include <concepts>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

#include <spdlog/spdlog.h>

export module pylai:log;

export namespace pylai::log {

template<typename... Args>
struct FormatStr
{
public:
	template<std::convertible_to<std::string_view> StrLike>
	consteval FormatStr(const StrLike& str, std::source_location location = std::source_location::current()) :
		str(str),
		location(location)
	{}

	consteval FormatStr(std::source_location location = std::source_location::current()) : str(), location(location) {}

	spdlog::format_string_t<Args...> str;
	std::source_location location;
};

template<typename... Args>
using Format = FormatStr<std::type_identity_t<Args>...>;

namespace detail {

template<spdlog::level::level_enum threshold, typename... Args>
void log_at(const Format<Args...>& fmt, Args&&... args)
{
	// 编译期过滤：低于阈值的调用（连同其参数求值）整体消失。
	if constexpr(static_cast<int>(threshold) >= SPDLOG_ACTIVE_LEVEL)
	{
		spdlog::default_logger()->log(
			spdlog::source_loc{
				fmt.location.file_name(), static_cast<int>(fmt.location.line()), fmt.location.function_name()
			},
			threshold, fmt.str, std::forward<Args>(args)...
		);
	}
}

} // namespace detail

template<typename... Args>
void trace(const Format<Args...>& fmt, Args&&... args)
{ detail::log_at<spdlog::level::trace>(fmt, std::forward<Args>(args)...); }

template<typename... Args>
void debug(const Format<Args...>& fmt, Args&&... args)
{ detail::log_at<spdlog::level::debug>(fmt, std::forward<Args>(args)...); }

template<typename... Args>
void info(const Format<Args...>& fmt, Args&&... args)
{ detail::log_at<spdlog::level::info>(fmt, std::forward<Args>(args)...); }

template<typename... Args>
void warn(const Format<Args...>& fmt, Args&&... args)
{ detail::log_at<spdlog::level::warn>(fmt, std::forward<Args>(args)...); }

template<typename... Args>
void err(const Format<Args...>& fmt, Args&&... args)
{ detail::log_at<spdlog::level::err>(fmt, std::forward<Args>(args)...); }

template<typename... Args>
void critical(const Format<Args...>& fmt, Args&&... args)
{ detail::log_at<spdlog::level::critical>(fmt, std::forward<Args>(args)...); }

// 配置默认 logger（pattern / 级别 / flush 策略）。
void init();

} // namespace pylai::log
