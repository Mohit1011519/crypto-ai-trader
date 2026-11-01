#include "common/config.hpp"

#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace crypto_trader::common {
namespace {

void require_non_empty(const std::string& value, const char* name) {
    if (value.empty()) {
        throw std::invalid_argument(std::string("Configuration field '") + name + "' must not be empty");
    }
}

bool valid_symbol(const std::string& symbol) {
    if (symbol.empty()) {
        return false;
    }
    for (const char character : symbol) {
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9'))) {
            return false;
        }
    }
    return true;
}

}  // namespace

AppConfig parse_config(const nlohmann::json& json) {
    AppConfig config{
        .exchange = json.at("exchange").get<std::string>(),
        .mode = json.at("mode").get<std::string>(),
        .rest_base_url = json.at("rest_base_url").get<std::string>(),
        .websocket_host = json.at("websocket_host").get<std::string>(),
        .websocket_port = json.at("websocket_port").get<std::string>(),
        .websocket_path = json.at("websocket_path").get<std::string>(),
        .trading_pairs = json.at("trading_pairs").get<std::vector<std::string>>(),
        .candle_interval = json.at("candle_interval").get<std::string>(),
        .candle_limit = json.at("candle_limit").get<int>(),
        .request_timeout_seconds = json.at("request_timeout_seconds").get<int>(),
        .reconnect_delay_seconds = json.at("reconnect_delay_seconds").get<int>(),
        .log_level = json.at("log_level").get<std::string>()
    };

    require_non_empty(config.exchange, "exchange");
    require_non_empty(config.mode, "mode");
    require_non_empty(config.rest_base_url, "rest_base_url");
    require_non_empty(config.websocket_host, "websocket_host");
    require_non_empty(config.websocket_port, "websocket_port");
    require_non_empty(config.websocket_path, "websocket_path");
    require_non_empty(config.candle_interval, "candle_interval");
    require_non_empty(config.log_level, "log_level");

    if (config.exchange != "binance") {
        throw std::invalid_argument("Only the 'binance' exchange is supported in Phase 1");
    }
    if (config.mode != "paper") {
        throw std::invalid_argument("Phase 1 supports only paper mode; live trading is not implemented");
    }
    if (config.trading_pairs.empty()) {
        throw std::invalid_argument("Configuration field 'trading_pairs' must contain at least one pair");
    }
    std::set<std::string> unique_pairs;
    for (const auto& pair : config.trading_pairs) {
        if (!valid_symbol(pair)) {
            throw std::invalid_argument("Trading pairs must contain only uppercase letters and digits: " + pair);
        }
        if (!unique_pairs.insert(pair).second) {
            throw std::invalid_argument("Duplicate trading pair in configuration: " + pair);
        }
    }
    if (config.candle_limit < 1 || config.candle_limit > 1000) {
        throw std::invalid_argument("candle_limit must be between 1 and 1000");
    }
    if (config.request_timeout_seconds < 1) {
        throw std::invalid_argument("request_timeout_seconds must be positive");
    }
    if (config.reconnect_delay_seconds < 1) {
        throw std::invalid_argument("reconnect_delay_seconds must be positive");
    }
    if (config.websocket_port.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("websocket_port must be numeric");
    }
    const auto port = std::stoul(config.websocket_port);
    if (port == 0 || port > std::numeric_limits<unsigned short>::max()) {
        throw std::invalid_argument("websocket_port must be between 1 and 65535");
    }
    if (config.websocket_path.front() != '/') {
        throw std::invalid_argument("websocket_path must begin with '/'");
    }
    if (config.websocket_path.size() > 1 && config.websocket_path.back() == '/') {
        throw std::invalid_argument("websocket_path must not end with '/'");
    }
    constexpr std::string_view https_prefix = "https://";
    if (config.rest_base_url.compare(0, https_prefix.size(), https_prefix) != 0) {
        throw std::invalid_argument("rest_base_url must use HTTPS");
    }
    while (config.rest_base_url.size() > https_prefix.size() &&
           config.rest_base_url.back() == '/') {
        config.rest_base_url.pop_back();
    }

    return config;
}

AppConfig load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Unable to open configuration file: " + path.string());
    }

    nlohmann::json json;
    input >> json;
    if (!json.is_object()) {
        throw std::invalid_argument("Configuration root must be a JSON object");
    }
    return parse_config(json);
}

}  // namespace crypto_trader::common
