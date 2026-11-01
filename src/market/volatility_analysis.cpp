#include "market/volatility_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace crypto_trader::market {

VolatilityAnalysis analyze_volatility(const std::span<const Candle> candles,
                                      const std::optional<double> atr,
                                      const common::ResearchSettings& settings) {
    const auto required = static_cast<std::size_t>(
        std::max(settings.historical_volatility_period + 1,
                 settings.atr_period));
    const double quality = std::min(
        1.0, static_cast<double>(candles.size()) / static_cast<double>(required));
    VolatilityAnalysis result{
        .metadata = {
            .timestamp_ms = candles.empty() ? 0 : candles.back().close_time_ms,
            .sample_count = candles.size(),
            .quality = quality
        },
        .atr = atr,
        .atr_percent = std::nullopt,
        .historical_volatility = std::nullopt,
        .sudden_volatility = false,
        .risk = VolatilityRisk::unavailable
    };

    if (candles.empty()) {
        return result;
    }
    if (atr && std::isfinite(*atr) && *atr >= 0.0) {
        result.atr_percent = *atr / candles.back().close;
        if (*result.atr_percent <= settings.normal_volatility_threshold) {
            result.risk = VolatilityRisk::normal;
        } else if (*result.atr_percent <= settings.high_volatility_threshold) {
            result.risk = VolatilityRisk::elevated;
        } else {
            result.risk = VolatilityRisk::high;
        }
    }

    const auto period = static_cast<std::size_t>(settings.historical_volatility_period);
    if (candles.size() > period) {
        std::vector<double> returns;
        returns.reserve(period);
        const auto begin = candles.end() - static_cast<std::ptrdiff_t>(period);
        for (auto current = begin; current != candles.end(); ++current) {
            const auto previous = current - 1;
            returns.push_back(std::log(current->close / previous->close));
        }
        double mean = 0.0;
        for (const double value : returns) {
            mean += value;
        }
        mean /= static_cast<double>(returns.size());
        double squared_deviation = 0.0;
        for (const double value : returns) {
            const double deviation = value - mean;
            squared_deviation += deviation * deviation;
        }
        result.historical_volatility = std::sqrt(
            squared_deviation / static_cast<double>(returns.size()));
    }

    if (candles.size() > period) {
        const auto begin = candles.end() - static_cast<std::ptrdiff_t>(period) - 1;
        double average_range_percent = 0.0;
        std::size_t count = 0;
        for (auto current = begin; current != candles.end() - 1; ++current) {
            average_range_percent += (current->high - current->low) / current->close;
            ++count;
        }
        if (count > 0) {
            average_range_percent /= static_cast<double>(count);
            const double current_range_percent =
                (candles.back().high - candles.back().low) / candles.back().close;
            result.sudden_volatility = average_range_percent > 0.0 &&
                current_range_percent >=
                    average_range_percent * settings.sudden_volatility_multiple;
        }
    }

    return result;
}

}  // namespace crypto_trader::market
