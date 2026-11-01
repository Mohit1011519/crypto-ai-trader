#include "indicators/technical_indicators.hpp"
#include "market/market_scanner.hpp"
#include "market/order_book_analysis.hpp"
#include "market/trend_analysis.hpp"
#include "market/volatility_analysis.hpp"
#include "market/volume_analysis.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

crypto_trader::common::ResearchSettings research_settings() {
    return {
        .ema_fast_period = 5,
        .ema_medium_period = 10,
        .ema_slow_period = 20,
        .rsi_period = 5,
        .macd_fast_period = 3,
        .macd_slow_period = 6,
        .macd_signal_period = 3,
        .bollinger_period = 10,
        .bollinger_standard_deviations = 2.0,
        .atr_period = 5,
        .stochastic_rsi_period = 5,
        .volume_average_period = 5,
        .volume_spike_multiple = 2.0,
        .support_resistance_period = 10,
        .historical_volatility_period = 5,
        .normal_volatility_threshold = 0.01,
        .high_volatility_threshold = 0.03,
        .sudden_volatility_multiple = 2.5,
        .order_book_depth = 5,
        .large_order_multiple = 3.0,
        .minimum_quote_liquidity = 100.0,
        .scan_interval_seconds = 1
    };
}

std::vector<crypto_trader::market::Candle> make_candles(
    const std::size_t count,
    const double close_step = 0.1) {
    std::vector<crypto_trader::market::Candle> candles;
    candles.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double close = 100.0 + static_cast<double>(index) * close_step;
        candles.push_back({
            .open_time_ms = static_cast<std::int64_t>(index) * 60'000,
            .open = close,
            .high = close + 1.0,
            .low = close - 1.0,
            .close = close,
            .volume = 100.0,
            .close_time_ms = static_cast<std::int64_t>(index) * 60'000 + 59'999,
            .quote_volume = close * 100.0
        });
    }
    return candles;
}

TEST(TechnicalIndicators, CalculatesConfiguredIndicatorsAndQuality) {
    const auto candles = make_candles(30);
    const auto result = crypto_trader::indicators::calculate(candles, research_settings());

    ASSERT_TRUE(result.ema_fast);
    ASSERT_TRUE(result.ema_medium);
    ASSERT_TRUE(result.ema_slow);
    ASSERT_TRUE(result.rsi);
    ASSERT_TRUE(result.macd);
    ASSERT_TRUE(result.macd_signal);
    ASSERT_TRUE(result.macd_histogram);
    ASSERT_TRUE(result.bollinger_upper);
    ASSERT_TRUE(result.bollinger_lower);
    ASSERT_TRUE(result.atr);
    ASSERT_TRUE(result.stochastic_rsi);
    EXPECT_DOUBLE_EQ(*result.rsi, 100.0);
    EXPECT_GT(*result.ema_fast, *result.ema_medium);
    EXPECT_GT(*result.ema_medium, *result.ema_slow);
    EXPECT_DOUBLE_EQ(result.metadata.quality, 1.0);
}

TEST(TechnicalIndicators, ReportsWarmupIndicatorsAsUnavailable) {
    const auto candles = make_candles(5);
    const auto result = crypto_trader::indicators::calculate(candles, research_settings());

    EXPECT_FALSE(result.ema_slow.has_value());
    EXPECT_FALSE(result.rsi.has_value());
    EXPECT_FALSE(result.stochastic_rsi.has_value());
    EXPECT_LT(result.metadata.quality, 1.0);
}

TEST(TechnicalIndicators, RejectsNonChronologicalCandles) {
    auto candles = make_candles(3);
    candles[2].open_time_ms = candles[1].open_time_ms;

    EXPECT_THROW(
        crypto_trader::indicators::calculate(candles, research_settings()),
        std::invalid_argument);
}

TEST(TrendAnalysis, DetectsTrendAndBreakoutAgainstPriorCandles) {
    auto candles = make_candles(30);
    candles.back().open = 110.0;
    candles.back().close = 110.0;
    candles.back().high = 111.0;
    candles.back().low = 109.0;
    const auto indicators = crypto_trader::indicators::calculate(candles, research_settings());

    const auto result = crypto_trader::market::analyze_trend(
        candles, indicators, research_settings());

    EXPECT_EQ(result.direction, crypto_trader::market::TrendDirection::bullish);
    EXPECT_TRUE(result.breakout_up);
    EXPECT_FALSE(result.breakout_down);
    EXPECT_TRUE(result.resistance.has_value());
}

TEST(VolumeAnalysis, DetectsSpikeAndCandleClosePressure) {
    auto candles = make_candles(12, 0.0);
    for (auto& candle : candles) {
        candle.high = candle.close + 1.0;
        candle.low = candle.close - 1.0;
        candle.close = candle.high;
    }
    candles.back().volume = 300.0;
    const auto result = crypto_trader::market::analyze_volume(candles, research_settings());

    EXPECT_TRUE(result.volume_spike);
    EXPECT_GT(*result.volume_ratio, 2.0);
    EXPECT_GT(result.buy_pressure, result.sell_pressure);
}

TEST(VolatilityAnalysis, DetectsSuddenRangeExpansion) {
    auto candles = make_candles(12, 0.0);
    candles.back().high += 10.0;
    const auto result = crypto_trader::market::analyze_volatility(
        candles, 2.0, research_settings());

    EXPECT_TRUE(result.atr_percent.has_value());
    EXPECT_TRUE(result.historical_volatility.has_value());
    EXPECT_TRUE(result.sudden_volatility);
    EXPECT_EQ(result.risk, crypto_trader::market::VolatilityRisk::elevated);
}

TEST(OrderBookAnalysis, CalculatesSpreadImbalanceAndLiquidity) {
    const crypto_trader::market::OrderBook book{
        .timestamp_ms = 1234,
        .bids = {{100.0, 10.0}, {99.0, 10.0}},
        .asks = {{101.0, 5.0}, {102.0, 5.0}}
    };
    const auto result = crypto_trader::market::analyze_order_book(book, research_settings());

    EXPECT_DOUBLE_EQ(result.spread, 1.0);
    EXPECT_GT(result.imbalance, 0.0);
    EXPECT_EQ(result.liquidity, crypto_trader::market::LiquidityLevel::sufficient);
    EXPECT_EQ(result.metadata.timestamp_ms, 1234);
}

class FakeExchange final : public crypto_trader::exchange::Exchange {
public:
    std::vector<crypto_trader::market::Candle> fetch_candles(
        const std::string& symbol,
        const std::string&,
        int) const override {
        if (symbol == "ETHUSDT") {
            throw std::runtime_error("simulated market-data failure");
        }
        return make_candles(30);
    }

    crypto_trader::market::OrderBook fetch_order_book(
        const std::string&,
        int) const override {
        return {
            .timestamp_ms = 1234,
            .bids = {{100.0, 10.0}},
            .asks = {{101.0, 10.0}}
        };
    }
};

crypto_trader::common::AppConfig scanner_config() {
    return {
        .exchange = "binance",
        .mode = "paper",
        .rest_base_url = "https://api.binance.com",
        .websocket_host = "stream.binance.com",
        .websocket_port = "9443",
        .websocket_path = "/ws",
        .trading_pairs = {"BTCUSDT", "ETHUSDT"},
        .candle_interval = "1m",
        .candle_limit = 500,
        .request_timeout_seconds = 15,
        .reconnect_delay_seconds = 5,
        .log_level = "off",
        .research = research_settings(),
        .signals = {
            .trend_weight = 0.25,
            .momentum_weight = 0.30,
            .volume_weight = 0.15,
            .volatility_weight = 0.10,
            .order_book_weight = 0.20,
            .sentiment_weight = 0.0,
            .minimum_signal_score = 0.35,
            .minimum_research_quality = 0.80,
            .minimum_research_coverage = 0.65,
            .minimum_confirmations = 2,
            .minimum_reward_risk_ratio = 1.50,
            .target_atr_multiple = 3.0,
            .stop_atr_multiple = 1.5,
            .taker_fee_rate = 0.001,
            .slippage_rate = 0.0005,
            .maximum_spread_percent = 0.005
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
        .paper_state_path = "test-paper-state.json",
        .live_order_submission_enabled = false
    };
}

TEST(MarketScanner, KeepsScanningOtherPairsAfterOnePairFails) {
    const FakeExchange exchange;
    const auto config = scanner_config();
    crypto_trader::market::MarketScanner scanner(exchange, config);

    const auto results = scanner.scan_once();

    ASSERT_EQ(results.size(), 2);
    EXPECT_TRUE(results[0].technical.has_value());
    EXPECT_TRUE(results[0].order_book.has_value());
    ASSERT_TRUE(results[0].signal.has_value());
    EXPECT_EQ(results[0].signal->action, crypto_trader::ai::Signal::hold);
    EXPECT_FALSE(results[0].candle_error.has_value());
    EXPECT_FALSE(results[1].technical.has_value());
    EXPECT_TRUE(results[1].candle_error.has_value());
}

}  // namespace
