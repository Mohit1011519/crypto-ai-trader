#include "market/trend_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace crypto_trader::market {

TrendAnalysis analyze_trend(const std::span<const Candle> candles,
                            const indicators::TechnicalIndicators& indicators,
                            const common::ResearchSettings& settings) {
    const auto required = static_cast<std::size_t>(
        std::max(settings.ema_slow_period, settings.support_resistance_period + 1));
    const double quality = std::min(
        1.0, static_cast<double>(candles.size()) / static_cast<double>(required));

    TrendAnalysis result{
        .metadata = {
            .timestamp_ms = candles.empty() ? 0 : candles.back().close_time_ms,
            .sample_count = candles.size(),
            .quality = quality
        },
        .direction = TrendDirection::unavailable,
        .strength = 0.0,
        .support = std::nullopt,
        .resistance = std::nullopt,
        .breakout_up = false,
        .breakout_down = false
    };

    if (indicators.ema_fast && indicators.ema_medium && indicators.ema_slow) {
        const auto fast = *indicators.ema_fast;
        const auto medium = *indicators.ema_medium;
        const auto slow = *indicators.ema_slow;
        if (fast > medium && medium > slow && indicators.close > fast) {
            result.direction = TrendDirection::bullish;
        } else if (fast < medium && medium < slow && indicators.close < fast) {
            result.direction = TrendDirection::bearish;
        } else {
            result.direction = TrendDirection::sideways;
        }

        const double normalized_spread = std::abs(fast - slow) / indicators.close;
        result.strength = normalized_spread / (1.0 + normalized_spread);
    }

    if (candles.size() > static_cast<std::size_t>(settings.support_resistance_period)) {
        const auto end = candles.end() - 1;
        const auto begin = end - settings.support_resistance_period;
        double support = std::numeric_limits<double>::max();
        double resistance = std::numeric_limits<double>::lowest();
        for (auto current = begin; current != end; ++current) {
            support = std::min(support, current->low);
            resistance = std::max(resistance, current->high);
        }
        result.support = support;
        result.resistance = resistance;
        result.breakout_up = candles.back().close > resistance;
        result.breakout_down = candles.back().close < support;
    }

    return result;
}

}  // namespace crypto_trader::market
