#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "common/config.hpp"
#include "market/candle.hpp"

namespace crypto_trader::ml {

struct MlEvaluation {
    std::size_t candles_processed;
    std::size_t training_samples;
    std::size_t validation_samples;
    std::size_t training_last_feature_index;
    std::size_t validation_first_feature_index;
    std::size_t horizon_candles;
    double validation_positive_rate;
    double validation_accuracy;
    double majority_class_baseline_accuracy;
    double precision;
    double recall;
    double log_loss;
    double brier_score;
    std::optional<double> roc_auc;
};

class MlEvaluator {
public:
    explicit MlEvaluator(common::AppConfig config);

    MlEvaluation evaluate(std::span<const market::Candle> candles) const;

private:
    common::AppConfig config_;
};

}  // namespace crypto_trader::ml
