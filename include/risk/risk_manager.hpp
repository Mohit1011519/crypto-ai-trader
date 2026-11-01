#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/config.hpp"

namespace crypto_trader::risk {

enum class OrderSide {
    buy,
    sell
};

struct PortfolioRiskSnapshot {
    double equity;
    double available_balance;
    double gross_exposure;
    double daily_starting_equity;
    double daily_net_pnl;
    double peak_equity;
    int open_positions;
    std::vector<std::string> open_symbols;
    // UTC trading day (YYYY-MM-DD). When set, a daily-loss breach pauses new
    // entries until the day changes instead of latching a full shutdown;
    // only the drawdown limit latches. Empty keeps the legacy latch.
    std::string utc_day = {};
};

struct OrderIntent {
    std::string symbol;
    OrderSide side;
    double entry_price;
    double atr;
    double spread_fraction = 0.0;
};

struct RiskPlan {
    double position_quantity;
    double position_notional;
    double risk_budget;
    double maximum_planned_loss;
    double stop_loss_price;
    double take_profit_price;
    double estimated_round_trip_cost;
    double net_reward_risk_ratio;
    double gross_exposure_after;
};

struct RiskDecision {
    bool approved;
    bool emergency_shutdown;
    std::optional<RiskPlan> plan;
    std::vector<std::string> reasons;
};

struct RiskStatus {
    bool emergency_shutdown;
    double daily_loss_fraction;
    double drawdown_fraction;
    std::vector<std::string> reasons;
    bool daily_pause = false;
};

class RiskManager {
public:
    explicit RiskManager(common::RiskSettings settings);

    RiskStatus monitor(const PortfolioRiskSnapshot& snapshot);
    RiskDecision assess(const OrderIntent& order,
                        const PortfolioRiskSnapshot& snapshot);
    void reset_emergency_shutdown(const PortfolioRiskSnapshot& snapshot);

private:
    common::RiskSettings settings_;
    mutable std::mutex mutex_;
    bool emergency_shutdown_ = false;
    std::vector<std::string> shutdown_reasons_;
    std::string paused_day_;
};

}  // namespace crypto_trader::risk
