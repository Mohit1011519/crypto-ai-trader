#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/config.hpp"
#include "risk/risk_manager.hpp"

namespace crypto_trader::market {
struct ScanResult;
}

namespace crypto_trader::paper {

struct PaperPosition {
    std::string symbol;
    double quantity;
    double entry_price;
    double mark_price;
    double stop_loss_price;
    double take_profit_price;
    double entry_fee;
    std::int64_t opened_at_ms;
};

struct PaperExecution {
    std::uint64_t id;
    std::string symbol;
    std::string side;
    std::string reason;
    double quantity;
    double price;
    double fee;
    double realized_pnl;
    std::int64_t timestamp_ms;
};

struct PaperPortfolioView {
    double cash_balance;
    double equity;
    double gross_exposure;
    double unrealized_pnl;
    double daily_net_pnl;
    double peak_equity;
    bool trading_halted;
    std::vector<PaperPosition> positions;
};

class PaperPortfolio {
public:
    PaperPortfolio(std::filesystem::path state_path,
                   double initial_balance,
                   common::RiskSettings risk_settings,
                   double fallback_spread_fraction);

    std::vector<PaperExecution> process_scan(
        const std::vector<market::ScanResult>& results,
        risk::RiskManager& risk_manager,
        std::int64_t timestamp_ms = 0);
    PaperPortfolioView view() const;
    std::vector<PaperExecution> execution_history() const;
    void reset_risk_shutdown(risk::RiskManager& risk_manager);

private:
    struct State {
        double cash_balance;
        double initial_balance;
        double daily_starting_equity;
        double peak_equity;
        std::string utc_day;
        bool trading_halted;
        std::uint64_t next_execution_id;
        std::vector<PaperPosition> positions;
        std::vector<PaperExecution> executions;
    };

    PaperPortfolioView view_unlocked(const State& state) const;
    risk::PortfolioRiskSnapshot risk_snapshot_unlocked(const State& state) const;
    void validate_state(const State& state) const;
    nlohmann::json serialize_state(const State& state) const;
    State deserialize_state(const nlohmann::json& input) const;
    void save_state(const State& state) const;

    std::filesystem::path state_path_;
    double initial_balance_;
    common::RiskSettings risk_settings_;
    double fallback_spread_fraction_;
    mutable std::mutex mutex_;
    State state_;
};

}  // namespace crypto_trader::paper
