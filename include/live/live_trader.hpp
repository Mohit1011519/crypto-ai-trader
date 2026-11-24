#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/config.hpp"
#include "execution/spot_venue.hpp"
#include "risk/risk_manager.hpp"

namespace crypto_trader::market {
struct ScanResult;
}

namespace crypto_trader::live {

struct LivePosition {
    std::string symbol;
    std::string base_asset;
    double quantity;
    double entry_price;
    // Quote spent including quote-denominated commission.
    double entry_cost;
    double mark_price;
    double initial_stop_loss_price;
    // Ratcheted stop from the shared exit rules.
    double stop_loss_price;
    // Trigger price of the stop order resting on the exchange.
    double exchange_stop_price;
    double take_profit_price;
    double highest_close;
    std::int64_t stop_order_id;
    std::int64_t opened_at_ms;
};

struct LiveExecution {
    std::uint64_t id;
    std::string symbol;
    std::string side;
    std::string reason;
    double quantity;
    double price;
    double fee;
    double realized_pnl;
    std::int64_t order_id;
    std::int64_t timestamp_ms;
};

// An entry order that was sent (or was about to be sent) to the venue but
// whose result is not yet in the ledger. It is persisted before the order is
// submitted, so a lost response or a crash right after the fill is found by
// client order id on the next cycle and the position is recorded and
// protected instead of being left untracked on the exchange.
struct PendingEntry {
    std::string symbol;
    std::string client_order_id;
    // ATR at venue prices when the order was sent; sets the initial stop.
    double atr;
    std::int64_t candle_ms;
    std::int64_t created_at_ms;
};

struct LiveAccountView {
    double cash_balance;
    double capital_allocation;
    double equity;
    double gross_exposure;
    double daily_net_pnl;
    double peak_equity;
    bool trading_halted;
    std::vector<LivePosition> positions;
    std::optional<PendingEntry> pending_entry;
};

// Automated spot trader for an authenticated venue. Every position is
// protected by an exchange-resident stop-market order so that losses stay
// bounded if this process stops. Local state is persisted after every order so
// a crash never forgets an exchange position. The ledger only manages
// `capital_allocation`; other account funds are never traded.
class LiveTrader {
public:
    LiveTrader(common::LiveSettings live_settings,
               common::RiskSettings risk_settings,
               execution::SpotVenue& venue);

    std::vector<LiveExecution> process_scan(const std::vector<market::ScanResult>& results,
                                            risk::RiskManager& risk_manager,
                                            std::int64_t timestamp_ms = 0);
    // Cancels every stop, market-sells every position, and halts new entries.
    std::vector<LiveExecution> flatten(const std::string& reason, std::int64_t timestamp_ms = 0);
    void reset_risk_shutdown(risk::RiskManager& risk_manager);
    LiveAccountView view() const;
    std::vector<LiveExecution> execution_history() const;

private:
    struct State {
        double cash_balance;
        double capital_allocation;
        double daily_starting_equity;
        double peak_equity;
        std::string utc_day;
        bool trading_halted;
        std::uint64_t next_execution_id;
        std::uint64_t next_client_sequence;
        std::vector<LivePosition> positions;
        std::vector<LiveExecution> executions;
        // Closed-candle time of the last entry per symbol, so a restart never
        // re-buys on a signal that was already acted on.
        std::map<std::string, std::int64_t> last_entry_candle_ms;
        std::optional<PendingEntry> pending_entry;
    };

    double equity_unlocked() const;
    risk::PortfolioRiskSnapshot snapshot_unlocked(double quote_free) const;
    std::string next_client_order_id();
    bool protect_position(LivePosition& position);
    void close_position(std::size_t index, const execution::VenueOrder& order,
                        const std::string& reason, std::int64_t timestamp_ms,
                        std::vector<LiveExecution>& executions);
    bool market_exit(std::size_t index, const std::string& reason, std::int64_t timestamp_ms,
                     std::vector<LiveExecution>& executions);
    bool manage_position(std::size_t index, const market::ScanResult* result,
                         std::int64_t timestamp_ms, std::vector<LiveExecution>& executions);
    void open_position(const market::ScanResult& result, risk::RiskManager& risk_manager,
                       std::int64_t timestamp_ms, std::vector<LiveExecution>& executions);
    // Books a filled entry into the ledger, clears the pending entry, and
    // places the protective stop (selling immediately if that fails).
    void record_entry(const std::string& symbol, const execution::VenueOrder& order,
                      double atr, std::int64_t candle_ms, std::int64_t timestamp_ms,
                      const std::string& reason, std::vector<LiveExecution>& executions);
    // Returns true when no entry is pending any more.
    bool resolve_pending_entry(std::int64_t timestamp_ms,
                               std::vector<LiveExecution>& executions);
    std::vector<LiveExecution> flatten_unlocked(const std::string& reason,
                                                std::int64_t timestamp_ms);
    void record(LiveExecution execution, std::vector<LiveExecution>& executions);
    nlohmann::json serialize() const;
    void load(const nlohmann::json& input);
    void persist() const;

    common::LiveSettings live_settings_;
    common::RiskSettings risk_settings_;
    execution::SpotVenue& venue_;
    mutable std::mutex mutex_;
    State state_;
};

}  // namespace crypto_trader::live
