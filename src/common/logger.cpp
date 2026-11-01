#include "common/logger.hpp"

#include <stdexcept>

#include <filesystem>
#include <memory>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace crypto_trader::common {

void initialize_logging(const std::string& log_level, const std::string& log_file) {
    const auto level = spdlog::level::from_str(log_level);
    if (level == spdlog::level::off && log_level != "off") {
        throw std::invalid_argument("Unsupported log_level: " + log_level);
    }

    std::vector<spdlog::sink_ptr> sinks{
        std::make_shared<spdlog::sinks::stdout_color_sink_mt>()};
    if (!log_file.empty()) {
        const auto parent = std::filesystem::path(log_file).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file, 5 * 1024 * 1024, 3));
    }
    auto logger = std::make_shared<spdlog::logger>("trader", sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] %v");
    spdlog::set_level(level);
    spdlog::flush_on(spdlog::level::info);
}

}  // namespace crypto_trader::common
