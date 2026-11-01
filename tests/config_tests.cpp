#include "common/config.hpp"

#include <stdexcept>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

nlohmann::json valid_config() {
    return {
        {"exchange", "binance"},
        {"mode", "paper"},
        {"rest_base_url", "https://api.binance.com"},
        {"websocket_host", "stream.binance.com"},
        {"websocket_port", "9443"},
        {"websocket_path", "/ws"},
        {"trading_pairs", {"BTCUSDT", "ETHUSDT"}},
        {"candle_interval", "1m"},
        {"candle_limit", 500},
        {"request_timeout_seconds", 15},
        {"reconnect_delay_seconds", 5},
        {"log_level", "info"}
    };
}

}  // namespace

TEST(ConfigTests, ParsesValidPaperConfiguration) {
    const auto config = crypto_trader::common::parse_config(valid_config());

    EXPECT_EQ(config.exchange, "binance");
    EXPECT_EQ(config.mode, "paper");
    EXPECT_EQ(config.trading_pairs.size(), 2);
    EXPECT_EQ(config.candle_limit, 500);
}

TEST(ConfigTests, RejectsLiveModeInPhaseOne) {
    auto json = valid_config();
    json["mode"] = "live";

    EXPECT_THROW(crypto_trader::common::parse_config(json), std::invalid_argument);
}

TEST(ConfigTests, RejectsOutOfRangeCandleLimit) {
    auto json = valid_config();
    json["candle_limit"] = 1001;

    EXPECT_THROW(crypto_trader::common::parse_config(json), std::invalid_argument);
}

TEST(ConfigTests, RejectsMalformedOrDuplicatePairs) {
    auto malformed = valid_config();
    malformed["trading_pairs"] = {"BTC/USDT"};
    EXPECT_THROW(crypto_trader::common::parse_config(malformed), std::invalid_argument);

    auto duplicate = valid_config();
    duplicate["trading_pairs"] = {"BTCUSDT", "BTCUSDT"};
    EXPECT_THROW(crypto_trader::common::parse_config(duplicate), std::invalid_argument);
}
