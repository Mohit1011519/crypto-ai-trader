#include "market/volume_analysis.hpp"

#include <algorithm>
#include <cmath>

namespace crypto_trader::market {

VolumeAnalysis analyze_volume(const std::span<const Candle> candles,
                              const common::ResearchSettings& settings) {
    const auto required = static_cast<std::size_t>(
        std::max(settings.volume_average_period + 1, settings.support_resistance_period + 1));
    const double quality = std::min(
        1.0, static_cast<double>(candles.size()) / static_cast<double>(required));

    VolumeAnalysis result{
        .metadata = {
            .timestamp_ms = candles.empty() ? 0 : candles.back().close_time_ms,
            .sample_count = candles.size(),
            .quality = quality
        },
        .current_volume = candles.empty() ? 0.0 : candles.back().volume,
        .average_volume = std::nullopt,
        .volume_ratio = std::nullopt,
        .volume_spike = false,
        .buy_pressure = 0.5,
        .sell_pressure = 0.5,
        .volume_price_divergence = false
    };

    if (candles.empty()) {
        return result;
    }

    if (candles.size() > static_cast<std::size_t>(settings.volume_average_period)) {
        const auto begin = candles.end() - settings.volume_average_period - 1;
        const auto end = candles.end() - 1;
        double average = 0.0;
        for (auto current = begin; current != end; ++current) {
            average += current->volume;
        }
        average /= static_cast<double>(settings.volume_average_period);
        result.average_volume = average;
        if (average > 0.0) {
            result.volume_ratio = result.current_volume / average;
            result.volume_spike = *result.volume_ratio >= settings.volume_spike_multiple;
        }
    }

    double total_volume = 0.0;
    double signed_volume = 0.0;
    const auto pressure_period = std::min(
        candles.size(), static_cast<std::size_t>(settings.volume_average_period));
    for (auto current = candles.end() - static_cast<std::ptrdiff_t>(pressure_period);
         current != candles.end(); ++current) {
        const double range = current->high - current->low;
        if (range > 0.0) {
            const double candle_pressure =
                (2.0 * (current->close - current->low) / range) - 1.0;
            signed_volume += candle_pressure * current->volume;
        }
        total_volume += current->volume;
    }
    if (total_volume > 0.0) {
        result.buy_pressure = std::clamp(
            (signed_volume / total_volume + 1.0) / 2.0, 0.0, 1.0);
        result.sell_pressure = 1.0 - result.buy_pressure;
    }

    const auto divergence_period = static_cast<std::size_t>(settings.support_resistance_period);
    if (candles.size() > divergence_period) {
        const auto begin = candles.end() - static_cast<std::ptrdiff_t>(divergence_period) - 1;
        const auto midpoint = begin + static_cast<std::ptrdiff_t>(divergence_period / 2);
        double first_half_volume = 0.0;
        double second_half_volume = 0.0;
        std::size_t first_count = 0;
        std::size_t second_count = 0;
        for (auto current = begin; current != midpoint; ++current) {
            first_half_volume += current->volume;
            ++first_count;
        }
        for (auto current = midpoint; current != candles.end(); ++current) {
            second_half_volume += current->volume;
            ++second_count;
        }
        const bool price_up = candles.back().close > begin->close;
        const bool price_down = candles.back().close < begin->close;
        const bool volume_falling =
            second_count > 0 && first_count > 0 &&
            second_half_volume / static_cast<double>(second_count) <
                first_half_volume / static_cast<double>(first_count);
        const bool volume_rising =
            second_count > 0 && first_count > 0 &&
            second_half_volume / static_cast<double>(second_count) >
                first_half_volume / static_cast<double>(first_count);
        result.volume_price_divergence =
            (price_up && volume_falling) || (price_down && volume_rising);
    }

    return result;
}

}  // namespace crypto_trader::market
