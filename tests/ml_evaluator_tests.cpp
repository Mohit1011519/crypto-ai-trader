#include "ml/ml_evaluator.hpp"

#include "market/candle.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

crypto_trader::common::AppConfig ml_config() {
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
            .taker_fee_rate = 0.0005,
            .slippage_rate = 0.0001,
            .maximum_spread_percent = 0.0005
        },
        .risk = {
            .risk_per_trade_fraction = 0.005,
            .maximum_open_positions = 3,
            .daily_maximum_loss_fraction = 0.02,
            .maximum_drawdown_fraction = 0.10,
            .maximum_gross_exposure_fraction = 1.0,
            .stop_atr_multiple = 1.5,
            .target_atr_multiple = 3.0,
            .taker_fee_rate = 0.0005,
            .slippage_rate = 0.0001
        },
        .paper_initial_balance = 10000.0,
        .paper_state_path = "unused-ml-state.json",
        .live_order_submission_enabled = false
    };
}

std::vector<crypto_trader::market::Candle> make_candles(const std::size_t count) {
    std::vector<crypto_trader::market::Candle> candles;
    candles.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double phase = static_cast<double>(index) * 0.17;
        const double close = 100.0 *
            std::exp(0.0004 * static_cast<double>(index) + 0.025 * std::sin(phase));
        const double open = index == 0 ? close :
            candles.back().close * (1.0 + 0.001 * std::cos(phase));
        const auto open_time = 1'800'000'000'000LL +
            static_cast<std::int64_t>(index) * 60'000;
        candles.push_back({
            .open_time_ms = open_time,
            .open = open,
            .high = std::max(open, close) * 1.002,
            .low = std::min(open, close) * 0.998,
            .close = close,
            .volume = 100.0 + 20.0 * (1.0 + std::sin(phase * 0.5)),
            .close_time_ms = open_time + 59'999,
            .quote_volume = close * (100.0 + 20.0 * (1.0 + std::sin(phase * 0.5)))
        });
    }
    return candles;
}

TEST(MlEvaluator, ProducesMetricsFromPurgedChronologicalHoldout) {
    const crypto_trader::ml::MlEvaluator evaluator(ml_config());
    const auto result = evaluator.evaluate(make_candles(500));

    EXPECT_EQ(result.candles_processed, 500U);
    EXPECT_EQ(result.horizon_candles, 5U);
    EXPECT_GE(result.training_samples, 20U);
    EXPECT_GE(result.validation_samples, 10U);
    EXPECT_LT(result.training_last_feature_index + result.horizon_candles,
              result.validation_first_feature_index);
    EXPECT_GE(result.validation_positive_rate, 0.0);
    EXPECT_LE(result.validation_positive_rate, 1.0);
    EXPECT_GE(result.validation_accuracy, 0.0);
    EXPECT_LE(result.validation_accuracy, 1.0);
    EXPECT_GE(result.majority_class_baseline_accuracy, 0.5);
    EXPECT_GE(result.precision, 0.0);
    EXPECT_LE(result.precision, 1.0);
    EXPECT_GE(result.recall, 0.0);
    EXPECT_LE(result.recall, 1.0);
    EXPECT_TRUE(std::isfinite(result.log_loss));
    EXPECT_GE(result.log_loss, 0.0);
    EXPECT_TRUE(std::isfinite(result.brier_score));
    EXPECT_GE(result.brier_score, 0.0);
    EXPECT_LE(result.brier_score, 1.0);
    if (result.roc_auc) {
        EXPECT_GE(*result.roc_auc, 0.0);
        EXPECT_LE(*result.roc_auc, 1.0);
    }
}

TEST(MlEvaluator, RejectsInsufficientHistoryAndInvalidCandles) {
    const crypto_trader::ml::MlEvaluator evaluator(ml_config());
    EXPECT_THROW(evaluator.evaluate(make_candles(50)), std::invalid_argument);

    auto candles = make_candles(100);
    candles[40].low = candles[40].high + 1.0;
    EXPECT_THROW(evaluator.evaluate(candles), std::invalid_argument);
}

}  // namespace
