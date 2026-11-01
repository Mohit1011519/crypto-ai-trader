#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace crypto_trader::common {

struct ResearchSettings {
    int ema_fast_period;
    int ema_medium_period;
    int ema_slow_period;
    int rsi_period;
    int macd_fast_period;
    int macd_slow_period;
    int macd_signal_period;
    int bollinger_period;
    double bollinger_standard_deviations;
    int atr_period;
    int stochastic_rsi_period;
    int volume_average_period;
    double volume_spike_multiple;
    int support_resistance_period;
    int historical_volatility_period;
    double normal_volatility_threshold;
    double high_volatility_threshold;
    double sudden_volatility_multiple;
    int order_book_depth;
    double large_order_multiple;
    double minimum_quote_liquidity;
    int scan_interval_seconds;
};

struct SignalSettings {
    double trend_weight;
    double momentum_weight;
    double volume_weight;
    double volatility_weight;
    double order_book_weight;
    double sentiment_weight;
    double minimum_signal_score;
    double minimum_research_quality;
    double minimum_research_coverage;
    int minimum_confirmations;
    double minimum_reward_risk_ratio;
    double target_atr_multiple;
    double stop_atr_multiple;
    double taker_fee_rate;
    double slippage_rate;
    double maximum_spread_percent;
};

struct RiskSettings {
    double risk_per_trade_fraction;
    int maximum_open_positions;
    double daily_maximum_loss_fraction;
    double maximum_drawdown_fraction;
    double maximum_gross_exposure_fraction;
    double stop_atr_multiple;
    double target_atr_multiple;
    double taker_fee_rate;
    double slippage_rate;
};

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
    ResearchSettings research;
    SignalSettings signals;
    RiskSettings risk;
    double paper_initial_balance;
    std::filesystem::path paper_state_path;
};

AppConfig parse_config(const nlohmann::json& json);
AppConfig load_config(const std::filesystem::path& path);

}  // namespace crypto_trader::common
