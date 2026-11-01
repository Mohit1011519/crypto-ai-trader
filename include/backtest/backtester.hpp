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

struct MarketHistory {
    std::string symbol;
    std::span<const market::Candle> candles;
};

struct YearlyReturn {
    int year;
    double return_fraction;
};

// All markets traded simultaneously from one shared account, exactly as the
// live trader does: shared cash, open-position and exposure limits, and one
// daily-loss/drawdown kill switch. This reveals correlated drawdowns that
// single-symbol runs hide.
struct PortfolioBacktestResult {
    std::size_t bars_processed;
    double initial_equity;
    double final_equity;
    double total_return_fraction;
    // Equal-weight buy-and-hold of the same markets.
    double buy_and_hold_return_fraction;
    double maximum_drawdown_fraction;
    double worst_day_return_fraction;
    double per_bar_sharpe_ratio;
    std::size_t completed_trades;
    std::size_t winning_trades;
    std::size_t losing_trades;
    std::optional<double> profit_factor;
    int maximum_concurrent_positions;
    bool risk_shutdown;
    std::vector<YearlyReturn> yearly_returns;
    std::vector<BacktestTrade> trades;
};

class Backtester {
public:
    explicit Backtester(common::AppConfig config);

    BacktestResult run(const std::string& symbol,
                       std::span<const market::Candle> candles) const;
    PortfolioBacktestResult run_portfolio(std::span<const MarketHistory> markets) const;

private:
    common::AppConfig config_;
};

}  // namespace crypto_trader::backtest
