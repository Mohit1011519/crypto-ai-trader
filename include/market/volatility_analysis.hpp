#pragma once

#include <optional>
#include <span>

#include "common/config.hpp"
#include "market/candle.hpp"
#include "market/research.hpp"

namespace crypto_trader::market {

enum class VolatilityRisk {
    normal,
    elevated,
    high,
    unavailable
};

struct VolatilityAnalysis {
    ResearchMetadata metadata;
    std::optional<double> atr;
    std::optional<double> atr_percent;
    std::optional<double> historical_volatility;
    bool sudden_volatility;
    VolatilityRisk risk;
};

VolatilityAnalysis analyze_volatility(std::span<const Candle> candles,
                                      const std::optional<double> atr,
                                      const common::ResearchSettings& settings);

}  // namespace crypto_trader::market
