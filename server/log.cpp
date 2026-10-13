module;

#include <chrono>
#include <memory>
#include <utility>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

module pylai;

import :log;

namespace pylai::log {

void init()
{
	auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	auto logger = std::make_shared<spdlog::logger>("pylai", std::move(sink));

	logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%^%l%$][%s:%#] %v");
	logger->set_level(spdlog::level::trace);
	logger->flush_on(spdlog::level::trace);

	spdlog::set_default_logger(std::move(logger));
}

} // namespace pylai::log
