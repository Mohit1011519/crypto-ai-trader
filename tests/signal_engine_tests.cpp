#include "ai/signal_engine.hpp"

#include "market/market_scanner.hpp"

#include <string>

#include <gtest/gtest.h>

namespace {

crypto_trader::common::SignalSettings signal_settings() {
    return {
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
    };
}

crypto_trader::market::ScanResult bullish_research(const std::string& symbol = "BTCUSDT") {
    using namespace crypto_trader;
    constexpr market::ResearchMetadata metadata{
        .timestamp_ms = 1'700'000'000'000,
        .sample_count = 500,
        .quality = 1.0
    };
    return {
        .symbol = symbol,
        .technical = indicators::TechnicalIndicators{
            .metadata = metadata,
            .close = 100.0,
            .ema_fast = 101.0,
            .ema_medium = 100.5,
            .ema_slow = 99.0,
            .rsi = 75.0,
            .macd = 0.3,
            .macd_signal = 0.1,
            .macd_histogram = 0.2,
            .bollinger_middle = 100.0,
            .bollinger_upper = 102.0,
            .bollinger_lower = 98.0,
            .atr = 2.0,
            .stochastic_rsi = 80.0
        },
        .trend = market::TrendAnalysis{
            .metadata = metadata,
            .direction = market::TrendDirection::bullish,
            .strength = 0.02,
            .support = 98.0,
            .resistance = 102.0,
            .breakout_up = false,
            .breakout_down = false
        },
        .volume = market::VolumeAnalysis{
            .metadata = metadata,
            .current_volume = 150.0,
            .average_volume = 100.0,
            .volume_ratio = 1.5,
            .volume_spike = false,
            .buy_pressure = 0.85,
            .sell_pressure = 0.15,
            .volume_price_divergence = false
        },
        .volatility = market::VolatilityAnalysis{
            .metadata = metadata,
            .atr = 2.0,
            .atr_percent = 0.02,
            .historical_volatility = 0.01,
            .sudden_volatility = false,
            .risk = market::VolatilityRisk::elevated
        },
        .order_book = market::OrderBookAnalysis{
            .metadata = metadata,
            .best_bid = 99.95,
            .best_ask = 100.05,
            .spread = 0.10,
            .spread_percent = 0.001,
            .bid_quantity = 15.0,
            .ask_quantity = 5.0,
            .imbalance = 0.5,
            .quote_liquidity = 2000.0,
            .large_bid_count = 1,
            .large_ask_count = 0,
            .liquidity = market::LiquidityLevel::sufficient
        },
        .signal = std::nullopt,
        .candle_error = std::nullopt,
        .order_book_error = std::nullopt
    };
}

TEST(SignalEngine, ProducesExplainableBuyAndSellSignals) {
    auto buy_research = bullish_research();
    crypto_trader::ai::SignalEngine buy_engine(signal_settings());
    const auto buy = buy_engine.evaluate(buy_research);

    ASSERT_EQ(buy.action, crypto_trader::ai::Signal::buy);
    EXPECT_GT(buy.research_score, 0.35);
    EXPECT_GE(buy.confirmations, 2);
    ASSERT_TRUE(buy.expected_reward_risk_ratio);
    EXPECT_GE(*buy.expected_reward_risk_ratio, 1.5);
    EXPECT_FALSE(buy.components.empty());
    EXPECT_FALSE(buy.reasons.empty());
    EXPECT_GT(buy.estimated_round_trip_cost_percent, 0.0);

    auto sell_research = bullish_research("ETHUSDT");
    sell_research.technical->rsi = 25.0;
    sell_research.technical->macd_histogram = -0.2;
    sell_research.trend->direction = crypto_trader::market::TrendDirection::bearish;
    sell_research.order_book->imbalance = -0.5;
    sell_research.volume->buy_pressure = 0.15;
    sell_research.volume->sell_pressure = 0.85;
    crypto_trader::ai::SignalEngine sell_engine(signal_settings());
    EXPECT_EQ(sell_engine.evaluate(sell_research).action, crypto_trader::ai::Signal::sell);
}

TEST(SignalEngine, HoldsWhenMarketQualityOrRegimeIsUnsafe) {
    crypto_trader::ai::SignalEngine low_quality_engine(signal_settings());
    auto low_quality = bullish_research("LOWQUALITY");
    low_quality.technical->metadata.quality = 0.2;
    const auto quality_result = low_quality_engine.evaluate(low_quality);
    EXPECT_EQ(quality_result.action, crypto_trader::ai::Signal::hold);
    EXPECT_FALSE(quality_result.reasons.empty());

    crypto_trader::ai::SignalEngine volatility_engine(signal_settings());
    auto high_volatility = bullish_research("HIGHVOL");
    high_volatility.volatility->risk = crypto_trader::market::VolatilityRisk::high;
    const auto volatility_result = volatility_engine.evaluate(high_volatility);
    EXPECT_EQ(volatility_result.action, crypto_trader::ai::Signal::hold);
    EXPECT_FALSE(volatility_result.expected_reward_risk_ratio.has_value());

    crypto_trader::ai::SignalEngine costs_engine(signal_settings());
    auto fees_dominate = bullish_research("COSTS");
    fees_dominate.technical->atr = 0.01;
    const auto cost_result = costs_engine.evaluate(fees_dominate);
    EXPECT_EQ(cost_result.action, crypto_trader::ai::Signal::hold);
    EXPECT_FALSE(cost_result.expected_reward_risk_ratio.has_value());
}

TEST(SignalEngine, SuppressesRepeatedDirectionAndRequiresConfirmations) {
    crypto_trader::ai::SignalEngine engine(signal_settings());
    const auto first = engine.evaluate(bullish_research("REPEATED"));
    const auto second = engine.evaluate(bullish_research("REPEATED"));
    EXPECT_EQ(first.action, crypto_trader::ai::Signal::buy);
    EXPECT_EQ(second.action, crypto_trader::ai::Signal::hold);
    EXPECT_EQ(crypto_trader::ai::to_string(second.action), "HOLD");

    auto weak_research = bullish_research("WEAK");
    weak_research.technical->rsi = 50.0;
    weak_research.technical->macd_histogram = 0.0;
    weak_research.volume->buy_pressure = 0.5;
    weak_research.volume->sell_pressure = 0.5;
    weak_research.order_book->imbalance = 0.0;
    crypto_trader::ai::SignalEngine weak_engine(signal_settings());
    const auto weak = weak_engine.evaluate(weak_research);
    EXPECT_EQ(weak.action, crypto_trader::ai::Signal::hold);
    EXPECT_LT(weak.confirmations, signal_settings().minimum_confirmations);
}

TEST(SignalEngine, RejectsWideSpreadAndMissingResearch) {
    crypto_trader::ai::SignalEngine spread_engine(signal_settings());
    auto wide_spread = bullish_research("WIDESPREAD");
    wide_spread.order_book->spread_percent = 0.02;
    EXPECT_EQ(spread_engine.evaluate(wide_spread).action, crypto_trader::ai::Signal::hold);

    crypto_trader::ai::SignalEngine missing_engine(signal_settings());
    crypto_trader::market::ScanResult unavailable{.symbol = "MISSING"};
    unavailable.candle_error = "exchange timed out";
    const auto missing = missing_engine.evaluate(unavailable);
    EXPECT_EQ(missing.action, crypto_trader::ai::Signal::hold);
    EXPECT_FALSE(missing.reasons.empty());
}

TEST(SignalEngine, UsesMaximumAllowedSpreadWhenOrderBookIsMissing) {
    auto settings = signal_settings();
    settings.minimum_reward_risk_ratio = 1.0;
    crypto_trader::ai::SignalEngine engine(settings);
    auto research = bullish_research("NOBOOK");
    research.order_book.reset();
    research.order_book_error = "exchange timed out";

    const auto result = engine.evaluate(research);

    EXPECT_EQ(result.action, crypto_trader::ai::Signal::buy);
    EXPECT_DOUBLE_EQ(result.estimated_round_trip_cost_percent, 0.008);
    EXPECT_LT(result.research_coverage, 1.0);
}

}  // namespace
