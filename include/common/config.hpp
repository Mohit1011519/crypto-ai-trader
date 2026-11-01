#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace crypto_trader::common {

struct AppConfig {
    std::string exchange;
    std::string mode;
    std::string rest_base_url;
    std::string websocket_host;
    std::string websocket_port;
    std::string websocket_path;
    std::vector<std::string> trading_pairs;
    std::string candle_interval;
    int candle_limit;
    int request_timeout_seconds;
    int reconnect_delay_seconds;
    std::string log_level;
};

AppConfig parse_config(const nlohmann::json& json);
AppConfig load_config(const std::filesystem::path& path);

}  // namespace crypto_trader::common
