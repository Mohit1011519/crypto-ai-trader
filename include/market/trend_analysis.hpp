#pragma once

#include <optional>
#include <span>

#include "common/config.hpp"
#include "indicators/technical_indicators.hpp"
#include "market/candle.hpp"
#include "market/research.hpp"

namespace crypto_trader::market {

enum class TrendDirection {
    bullish,
    bearish,
    sideways,
    unavailable
};

struct TrendAnalysis {
    ResearchMetadata metadata;
    TrendDirection direction;
    double strength;
    std::optional<double> support;
    std::optional<double> resistance;
    bool breakout_up;
    bool breakout_down;
};

TrendAnalysis analyze_trend(std::span<const Candle> candles,
                            const indicators::TechnicalIndicators& indicators,
                            const common::ResearchSettings& settings);

}  // namespace crypto_trader::market
