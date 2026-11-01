#include "backtest/backtester.hpp"

#include "market/candle.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

crypto_trader::common::AppConfig backtest_config() {
    return {
        .exchange = "binance",
        .mode = "paper",
        .rest_base_url = "https://api.binance.com",
        .websocket_host = "stream.binance.com",
        .websocket_port = "9443",
        .websocket_path = "/ws",
        .trading_pairs = {"BTCUSDT"},
        .candle_interval = "1m",
        .candle_limit = 500,
        .request_timeout_seconds = 15,
        .reconnect_delay_seconds = 5,
        .log_level = "off",
        .research = {
            .ema_fast_period = 2,
            .ema_medium_period = 3,
            .ema_slow_period = 5,
            .rsi_period = 3,
            .macd_fast_period = 2,
            .macd_slow_period = 4,
            .macd_signal_period = 2,
            .bollinger_period = 4,
            .bollinger_standard_deviations = 2.0,
            .atr_period = 2,
            .stochastic_rsi_period = 3,
            .volume_average_period = 3,
            .volume_spike_multiple = 2.0,
            .support_resistance_period = 4,
            .historical_volatility_period = 3,
            .normal_volatility_threshold = 0.01,
            .high_volatility_threshold = 0.03,
            .sudden_volatility_multiple = 2.5,
            .order_book_depth = 5,
            .large_order_multiple = 3.0,
            .minimum_quote_liquidity = 100.0,
            .scan_interval_seconds = 1
        },
        .signals = {
            .trend_weight = 0.25,
            .momentum_weight = 0.30,
            .volume_weight = 0.15,
            .volatility_weight = 0.10,
            .order_book_weight = 0.20,
            .sentiment_weight = 0.0,
            .minimum_signal_score = 0.10,
            .minimum_research_quality = 0.40,
            .minimum_research_coverage = 0.30,
            .minimum_confirmations = 2,
            .minimum_reward_risk_ratio = 0.10,
            .target_atr_multiple = 3.0,
            .stop_atr_multiple = 1.5,
            .taker_fee_rate = 0.001,
            .slippage_rate = 0.0005,
            .maximum_spread_percent = 0.001
        },
        .risk = {
            .risk_per_trade_fraction = 0.005,
            .maximum_open_positions = 3,
            .daily_maximum_loss_fraction = 0.02,
            .maximum_drawdown_fraction = 0.10,
            .maximum_gross_exposure_fraction = 1.0,
            .stop_atr_multiple = 1.5,
            .target_atr_multiple = 3.0,
            .taker_fee_rate = 0.001,
            .slippage_rate = 0.0005
        },
        .paper_initial_balance = 10000.0,
        .paper_state_path = "unused-backtest-state.json",
        .live_order_submission_enabled = false
    };
}

std::vector<crypto_trader::market::Candle> rising_candles(const std::size_t count) {
    std::vector<crypto_trader::market::Candle> candles;
    candles.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double open = 100.0 + static_cast<double>(index) * 2.0;
        const double close = open + 1.6;
        const auto open_time = 1'800'000'000'000LL +
            static_cast<std::int64_t>(index) * 60'000;
        candles.push_back({
            .open_time_ms = open_time,
            .open = open,
            .high = close + 0.2,
            .low = open - 0.2,
            .close = close,
            .volume = 100.0,
            .close_time_ms = open_time + 59'999,
            .quote_volume = close * 100.0
        });
    }
    return candles;
}

TEST(Backtester, RejectsTooFewCandlesAndInvalidChronology) {
    const crypto_trader::backtest::Backtester backtester(backtest_config());
    const auto candles = rising_candles(4);
    EXPECT_THROW(backtester.run("BTCUSDT", std::span(candles).first(2)),
                 std::invalid_argument);

    auto invalid = candles;
    invalid[2].open_time_ms = invalid[1].open_time_ms;
    EXPECT_THROW(backtester.run("BTCUSDT", invalid), std::invalid_argument);
}

TEST(Backtester, ProducesFiniteMetricsAndUsesConservativeSimulatedFills) {
    const crypto_trader::backtest::Backtester backtester(backtest_config());
    const auto result = backtester.run("BTCUSDT", rising_candles(80));

    EXPECT_EQ(result.candles_processed, 80U);
    EXPECT_DOUBLE_EQ(result.initial_equity, 10000.0);
    EXPECT_TRUE(std::isfinite(result.final_equity));
    EXPECT_TRUE(std::isfinite(result.total_return_fraction));
    EXPECT_TRUE(std::isfinite(result.buy_and_hold_return_fraction));
    EXPECT_TRUE(std::isfinite(result.maximum_drawdown_fraction));
    EXPECT_GE(result.maximum_drawdown_fraction, 0.0);
    EXPECT_TRUE(std::isfinite(result.per_bar_sharpe_ratio));
    EXPECT_FALSE(result.risk_shutdown);
    ASSERT_FALSE(result.trades.empty());
    EXPECT_GT(result.winning_trades, 0U);
    EXPECT_GT(result.trades.front().entry_price, 100.0);
    EXPECT_LT(result.trades.front().entry_price,
              100.0 + static_cast<double>(80) * 2.0);
    EXPECT_LT(result.trades.front().exit_price,
              100.0 + static_cast<double>(80) * 2.0);
    EXPECT_GT(result.trades.front().fees, 0.0);
    EXPECT_EQ(result.completed_trades,
              result.winning_trades + result.losing_trades);
}

TEST(Backtester, RejectsInvalidSymbolAndInvalidCandleValues) {
    const crypto_trader::backtest::Backtester backtester(backtest_config());
    auto candles = rising_candles(5);
    EXPECT_THROW(backtester.run("", candles), std::invalid_argument);
    candles[2].high = candles[2].low - 1.0;
    EXPECT_THROW(backtester.run("BTCUSDT", candles), std::invalid_argument);
}

}  // namespace
