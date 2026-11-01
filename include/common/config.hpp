#pragma once

#include <filesystem>
#include <optional>
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
    // Spot is long-only, so buys are only taken in the direction of an
    // established EMA uptrend ("trade with the flow").
    bool require_bullish_trend_for_buy = true;
    // Legacy behaviour: suppress a BUY/SELL until the opposite direction is
    // emitted. When false, only a re-evaluation of the same closed candle is
    // suppressed, which allows re-entry after a protective exit.
    bool suppress_repeated_direction = true;
    // Require the last close to break above the prior support/resistance
    // window high (Donchian breakout) before buying.
    bool require_breakout_for_buy = false;
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
    // Chandelier trailing stop: highest close since entry minus this many
    // current ATRs. Zero disables trailing.
    double trailing_stop_atr_multiple = 0.0;
    // Move the stop to break-even (entry plus round-trip costs) once the
    // highest close reaches entry plus this multiple of the initial risk.
    // Zero disables the break-even rule.
    double breakeven_trigger_r = 0.0;
};

struct BacktestSettings {
    // Total closed candles fetched (paged) for --backtest.
    int candle_count = 1000;
    // Spread assumed for simulated fills when historical books are
    // unavailable. Unset means the conservative maximum_spread_percent.
    std::optional<double> spread_fraction;
};

// Order execution against Binance Spot Testnet. Mainnet order endpoints are
// rejected by configuration validation. API credentials are read from the
// BINANCE_TESTNET_API_KEY / BINANCE_TESTNET_API_SECRET environment variables
// and are never stored in configuration or state files.
struct LiveSettings {
    std::string order_base_url = "https://testnet.binance.vision";
    std::filesystem::path state_path = "data/testnet_portfolio.json";
    std::string quote_asset = "USDT";
    // Starting quote capital the bot manages. Its ledger grows with profits and
    // shrinks with losses; the bot never spends more than that ledger or the
    // account's free balance, so other funds in the account are untouched.
    double capital_allocation = 1000.0;
    // Hard cap on a single entry order's notional.
    double maximum_order_notional = 250.0;
    // Only replace the exchange stop when the ratcheted stop has risen by at
    // least this fraction, limiting cancel/replace churn.
    double stop_update_minimum_fraction = 0.002;
    // Sell every position when the daily-loss or drawdown limit trips.
    bool flatten_on_risk_shutdown = true;
    int receive_window_ms = 5000;
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
    // Empty disables the log file.
    std::string log_file = "";
    ResearchSettings research;
    SignalSettings signals;
    RiskSettings risk;
    double paper_initial_balance;
    std::filesystem::path paper_state_path;
    bool live_order_submission_enabled;
    BacktestSettings backtest{};
    LiveSettings live{};
};

AppConfig parse_config(const nlohmann::json& json);
AppConfig load_config(const std::filesystem::path& path);

}  // namespace crypto_trader::common
