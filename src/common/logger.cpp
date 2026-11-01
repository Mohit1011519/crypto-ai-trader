#include "common/logger.hpp"

#include <stdexcept>

#include <spdlog/spdlog.h>

namespace crypto_trader::common {

void initialize_logging(const std::string& log_level) {
    const auto level = spdlog::level::from_str(log_level);
    if (level == spdlog::level::off && log_level != "off") {
        throw std::invalid_argument("Unsupported log_level: " + log_level);
    }

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(level);
}

}  // namespace crypto_trader::common
