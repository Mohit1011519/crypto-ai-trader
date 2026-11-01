#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/config.hpp"
#include "market/candle.hpp"

namespace crypto_trader::backtest {

struct BacktestTrade {
    std::string symbol;
    std::string exit_reason;
    double quantity;
    double entry_price;
    double exit_price;
    double fees;
    double net_pnl;
    std::int64_t opened_at_ms;
    std::int64_t closed_at_ms;
};

struct BacktestResult {
    std::string symbol;
    std::size_t candles_processed;
    double initial_equity;
    double final_equity;
    double total_return_fraction;
    double buy_and_hold_return_fraction;
    double maximum_drawdown_fraction;
    double per_bar_sharpe_ratio;
    std::size_t completed_trades;
    std::size_t winning_trades;
    std::size_t losing_trades;
    std::optional<double> profit_factor;
    bool risk_shutdown;
    std::vector<BacktestTrade> trades;
};

class Backtester {
public:
    explicit Backtester(common::AppConfig config);

    BacktestResult run(const std::string& symbol,
                       std::span<const market::Candle> candles) const;

private:
    common::AppConfig config_;
};

}  // namespace crypto_trader::backtest
