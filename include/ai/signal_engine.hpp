#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config.hpp"
#include "market/research.hpp"

namespace crypto_trader::market {
struct ScanResult;
}

namespace crypto_trader::ai {

enum class Signal {
    buy,
    sell,
    hold
};

struct SignalComponent {
    std::string name;
    double score;
    double weight;
};

struct TradingSignal {
    market::ResearchMetadata metadata;
    Signal action;
    double research_score;
    double confidence;
    double research_coverage;
    int confirmations;
    std::optional<double> expected_reward_risk_ratio;
    double estimated_round_trip_cost_percent;
    std::vector<SignalComponent> components;
    std::vector<std::string> reasons;
    // The EMA trend has turned against long positions. Evaluated before the
    // entry gates so that exits still fire in volatile or illiquid markets.
    bool exit_long = false;
};

class SignalEngine {
public:
    explicit SignalEngine(common::SignalSettings settings);

    TradingSignal evaluate(const market::ScanResult& research);

private:
    common::SignalSettings settings_;
    std::mutex mutex_;
    struct EmittedSignal {
        Signal action;
        std::int64_t timestamp_ms;
    };
    std::unordered_map<std::string, EmittedSignal> last_emitted_signal_;
};

const char* to_string(Signal signal) noexcept;

}  // namespace crypto_trader::ai
