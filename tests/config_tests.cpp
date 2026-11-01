#include "common/config.hpp"

#include <stdexcept>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

nlohmann::json valid_config() {
    return {
        {"exchange", "binance"},
        {"mode", "paper"},
        {"live_order_submission_enabled", false},
        {"paper_initial_balance", 10000.0},
        {"paper_state_path", "data/paper_portfolio.json"},
        {"rest_base_url", "https://api.binance.com"},
        {"websocket_host", "stream.binance.com"},
        {"websocket_port", "9443"},
        {"websocket_path", "/ws"},
        {"trading_pairs", {"BTCUSDT", "ETHUSDT"}},
        {"candle_interval", "1m"},
        {"candle_limit", 500},
        {"request_timeout_seconds", 15},
        {"reconnect_delay_seconds", 5},
        {"log_level", "info"},
        {"research", {
            {"ema_fast_period", 20},
            {"ema_medium_period", 50},
            {"ema_slow_period", 200},
            {"rsi_period", 14},
            {"macd_fast_period", 12},
            {"macd_slow_period", 26},
            {"macd_signal_period", 9},
            {"bollinger_period", 20},
            {"bollinger_standard_deviations", 2.0},
            {"atr_period", 14},
            {"stochastic_rsi_period", 14},
            {"volume_average_period", 20},
            {"volume_spike_multiple", 2.0},
            {"support_resistance_period", 50},
            {"historical_volatility_period", 30},
            {"normal_volatility_threshold", 0.01},
            {"high_volatility_threshold", 0.03},
            {"sudden_volatility_multiple", 2.5},
            {"order_book_depth", 20},
            {"large_order_multiple", 3.0},
            {"minimum_quote_liquidity", 10000.0},
            {"scan_interval_seconds", 60}
        }},
        {"signals", {
            {"weights", {
                {"trend", 0.25},
                {"momentum", 0.30},
                {"volume", 0.15},
                {"volatility", 0.10},
                {"order_book", 0.20},
                {"sentiment", 0.0}
            }},
            {"minimum_signal_score", 0.35},
            {"minimum_research_quality", 0.80},
            {"minimum_research_coverage", 0.65},
            {"minimum_confirmations", 2},
            {"minimum_reward_risk_ratio", 1.50},
            {"target_atr_multiple", 3.0},
            {"stop_atr_multiple", 1.5},
            {"taker_fee_rate", 0.001},
            {"slippage_rate", 0.0005},
            {"maximum_spread_percent", 0.005}
        }},
        {"risk", {
            {"risk_per_trade_fraction", 0.005},
            {"maximum_open_positions", 3},
            {"daily_maximum_loss_fraction", 0.02},
            {"maximum_drawdown_fraction", 0.10},
            {"maximum_gross_exposure_fraction", 1.0},
            {"stop_atr_multiple", 1.5},
            {"target_atr_multiple", 3.0},
            {"taker_fee_rate", 0.001},
            {"slippage_rate", 0.0005}
        }}
    };
}

}  // namespace

TEST(ConfigTests, ParsesValidPaperConfiguration) {
    const auto config = crypto_trader::common::parse_config(valid_config());

    EXPECT_EQ(config.exchange, "binance");
    EXPECT_EQ(config.mode, "paper");
    EXPECT_EQ(config.trading_pairs.size(), 2);
    EXPECT_EQ(config.candle_limit, 500);
    EXPECT_DOUBLE_EQ(config.signals.momentum_weight, 0.30);
    EXPECT_DOUBLE_EQ(config.signals.minimum_reward_risk_ratio, 1.50);
    EXPECT_DOUBLE_EQ(config.risk.risk_per_trade_fraction, 0.005);
    EXPECT_EQ(config.risk.maximum_open_positions, 3);
    EXPECT_DOUBLE_EQ(config.paper_initial_balance, 10000.0);
    EXPECT_FALSE(config.live_order_submission_enabled);
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

TEST(ConfigTests, RejectsInconsistentResearchPeriods) {
    auto json = valid_config();
    json["research"]["ema_slow_period"] = 20;

    EXPECT_THROW(crypto_trader::common::parse_config(json), std::invalid_argument);
}

TEST(ConfigTests, RejectsUnsupportedOrderBookDepth) {
    auto json = valid_config();
    json["research"]["order_book_depth"] = 17;

    EXPECT_THROW(crypto_trader::common::parse_config(json), std::invalid_argument);
}

TEST(ConfigTests, RejectsSignalWeightsThatDoNotSumToOne) {
    auto json = valid_config();
    json["signals"]["weights"]["trend"] = 0.5;

    EXPECT_THROW(crypto_trader::common::parse_config(json), std::invalid_argument);
}

TEST(ConfigTests, RejectsSignalCostsOutsideValidRange) {
    auto json = valid_config();
    json["signals"]["taker_fee_rate"] = 1.0;

    EXPECT_THROW(crypto_trader::common::parse_config(json), std::invalid_argument);
}

TEST(ConfigTests, RejectsRiskLimitsOutsideSafeRange) {
    auto excessive_trade_risk = valid_config();
    excessive_trade_risk["risk"]["risk_per_trade_fraction"] = 0.1;
    EXPECT_THROW(crypto_trader::common::parse_config(excessive_trade_risk), std::invalid_argument);

    auto leveraged_exposure = valid_config();
    leveraged_exposure["risk"]["maximum_gross_exposure_fraction"] = 1.5;
    EXPECT_THROW(crypto_trader::common::parse_config(leveraged_exposure), std::invalid_argument);
}

TEST(ConfigTests, RejectsInvalidPaperPortfolioSettings) {
    auto invalid_balance = valid_config();
    invalid_balance["paper_initial_balance"] = 0.0;
    EXPECT_THROW(crypto_trader::common::parse_config(invalid_balance), std::invalid_argument);

    auto missing_state_path = valid_config();
    missing_state_path["paper_state_path"] = "";
    EXPECT_THROW(crypto_trader::common::parse_config(missing_state_path), std::invalid_argument);
}

TEST(ConfigTests, RejectsEnablingLiveOrderSubmission) {
    auto config = valid_config();
    config["live_order_submission_enabled"] = true;
    EXPECT_THROW(crypto_trader::common::parse_config(config), std::invalid_argument);
}

TEST(ConfigTests, ParsesOptionalStrategyAndBacktestSettings) {
    const auto defaults = crypto_trader::common::parse_config(valid_config());
    EXPECT_TRUE(defaults.signals.require_bullish_trend_for_buy);
    EXPECT_TRUE(defaults.signals.suppress_repeated_direction);
    EXPECT_FALSE(defaults.signals.require_breakout_for_buy);
    EXPECT_DOUBLE_EQ(defaults.risk.trailing_stop_atr_multiple, 0.0);
    EXPECT_EQ(defaults.backtest.candle_count, 1000);
    EXPECT_FALSE(defaults.backtest.spread_fraction.has_value());

    auto json = valid_config();
    json["signals"]["require_breakout_for_buy"] = true;
    json["signals"]["suppress_repeated_direction"] = false;
    json["risk"]["trailing_stop_atr_multiple"] = 4.0;
    json["risk"]["breakeven_trigger_r"] = 1.5;
    json["backtest"] = {{"candle_count", 6500}, {"spread_fraction", 0.0002}};
    const auto config = crypto_trader::common::parse_config(json);
    EXPECT_TRUE(config.signals.require_breakout_for_buy);
    EXPECT_FALSE(config.signals.suppress_repeated_direction);
    EXPECT_DOUBLE_EQ(config.risk.trailing_stop_atr_multiple, 4.0);
    EXPECT_DOUBLE_EQ(config.risk.breakeven_trigger_r, 1.5);
    EXPECT_EQ(config.backtest.candle_count, 6500);
    EXPECT_DOUBLE_EQ(config.backtest.spread_fraction.value(), 0.0002);
}

TEST(ConfigTests, RejectsInvalidTrailingAndBacktestSettings) {
    auto trailing = valid_config();
    trailing["risk"]["trailing_stop_atr_multiple"] = -1.0;
    EXPECT_THROW(crypto_trader::common::parse_config(trailing), std::invalid_argument);

    auto spread = valid_config();
    spread["backtest"] = {{"spread_fraction", 1.5}};
    EXPECT_THROW(crypto_trader::common::parse_config(spread), std::invalid_argument);

    auto count = valid_config();
    count["backtest"] = {{"candle_count", 1}};
    EXPECT_THROW(crypto_trader::common::parse_config(count), std::invalid_argument);
}

TEST(ConfigTests, TestnetModeRequiresExplicitOptInAndTestnetOrderEndpoint) {
    auto testnet = valid_config();
    testnet["mode"] = "testnet";
    testnet["live_order_submission_enabled"] = true;
    testnet["live"] = {{"capital_allocation", 500.0}, {"maximum_order_notional", 100.0}};
    const auto config = crypto_trader::common::parse_config(testnet);
    EXPECT_EQ(config.mode, "testnet");
    EXPECT_DOUBLE_EQ(config.live.capital_allocation, 500.0);
    EXPECT_EQ(config.live.order_base_url, "https://testnet.binance.vision");

    auto not_opted_in = testnet;
    not_opted_in["live_order_submission_enabled"] = false;
    EXPECT_THROW(crypto_trader::common::parse_config(not_opted_in), std::invalid_argument);

    auto mainnet = testnet;
    mainnet["live"]["order_base_url"] = "https://api.binance.com";
    EXPECT_THROW(crypto_trader::common::parse_config(mainnet), std::invalid_argument);

    auto oversized_order = testnet;
    oversized_order["live"]["maximum_order_notional"] = 1000.0;
    EXPECT_THROW(crypto_trader::common::parse_config(oversized_order), std::invalid_argument);

    auto wrong_quote = testnet;
    wrong_quote["trading_pairs"] = {"ETHBTC"};
    EXPECT_THROW(crypto_trader::common::parse_config(wrong_quote), std::invalid_argument);
}
