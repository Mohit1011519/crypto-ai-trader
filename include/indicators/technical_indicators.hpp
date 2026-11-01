#pragma once

#include <optional>
#include <span>

#include "common/config.hpp"
#include "market/candle.hpp"
#include "market/research.hpp"

namespace crypto_trader::indicators {

struct TechnicalIndicators {
    market::ResearchMetadata metadata;
    double close;
    std::optional<double> ema_fast;
    std::optional<double> ema_medium;
    std::optional<double> ema_slow;
    std::optional<double> rsi;
    std::optional<double> macd;
    std::optional<double> macd_signal;
    std::optional<double> macd_histogram;
    std::optional<double> bollinger_middle;
    std::optional<double> bollinger_upper;
    std::optional<double> bollinger_lower;
    std::optional<double> atr;
    std::optional<double> stochastic_rsi;
};

TechnicalIndicators calculate(const std::span<const market::Candle> candles,
                              const common::ResearchSettings& settings);

}  // namespace crypto_trader::indicators
