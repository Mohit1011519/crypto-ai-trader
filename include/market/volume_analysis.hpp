#pragma once

#include <optional>
#include <span>

#include "common/config.hpp"
#include "market/candle.hpp"
#include "market/research.hpp"

namespace crypto_trader::market {

struct VolumeAnalysis {
    ResearchMetadata metadata;
    double current_volume;
    std::optional<double> average_volume;
    std::optional<double> volume_ratio;
    bool volume_spike;
    double buy_pressure;
    double sell_pressure;
    bool volume_price_divergence;
};

VolumeAnalysis analyze_volume(std::span<const Candle> candles,
                              const common::ResearchSettings& settings);

}  // namespace crypto_trader::market
