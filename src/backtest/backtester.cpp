#include "backtest/backtester.hpp"

#include "ai/signal_engine.hpp"
#include "indicators/technical_indicators.hpp"
#include "market/market_scanner.hpp"
#include "market/trend_analysis.hpp"
#include "market/volatility_analysis.hpp"
#include "market/volume_analysis.hpp"
#include "risk/exit_rules.hpp"
#include "risk/risk_manager.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace crypto_trader::backtest {
namespace {

std::string utc_day(const std::int64_t timestamp_ms) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm utc_time{};
#ifdef _WIN32
    if (gmtime_s(&utc_time, &seconds) != 0) {
        throw std::invalid_argument("Unable to convert backtest timestamp to UTC");
    }
#else
    if (gmtime_r(&seconds, &utc_time) == nullptr) {
        throw std::invalid_argument("Unable to convert backtest timestamp to UTC");
    }
#endif
    char buffer[11]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &utc_time) == 0) {
        throw std::invalid_argument("Unable to format backtest UTC date");
    }
    return buffer;
}

market::ScanResult analyze_prefix(const std::string& symbol,
                                  const std::span<const market::Candle> candles,
                                  const common::AppConfig& config,
                                  ai::SignalEngine& signal_engine) {
    market::ScanResult result{.symbol = symbol};
    result.technical = indicators::calculate(candles, config.research);
    result.trend = market::analyze_trend(candles, *result.technical, config.research);
    result.volume = market::analyze_volume(candles, config.research);
    result.volatility = market::analyze_volatility(
        candles, result.technical->atr, config.research);
    result.order_book_error = "Historical order-book snapshots are unavailable";
    result.signal = signal_engine.evaluate(result);
    return result;
}

int utc_year(const std::int64_t timestamp_ms) {
    return std::stoi(utc_day(timestamp_ms).substr(0, 4));
}

struct OpenPosition {
    risk::RiskPlan plan;
    risk::ProtectiveStopState stop;
    double entry_price;
    double entry_fee;
    std::int64_t entry_time;
};

struct MarketState {
    std::string symbol;
    std::span<const market::Candle> candles;
    std::size_t next = 0;
    double mark = 0.0;
    std::optional<OpenPosition> position;
};

}  // namespace

Backtester::Backtester(common::AppConfig config) : config_(std::move(config)) {}

BacktestResult Backtester::run(const std::string& symbol,
                               const std::span<const market::Candle> candles) const {
    const std::vector<MarketHistory> markets{{symbol, candles}};
    const auto portfolio = run_portfolio(markets);
    std::size_t wins = 0;
    std::size_t losses = 0;
    for (const auto& trade : portfolio.trades) {
        (trade.net_pnl > 0.0 ? wins : losses) += 1;
    }
    return {
        .symbol = symbol,
        .candles_processed = candles.size(),
        .initial_equity = portfolio.initial_equity,
        .final_equity = portfolio.final_equity,
        .total_return_fraction = portfolio.total_return_fraction,
        .buy_and_hold_return_fraction = portfolio.buy_and_hold_return_fraction,
        .maximum_drawdown_fraction = portfolio.maximum_drawdown_fraction,
        .per_bar_sharpe_ratio = portfolio.per_bar_sharpe_ratio,
        .completed_trades = portfolio.completed_trades,
        .winning_trades = wins,
        .losing_trades = losses,
        .profit_factor = portfolio.profit_factor,
        .risk_shutdown = portfolio.risk_shutdown,
        .trades = portfolio.trades
    };
}

PortfolioBacktestResult Backtester::run_portfolio(
    const std::span<const MarketHistory> histories) const {
    if (histories.empty()) {
        throw std::invalid_argument("Backtest requires at least one market");
    }
    std::set<std::string> unique_symbols;
    for (const auto& history : histories) {
        if (history.symbol.empty() || !unique_symbols.insert(history.symbol).second) {
            throw std::invalid_argument("Backtest symbol must not be empty or duplicated");
        }
        if (history.candles.size() < 3) {
            throw std::invalid_argument("Backtest requires at least three chronological candles");
        }
    }
    const double spread = config_.backtest.spread_fraction.value_or(
        config_.signals.maximum_spread_percent);
    if (!std::isfinite(config_.paper_initial_balance) ||
        config_.paper_initial_balance <= 0.0 ||
        !std::isfinite(spread) || spread < 0.0 || spread >= 1.0) {
        throw std::invalid_argument("Backtest starting balance or assumed spread is invalid");
    }

    std::vector<MarketState> markets;
    std::set<std::int64_t> timeline;
    for (const auto& history : histories) {
        // Validates chronology and candle values before simulating.
        (void)indicators::calculate(history.candles, config_.research);
        markets.push_back({.symbol = history.symbol, .candles = history.candles});
        for (const auto& candle : history.candles) {
            timeline.insert(candle.open_time_ms);
        }
    }

    // Without historical books the engine's cost estimate falls back to
    // maximum_spread_percent, so align it with the spread charged on fills.
    auto signal_settings = config_.signals;
    signal_settings.maximum_spread_percent = spread;
    ai::SignalEngine signal_engine(signal_settings);
    risk::RiskManager risk_manager(config_.risk);
    // Live scans only ever see candle_limit closed candles, so indicators are
    // computed over the same trailing window to keep EMA seeding identical.
    const auto lookback = static_cast<std::size_t>(config_.candle_limit);

    double cash = config_.paper_initial_balance;
    double daily_starting_equity = cash;
    double peak_equity = cash;
    double equity = cash;
    std::string current_day = utc_day(*timeline.begin());
    int current_year = utc_year(*timeline.begin());
    double year_starting_equity = cash;
    std::vector<YearlyReturn> yearly_returns;
    double worst_day = 0.0;
    bool halted = false;
    bool flattened = false;
    int maximum_concurrent = 0;
    double sum_bar_returns = 0.0;
    double sum_squared_bar_returns = 0.0;
    std::size_t bar_return_count = 0;
    std::size_t wins = 0;
    std::size_t losses = 0;
    double gross_profit = 0.0;
    double gross_loss = 0.0;
    std::vector<BacktestTrade> trades;

    const auto current_equity = [&] {
        double value = cash;
        for (const auto& market : markets) {
            if (market.position) {
                value += market.position->plan.position_quantity * market.mark;
            }
        }
        return value;
    };
    const auto account_snapshot = [&] {
        const double value = current_equity();
        double exposure = 0.0;
        std::vector<std::string> symbols;
        for (const auto& market : markets) {
            if (market.position) {
                exposure += market.position->plan.position_quantity * market.mark;
                symbols.push_back(market.symbol);
            }
        }
        return risk::PortfolioRiskSnapshot{
            .equity = value,
            .available_balance = std::min(cash, value),
            .gross_exposure = exposure,
            .daily_starting_equity = daily_starting_equity,
            .daily_net_pnl = value - daily_starting_equity,
            .peak_equity = std::max(peak_equity, value),
            .open_positions = static_cast<int>(symbols.size()),
            .open_symbols = std::move(symbols),
            .utc_day = current_day
        };
    };

    const auto close_position = [&](MarketState& market,
                                    const double trigger_price,
                                    const std::int64_t timestamp,
                                    const char* reason) {
        auto& position = *market.position;
        const double quantity = position.plan.position_quantity;
        const double exit_price = trigger_price *
            (1.0 - config_.risk.slippage_rate - spread / 2.0);
        if (!std::isfinite(exit_price) || exit_price <= 0.0) {
            throw std::invalid_argument("Backtest simulated exit price is invalid");
        }
        const double exit_notional = quantity * exit_price;
        const double exit_fee = exit_notional * config_.risk.taker_fee_rate;
        const double net_pnl = quantity * (exit_price - position.entry_price) -
            position.entry_fee - exit_fee;
        if (!std::isfinite(exit_notional) || !std::isfinite(exit_fee) ||
            !std::isfinite(net_pnl)) {
            throw std::invalid_argument("Backtest simulated exit contains non-finite values");
        }
        cash += exit_notional - exit_fee;
        trades.push_back({
            .symbol = market.symbol,
            .exit_reason = reason,
            .quantity = quantity,
            .entry_price = position.entry_price,
            .exit_price = exit_price,
            .fees = position.entry_fee + exit_fee,
            .net_pnl = net_pnl,
            .opened_at_ms = position.entry_time,
            .closed_at_ms = timestamp
        });
        if (net_pnl > 0.0) {
            ++wins;
            gross_profit += net_pnl;
        } else {
            ++losses;
            gross_loss += -net_pnl;
        }
        market.position.reset();
    };

    double maximum_drawdown = 0.0;
    double previous_bar_equity = cash;
    std::vector<std::optional<market::ScanResult>> research(markets.size());

    for (const auto timestamp : timeline) {
        std::vector<std::size_t> active;
        for (std::size_t m = 0; m < markets.size(); ++m) {
            auto& market = markets[m];
            if (market.next < market.candles.size() &&
                market.candles[market.next].open_time_ms == timestamp) {
                active.push_back(m);
            }
        }

        for (const auto m : active) {
            const auto& market = markets[m];
            research[m].reset();
            const std::size_t index = market.next;
            if (index == 0) {
                continue;
            }
            const std::size_t window_start = index > lookback ? index - lookback : 0;
            research[m] = analyze_prefix(
                market.symbol,
                market.candles.subspan(window_start, index - window_start),
                config_, signal_engine);
        }

        const auto day = utc_day(timestamp);
        if (day != current_day) {
            if (daily_starting_equity > 0.0) {
                worst_day = std::min(worst_day, equity / daily_starting_equity - 1.0);
            }
            current_day = day;
            daily_starting_equity = equity;
        }
        const int year = utc_year(timestamp);
        if (year != current_year) {
            yearly_returns.push_back({current_year, equity / year_starting_equity - 1.0});
            current_year = year;
            year_starting_equity = equity;
        }

        for (const auto m : active) {
            markets[m].mark = markets[m].candles[markets[m].next].open;
        }
        peak_equity = std::max(peak_equity, account_snapshot().equity);
        halted = halted || risk_manager.monitor(account_snapshot()).emergency_shutdown;

        if (halted && !flattened && config_.live.flatten_on_risk_shutdown) {
            // Mirrors the live kill switch: every position is sold at market.
            for (auto& market : markets) {
                if (market.position) {
                    close_position(market, market.mark, timestamp, "risk shutdown");
                }
            }
            flattened = true;
        }

        for (const auto m : active) {
            auto& market = markets[m];
            if (!market.position || !research[m]) {
                continue;
            }
            const auto& candle = market.candles[market.next];
            const auto& bar_research = *research[m];
            auto& position = *market.position;
            // The previous bar has closed; ratchet the stop before this bar trades.
            const double atr = bar_research.technical && bar_research.technical->atr
                ? *bar_research.technical->atr : 0.0;
            position.plan.stop_loss_price = risk::advance_protective_stop(
                position.stop, market.candles[market.next - 1].close, atr, config_.risk);
            const bool trailed =
                position.plan.stop_loss_price > position.stop.initial_stop_price;
            const bool stop_touched = candle.low <= position.plan.stop_loss_price;
            const bool target_touched = candle.high >= position.plan.take_profit_price;
            const auto signal = bar_research.signal
                ? bar_research.signal->action : ai::Signal::hold;
            if (stop_touched) {
                const double stop_fill = candle.open <= position.plan.stop_loss_price
                    ? candle.open : position.plan.stop_loss_price;
                close_position(market, stop_fill, candle.open_time_ms,
                               target_touched ? "stop (both levels touched; stop-first)" :
                               trailed ? "trailing stop" : "protective stop");
            } else if (target_touched) {
                close_position(market, position.plan.take_profit_price, candle.open_time_ms,
                               "take-profit");
            } else if (signal == ai::Signal::sell) {
                close_position(market, candle.open, candle.open_time_ms, "advisory sell signal");
            } else if (bar_research.signal && bar_research.signal->exit_long) {
                close_position(market, candle.open, candle.open_time_ms, "trend break");
            }
        }

        for (const auto m : active) {
            auto& market = markets[m];
            if (halted || market.position || !research[m]) {
                continue;
            }
            const auto& bar_research = *research[m];
            if (!bar_research.signal || bar_research.signal->action != ai::Signal::buy ||
                !bar_research.technical || !bar_research.technical->atr ||
                !std::isfinite(*bar_research.technical->atr) ||
                *bar_research.technical->atr <= 0.0) {
                continue;
            }
            const auto& candle = market.candles[market.next];
            const double estimated_entry = candle.open *
                (1.0 + config_.risk.slippage_rate + spread / 2.0);
            const auto decision = risk_manager.assess({
                .symbol = market.symbol,
                .side = risk::OrderSide::buy,
                .entry_price = estimated_entry,
                .atr = *bar_research.technical->atr,
                .spread_fraction = spread
            }, account_snapshot());
            if (decision.emergency_shutdown) {
                halted = true;
                continue;
            }
            if (!decision.approved || !decision.plan) {
                continue;
            }
            const auto& plan = *decision.plan;
            const double notional = plan.position_quantity * estimated_entry;
            const double fee = notional * config_.risk.taker_fee_rate;
            if (!std::isfinite(notional) || !std::isfinite(fee) || notional + fee > cash) {
                throw std::logic_error("Risk-approved backtest entry exceeds available cash");
            }
            cash -= notional + fee;
            market.position = OpenPosition{
                .plan = plan,
                .stop = {
                    .entry_price = estimated_entry,
                    .initial_stop_price = plan.stop_loss_price,
                    .stop_price = plan.stop_loss_price,
                    .highest_close = estimated_entry
                },
                .entry_price = estimated_entry,
                .entry_fee = fee,
                .entry_time = candle.open_time_ms
            };

            const bool stop_touched = candle.low <= plan.stop_loss_price;
            const bool target_touched = candle.high >= plan.take_profit_price;
            if (stop_touched) {
                const double stop_fill = candle.open <= plan.stop_loss_price
                    ? candle.open : plan.stop_loss_price;
                close_position(market, stop_fill, candle.close_time_ms,
                               target_touched ? "stop (both levels touched; stop-first)"
                                              : "protective stop");
            } else if (target_touched) {
                close_position(market, plan.take_profit_price, candle.close_time_ms,
                               "take-profit");
            }
        }

        int open_now = 0;
        for (const auto m : active) {
            markets[m].mark = markets[m].candles[markets[m].next].close;
            ++markets[m].next;
        }
        for (const auto& market : markets) {
            open_now += market.position ? 1 : 0;
        }
        maximum_concurrent = std::max(maximum_concurrent, open_now);

        equity = current_equity();
        peak_equity = std::max(peak_equity, equity);
        halted = halted || risk_manager.monitor(account_snapshot()).emergency_shutdown;
        maximum_drawdown = std::max(maximum_drawdown, (peak_equity - equity) / peak_equity);
        if (previous_bar_equity > 0.0) {
            const double bar_return = (equity - previous_bar_equity) / previous_bar_equity;
            sum_bar_returns += bar_return;
            sum_squared_bar_returns += bar_return * bar_return;
            ++bar_return_count;
        }
        previous_bar_equity = equity;
    }

    equity = current_equity();
    worst_day = std::min(worst_day, equity / daily_starting_equity - 1.0);
    yearly_returns.push_back({current_year, equity / year_starting_equity - 1.0});
    double per_bar_sharpe = 0.0;
    if (bar_return_count > 1) {
        const double mean = sum_bar_returns / static_cast<double>(bar_return_count);
        const double variance = std::max(
            0.0, sum_squared_bar_returns / static_cast<double>(bar_return_count) - mean * mean);
        if (variance > 0.0) {
            per_bar_sharpe = mean / std::sqrt(variance);
        }
    }
    double buy_and_hold = 0.0;
    for (const auto& history : histories) {
        buy_and_hold += history.candles.back().close / history.candles.front().close - 1.0;
    }
    buy_and_hold /= static_cast<double>(histories.size());

    return {
        .bars_processed = timeline.size(),
        .initial_equity = config_.paper_initial_balance,
        .final_equity = equity,
        .total_return_fraction = equity / config_.paper_initial_balance - 1.0,
        .buy_and_hold_return_fraction = buy_and_hold,
        .maximum_drawdown_fraction = maximum_drawdown,
        .worst_day_return_fraction = worst_day,
        .per_bar_sharpe_ratio = per_bar_sharpe,
        .completed_trades = trades.size(),
        .winning_trades = wins,
        .losing_trades = losses,
        .profit_factor = gross_loss > 0.0
            ? std::optional<double>(gross_profit / gross_loss)
            : std::nullopt,
        .maximum_concurrent_positions = maximum_concurrent,
        .risk_shutdown = halted,
        .yearly_returns = std::move(yearly_returns),
        .trades = std::move(trades)
    };
}

}  // namespace crypto_trader::backtest
