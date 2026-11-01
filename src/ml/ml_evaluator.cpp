#include "ml/ml_evaluator.hpp"

#include "indicators/technical_indicators.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace crypto_trader::ml {
namespace {

constexpr std::size_t feature_count = 6;
constexpr std::size_t lookback_candles = 10;
constexpr std::size_t forecast_horizon = 5;
constexpr double training_fraction = 0.70;
constexpr double learning_rate = 0.10;
constexpr double l2_penalty = 0.01;
constexpr std::size_t training_epochs = 1200;

struct Sample {
    std::array<double, feature_count> features;
    double label;
    std::size_t feature_index;
};

struct Standardizer {
    std::array<double, feature_count> means{};
    std::array<double, feature_count> scales{};
};

struct LogisticModel {
    std::array<double, feature_count + 1> weights{};

    double probability(const std::array<double, feature_count>& features,
                       const Standardizer& standardizer) const {
        double score = weights[0];
        for (std::size_t index = 0; index < feature_count; ++index) {
            score += weights[index + 1] *
                ((features[index] - standardizer.means[index]) /
                 standardizer.scales[index]);
        }
        score = std::clamp(score, -35.0, 35.0);
        return 1.0 / (1.0 + std::exp(-score));
    }
};

std::array<double, feature_count> make_features(
    const std::span<const market::Candle> candles,
    const std::size_t index) {
    constexpr std::size_t window = lookback_candles;
    const double close = candles[index].close;
    const double mean_close = [&] {
        double sum = 0.0;
        for (std::size_t offset = index - window + 1; offset <= index; ++offset) {
            sum += candles[offset].close;
        }
        return sum / static_cast<double>(window);
    }();
    double mean_volume = 0.0;
    for (std::size_t offset = index - window; offset < index; ++offset) {
        mean_volume += candles[offset].volume;
    }
    mean_volume /= static_cast<double>(window);

    std::array<double, window> returns{};
    double returns_sum = 0.0;
    for (std::size_t offset = 0; offset < window; ++offset) {
        const auto candle_index = index - window + 1 + offset;
        returns[offset] = std::log(
            candles[candle_index].close / candles[candle_index - 1].close);
        returns_sum += returns[offset];
    }
    const double return_mean = returns_sum / static_cast<double>(window);
    double squared_deviation = 0.0;
    for (const double value : returns) {
        const double difference = value - return_mean;
        squared_deviation += difference * difference;
    }
    const double return_volatility =
        std::sqrt(squared_deviation / static_cast<double>(window));
    const double volume_ratio = mean_volume > 0.0
        ? std::log1p(candles[index].volume / mean_volume) : 0.0;

    return {
        std::log(close / candles[index - 1].close),
        std::log(close / candles[index - 5].close),
        std::log(close / candles[index - window].close),
        close / mean_close - 1.0,
        return_volatility,
        volume_ratio
    };
}

Standardizer fit_standardizer(const std::vector<Sample>& samples,
                              const std::size_t training_count) {
    Standardizer result;
    result.scales.fill(1.0);
    for (std::size_t sample_index = 0; sample_index < training_count; ++sample_index) {
        for (std::size_t feature = 0; feature < feature_count; ++feature) {
            result.means[feature] += samples[sample_index].features[feature];
        }
    }
    for (auto& mean : result.means) {
        mean /= static_cast<double>(training_count);
    }
    std::array<double, feature_count> variances{};
    for (std::size_t sample_index = 0; sample_index < training_count; ++sample_index) {
        for (std::size_t feature = 0; feature < feature_count; ++feature) {
            const double delta = samples[sample_index].features[feature] -
                result.means[feature];
            variances[feature] += delta * delta;
        }
    }
    for (std::size_t feature = 0; feature < feature_count; ++feature) {
        const double standard_deviation = std::sqrt(
            variances[feature] / static_cast<double>(training_count));
        if (standard_deviation > 1e-12) {
            result.scales[feature] = standard_deviation;
        }
    }
    return result;
}

LogisticModel fit_model(const std::vector<Sample>& samples,
                        const std::size_t training_count,
                        const Standardizer& standardizer) {
    LogisticModel model;
    for (std::size_t epoch = 0; epoch < training_epochs; ++epoch) {
        std::array<double, feature_count + 1> gradients{};
        for (std::size_t sample_index = 0; sample_index < training_count; ++sample_index) {
            const auto& sample = samples[sample_index];
            const double probability = model.probability(sample.features, standardizer);
            const double error = probability - sample.label;
            gradients[0] += error;
            for (std::size_t feature = 0; feature < feature_count; ++feature) {
                const double standardized =
                    (sample.features[feature] - standardizer.means[feature]) /
                    standardizer.scales[feature];
                gradients[feature + 1] += error * standardized;
            }
        }
        const double sample_count = static_cast<double>(training_count);
        model.weights[0] -= learning_rate * gradients[0] / sample_count;
        for (std::size_t feature = 0; feature < feature_count; ++feature) {
            const auto weight_index = feature + 1;
            const double gradient =
                gradients[weight_index] / sample_count +
                l2_penalty * model.weights[weight_index];
            model.weights[weight_index] -= learning_rate * gradient;
        }
    }
    return model;
}

std::optional<double> calculate_auc(
    const std::vector<std::pair<double, double>>& scored_labels) {
    const auto positives = static_cast<std::size_t>(std::count_if(
        scored_labels.begin(), scored_labels.end(),
        [](const auto& row) { return row.second == 1.0; }));
    const auto negatives = scored_labels.size() - positives;
    if (positives == 0 || negatives == 0) {
        return std::nullopt;
    }

    auto sorted = scored_labels;
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });
    double positive_rank_sum = 0.0;
    std::size_t begin = 0;
    while (begin < sorted.size()) {
        std::size_t end = begin + 1;
        while (end < sorted.size() && sorted[end].first == sorted[begin].first) {
            ++end;
        }
        const double average_rank =
            (static_cast<double>(begin + 1) + static_cast<double>(end)) / 2.0;
        for (std::size_t index = begin; index < end; ++index) {
            if (sorted[index].second == 1.0) {
                positive_rank_sum += average_rank;
            }
        }
        begin = end;
    }
    const double area =
        (positive_rank_sum - static_cast<double>(positives) *
         static_cast<double>(positives + 1) / 2.0) /
        (static_cast<double>(positives) * static_cast<double>(negatives));
    return area;
}

}  // namespace

MlEvaluator::MlEvaluator(common::AppConfig config) : config_(std::move(config)) {}

MlEvaluation MlEvaluator::evaluate(const std::span<const market::Candle> candles) const {
    if (candles.size() <= lookback_candles + forecast_horizon + 30) {
        throw std::invalid_argument(
            "Machine-learning evaluation needs at least 51 chronological candles");
    }
    (void)indicators::calculate(candles, config_.research);

    const double cost_hurdle =
        2.0 * (config_.risk.taker_fee_rate + config_.risk.slippage_rate) +
        config_.signals.maximum_spread_percent;
    if (!std::isfinite(cost_hurdle) || cost_hurdle < 0.0 || cost_hurdle >= 1.0) {
        throw std::invalid_argument("Machine-learning label cost hurdle is invalid");
    }
    const double positive_threshold = std::log1p(cost_hurdle);

    std::vector<Sample> samples;
    for (std::size_t index = lookback_candles;
         index + forecast_horizon < candles.size(); ++index) {
        const double forward_return = std::log(
            candles[index + forecast_horizon].close / candles[index].close);
        const auto features = make_features(candles, index);
        if (!std::isfinite(forward_return) ||
            !std::all_of(features.begin(), features.end(),
                         [](const double value) { return std::isfinite(value); })) {
            throw std::invalid_argument("Machine-learning sample contains non-finite values");
        }
        samples.push_back({
            .features = features,
            .label = forward_return > positive_threshold ? 1.0 : 0.0,
            .feature_index = index
        });
    }

    const std::size_t split = static_cast<std::size_t>(
        static_cast<double>(samples.size()) * training_fraction);
    if (split <= forecast_horizon || samples.size() - split < 10) {
        throw std::invalid_argument(
            "Machine-learning evaluation needs at least 10 validation examples after a purged split");
    }
    std::size_t training_count = 0;
    while (training_count < split &&
           samples[training_count].feature_index + forecast_horizon <
               samples[split].feature_index) {
        ++training_count;
    }
    if (training_count < 20) {
        throw std::invalid_argument(
            "Machine-learning evaluation needs at least 20 purged training examples");
    }

    const auto standardizer = fit_standardizer(samples, training_count);
    const auto model = fit_model(samples, training_count, standardizer);
    std::size_t correct = 0;
    std::size_t predicted_positive = 0;
    std::size_t true_positive = 0;
    std::size_t actual_positive = 0;
    std::size_t training_positive = 0;
    double log_loss = 0.0;
    double brier_score = 0.0;
    double validation_positive_rate = 0.0;
    std::vector<std::pair<double, double>> scored_labels;
    scored_labels.reserve(samples.size() - split);
    for (std::size_t index = 0; index < training_count; ++index) {
        training_positive += samples[index].label == 1.0 ? 1U : 0U;
    }
    const double training_majority_accuracy = std::max(
        static_cast<double>(training_positive),
        static_cast<double>(training_count - training_positive)) /
        static_cast<double>(training_count);

    for (std::size_t index = split; index < samples.size(); ++index) {
        const auto& sample = samples[index];
        const double probability = model.probability(sample.features, standardizer);
        const bool predicted = probability >= 0.5;
        const bool actual = sample.label == 1.0;
        correct += predicted == actual ? 1U : 0U;
        predicted_positive += predicted ? 1U : 0U;
        true_positive += predicted && actual ? 1U : 0U;
        actual_positive += actual ? 1U : 0U;
        validation_positive_rate += actual ? 1.0 : 0.0;
        const double clipped_probability = std::clamp(probability, 1e-15, 1.0 - 1e-15);
        log_loss -= actual ? std::log(clipped_probability)
                           : std::log(1.0 - clipped_probability);
        const double error = probability - sample.label;
        brier_score += error * error;
        scored_labels.emplace_back(probability, sample.label);
    }

    const std::size_t validation_count = samples.size() - split;
    return {
        .candles_processed = candles.size(),
        .training_samples = training_count,
        .validation_samples = validation_count,
        .training_last_feature_index = samples[training_count - 1].feature_index,
        .validation_first_feature_index = samples[split].feature_index,
        .horizon_candles = forecast_horizon,
        .validation_positive_rate =
            validation_positive_rate / static_cast<double>(validation_count),
        .validation_accuracy =
            static_cast<double>(correct) / static_cast<double>(validation_count),
        .majority_class_baseline_accuracy = training_majority_accuracy,
        .precision = predicted_positive > 0
            ? static_cast<double>(true_positive) / static_cast<double>(predicted_positive)
            : 0.0,
        .recall = actual_positive > 0
            ? static_cast<double>(true_positive) / static_cast<double>(actual_positive)
            : 0.0,
        .log_loss = log_loss / static_cast<double>(validation_count),
        .brier_score = brier_score / static_cast<double>(validation_count),
        .roc_auc = calculate_auc(scored_labels)
    };
}

}  // namespace crypto_trader::ml
