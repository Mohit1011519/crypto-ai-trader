#include "indicators/technical_indicators.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace crypto_trader::indicators {
namespace {

using OptionalSeries = std::vector<std::optional<double>>;

void validate_candles(const std::span<const market::Candle> candles) {
    if (candles.empty()) {
        throw std::invalid_argument("At least one candle is required for analysis");
    }

    for (std::size_t index = 0; index < candles.size(); ++index) {
        const auto& candle = candles[index];
        if (!std::isfinite(candle.open) || !std::isfinite(candle.high) ||
            !std::isfinite(candle.low) || !std::isfinite(candle.close) ||
            !std::isfinite(candle.volume) || candle.open <= 0.0 ||
            candle.high < candle.low || candle.high < candle.open ||
            candle.high < candle.close || candle.low > candle.open ||
            candle.low > candle.close || candle.close <= 0.0 || candle.volume < 0.0) {
            throw std::invalid_argument("Candle data contains invalid OHLCV values");
        }
        if (index > 0 && candles[index - 1].open_time_ms >= candle.open_time_ms) {
            throw std::invalid_argument("Candles must be in strictly increasing time order");
        }
    }
}

OptionalSeries ema_series(const std::vector<double>& values, const int period) {
    OptionalSeries result(values.size());
    if (values.size() < static_cast<std::size_t>(period)) {
        return result;
    }

    double initial_sum = 0.0;
    for (int index = 0; index < period; ++index) {
        initial_sum += values[static_cast<std::size_t>(index)];
    }
    double ema = initial_sum / static_cast<double>(period);
    result[static_cast<std::size_t>(period - 1)] = ema;
    const double multiplier = 2.0 / (static_cast<double>(period) + 1.0);
    for (std::size_t index = static_cast<std::size_t>(period); index < values.size(); ++index) {
        ema = (values[index] - ema) * multiplier + ema;
        result[index] = ema;
    }
    return result;
}

OptionalSeries rsi_series(const std::vector<double>& closes, const int period) {
    OptionalSeries result(closes.size());
    if (closes.size() <= static_cast<std::size_t>(period)) {
        return result;
    }

    double average_gain = 0.0;
    double average_loss = 0.0;
    for (int index = 1; index <= period; ++index) {
        const double change = closes[static_cast<std::size_t>(index)] -
            closes[static_cast<std::size_t>(index - 1)];
        average_gain += std::max(change, 0.0);
        average_loss += std::max(-change, 0.0);
    }
    average_gain /= static_cast<double>(period);
    average_loss /= static_cast<double>(period);

    const auto to_rsi = [](const double gain, const double loss) {
        if (loss == 0.0) {
            return gain == 0.0 ? 50.0 : 100.0;
        }
        const double relative_strength = gain / loss;
        return 100.0 - (100.0 / (1.0 + relative_strength));
    };

    result[static_cast<std::size_t>(period)] = to_rsi(average_gain, average_loss);
    for (std::size_t index = static_cast<std::size_t>(period + 1); index < closes.size(); ++index) {
        const double change = closes[index] - closes[index - 1];
        average_gain = (average_gain * static_cast<double>(period - 1) +
                        std::max(change, 0.0)) / static_cast<double>(period);
        average_loss = (average_loss * static_cast<double>(period - 1) +
                        std::max(-change, 0.0)) / static_cast<double>(period);
        result[index] = to_rsi(average_gain, average_loss);
    }
    return result;
}

std::optional<double> last(const OptionalSeries& series) {
    if (series.empty()) {
        return std::nullopt;
    }
    return series.back();
}

std::size_t required_samples(const common::ResearchSettings& settings) {
    return static_cast<std::size_t>(std::max({
        settings.ema_slow_period,
        settings.macd_slow_period + settings.macd_signal_period - 1,
        settings.bollinger_period,
        settings.atr_period,
        settings.rsi_period + settings.stochastic_rsi_period
    }));
}

}  // namespace

TechnicalIndicators calculate(const std::span<const market::Candle> candles,
                              const common::ResearchSettings& settings) {
    validate_candles(candles);

    std::vector<double> closes;
    closes.reserve(candles.size());
    for (const auto& candle : candles) {
        closes.push_back(candle.close);
    }

    const auto ema_fast = ema_series(closes, settings.ema_fast_period);
    const auto ema_medium = ema_series(closes, settings.ema_medium_period);
    const auto ema_slow = ema_series(closes, settings.ema_slow_period);
    const auto rsi_values = rsi_series(closes, settings.rsi_period);

    auto macd_values = OptionalSeries(candles.size());
    const auto macd_fast = ema_series(closes, settings.macd_fast_period);
    const auto macd_slow = ema_series(closes, settings.macd_slow_period);
    std::vector<double> macd_for_signal;
    std::vector<std::size_t> macd_indices;
    for (std::size_t index = 0; index < candles.size(); ++index) {
        if (macd_fast[index] && macd_slow[index]) {
            macd_values[index] = *macd_fast[index] - *macd_slow[index];
            macd_for_signal.push_back(*macd_values[index]);
            macd_indices.push_back(index);
        }
    }
    const auto signal_values = ema_series(macd_for_signal, settings.macd_signal_period);
    OptionalSeries aligned_signal(candles.size());
    for (std::size_t index = 0; index < signal_values.size(); ++index) {
        if (signal_values[index]) {
            aligned_signal[macd_indices[index]] = signal_values[index];
        }
    }

    std::optional<double> bollinger_middle;
    std::optional<double> bollinger_upper;
    std::optional<double> bollinger_lower;
    if (candles.size() >= static_cast<std::size_t>(settings.bollinger_period)) {
        const auto first = closes.end() - settings.bollinger_period;
        double mean = 0.0;
        for (auto current = first; current != closes.end(); ++current) {
            mean += *current;
        }
        mean /= static_cast<double>(settings.bollinger_period);
        double squared_deviation = 0.0;
        for (auto current = first; current != closes.end(); ++current) {
            const double deviation = *current - mean;
            squared_deviation += deviation * deviation;
        }
        const double standard_deviation =
            std::sqrt(squared_deviation / static_cast<double>(settings.bollinger_period));
        bollinger_middle = mean;
        bollinger_upper = mean + settings.bollinger_standard_deviations * standard_deviation;
        bollinger_lower = mean - settings.bollinger_standard_deviations * standard_deviation;
    }

    std::optional<double> atr;
    if (candles.size() >= static_cast<std::size_t>(settings.atr_period)) {
        std::vector<double> true_ranges;
        true_ranges.reserve(candles.size());
        true_ranges.push_back(candles.front().high - candles.front().low);
        for (std::size_t index = 1; index < candles.size(); ++index) {
            const auto& candle = candles[index];
            const double previous_close = candles[index - 1].close;
            true_ranges.push_back(std::max({
                candle.high - candle.low,
                std::abs(candle.high - previous_close),
                std::abs(candle.low - previous_close)
            }));
        }
        double average_true_range = 0.0;
        for (int index = 0; index < settings.atr_period; ++index) {
            average_true_range += true_ranges[static_cast<std::size_t>(index)];
        }
        average_true_range /= static_cast<double>(settings.atr_period);
        for (std::size_t index = static_cast<std::size_t>(settings.atr_period);
             index < true_ranges.size(); ++index) {
            average_true_range =
                (average_true_range * static_cast<double>(settings.atr_period - 1) +
                 true_ranges[index]) / static_cast<double>(settings.atr_period);
        }
        atr = average_true_range;
    }

    std::optional<double> stochastic_rsi;
    if (rsi_values.size() >= static_cast<std::size_t>(settings.stochastic_rsi_period)) {
        std::vector<double> recent_rsi;
        for (auto current = rsi_values.rbegin(); current != rsi_values.rend() &&
             recent_rsi.size() < static_cast<std::size_t>(settings.stochastic_rsi_period); ++current) {
            if (*current) {
                recent_rsi.push_back(**current);
            }
        }
        if (recent_rsi.size() ==
            static_cast<std::size_t>(settings.stochastic_rsi_period)) {
            const auto [minimum, maximum] = std::minmax_element(recent_rsi.begin(), recent_rsi.end());
            if (*maximum == *minimum) {
                stochastic_rsi = 0.0;
            } else {
                stochastic_rsi = (recent_rsi.front() - *minimum) / (*maximum - *minimum) * 100.0;
            }
        }
    }

    const auto needed = required_samples(settings);
    const double quality = std::min(
        1.0, static_cast<double>(candles.size()) / static_cast<double>(needed));
    return {
        .metadata = {
            .timestamp_ms = candles.back().close_time_ms,
            .sample_count = candles.size(),
            .quality = quality
        },
        .close = candles.back().close,
        .ema_fast = last(ema_fast),
        .ema_medium = last(ema_medium),
        .ema_slow = last(ema_slow),
        .rsi = last(rsi_values),
        .macd = last(macd_values),
        .macd_signal = last(aligned_signal),
        .macd_histogram = (last(macd_values) && last(aligned_signal))
            ? std::optional<double>(*last(macd_values) - *last(aligned_signal))
            : std::nullopt,
        .bollinger_middle = bollinger_middle,
        .bollinger_upper = bollinger_upper,
        .bollinger_lower = bollinger_lower,
        .atr = atr,
        .stochastic_rsi = stochastic_rsi
    };
}

}  // namespace crypto_trader::indicators
