#include "backtest/backtester.hpp"

#include "ai/signal_engine.hpp"
#include "indicators/technical_indicators.hpp"
#include "market/market_scanner.hpp"
#include "market/trend_analysis.hpp"
#include "market/volatility_analysis.hpp"
#include "market/volume_analysis.hpp"
#include "risk/risk_manager.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
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

}  // namespace

Backtester::Backtester(common::AppConfig config) : config_(std::move(config)) {}

BacktestResult Backtester::run(const std::string& symbol,
                               const std::span<const market::Candle> candles) const {
    if (symbol.empty()) {
        throw std::invalid_argument("Backtest symbol must not be empty");
    }
    if (candles.size() < 3) {
        throw std::invalid_argument("Backtest requires at least three chronological candles");
    }
    if (!std::isfinite(config_.paper_initial_balance) ||
        config_.paper_initial_balance <= 0.0 ||
        !std::isfinite(config_.signals.maximum_spread_percent) ||
        config_.signals.maximum_spread_percent <= 0.0 ||
        config_.signals.maximum_spread_percent >= 1.0) {
        throw std::invalid_argument("Backtest starting balance or conservative spread is invalid");
    }

    (void)indicators::calculate(candles, config_.research);
    ai::SignalEngine signal_engine(config_.signals);
    risk::RiskManager risk_manager(config_.risk);

    double cash = config_.paper_initial_balance;
    double daily_starting_equity = cash;
    double peak_equity = cash;
    double equity = cash;
    std::string current_day = utc_day(candles.front().close_time_ms);
    bool halted = false;
    std::optional<risk::RiskPlan> open_position;
    double entry_price = 0.0;
    double entry_fee = 0.0;
    std::int64_t entry_time = 0;
    double mark_price = 0.0;
    double sum_bar_returns = 0.0;
    double sum_squared_bar_returns = 0.0;
    std::size_t bar_return_count = 0;
    std::size_t wins = 0;
    std::size_t losses = 0;
    double gross_profit = 0.0;
    double gross_loss = 0.0;
    std::vector<BacktestTrade> trades;

    const auto account_snapshot = [&] {
        const double current_equity = cash +
            (open_position ? open_position->position_quantity * mark_price : 0.0);
        std::vector<std::string> symbols;
        if (open_position) {
            symbols.push_back(symbol);
        }
        return risk::PortfolioRiskSnapshot{
            .equity = current_equity,
            .available_balance = std::min(cash, current_equity),
            .gross_exposure = open_position
                ? open_position->position_quantity * mark_price : 0.0,
            .daily_starting_equity = daily_starting_equity,
            .daily_net_pnl = current_equity - daily_starting_equity,
            .peak_equity = std::max(peak_equity, current_equity),
            .open_positions = open_position ? 1 : 0,
            .open_symbols = std::move(symbols)
        };
    };

    const auto close_position = [&](const double trigger_price,
                                    const std::int64_t timestamp,
                                    const char* reason) {
        const double exit_price = trigger_price *
            (1.0 - config_.risk.slippage_rate -
             config_.signals.maximum_spread_percent / 2.0);
        if (!std::isfinite(exit_price) || exit_price <= 0.0) {
            throw std::invalid_argument("Backtest simulated exit price is invalid");
        }
        const double exit_notional = open_position->position_quantity * exit_price;
        const double exit_fee = exit_notional * config_.risk.taker_fee_rate;
        const double net_pnl = open_position->position_quantity *
            (exit_price - entry_price) - entry_fee - exit_fee;
        if (!std::isfinite(exit_notional) || !std::isfinite(exit_fee) ||
            !std::isfinite(net_pnl)) {
            throw std::invalid_argument("Backtest simulated exit contains non-finite values");
        }
        cash += exit_notional - exit_fee;
        trades.push_back({
            .symbol = symbol,
            .exit_reason = reason,
            .quantity = open_position->position_quantity,
            .entry_price = entry_price,
            .exit_price = exit_price,
            .fees = entry_fee + exit_fee,
            .net_pnl = net_pnl,
            .opened_at_ms = entry_time,
            .closed_at_ms = timestamp
        });
        if (net_pnl > 0.0) {
            ++wins;
            gross_profit += net_pnl;
        } else {
            ++losses;
            gross_loss += -net_pnl;
        }
        open_position.reset();
        entry_price = 0.0;
        entry_fee = 0.0;
        entry_time = 0;
    };

    double maximum_drawdown = 0.0;
    double previous_bar_equity = cash;

    for (std::size_t index = 1; index < candles.size(); ++index) {
        const auto& candle = candles[index];
        const std::span<const market::Candle> prefix(candles.data(), index);
        const auto research = analyze_prefix(symbol, prefix, config_, signal_engine);
        const auto signal = research.signal
            ? research.signal->action : ai::Signal::hold;

        const auto day = utc_day(candle.open_time_ms);
        if (day != current_day) {
            current_day = day;
            daily_starting_equity = equity;
        }

        mark_price = candle.open;
        peak_equity = std::max(peak_equity, account_snapshot().equity);
        auto status = risk_manager.monitor(account_snapshot());
        halted = halted || status.emergency_shutdown;

        if (open_position) {
            const bool stop_touched = candle.low <= open_position->stop_loss_price;
            const bool target_touched = candle.high >= open_position->take_profit_price;
            if (stop_touched) {
                const double stop_fill = candle.open <= open_position->stop_loss_price
                    ? candle.open : open_position->stop_loss_price;
                close_position(stop_fill, candle.open_time_ms,
                               target_touched ? "stop (both levels touched; stop-first)" :
                                                "protective stop");
            } else if (target_touched) {
                close_position(open_position->take_profit_price, candle.open_time_ms,
                               "take-profit");
            } else if (signal == ai::Signal::sell) {
                close_position(candle.open, candle.open_time_ms, "advisory sell signal");
            }
        }

        if (!open_position && signal == ai::Signal::buy && !halted &&
            research.technical && research.technical->atr &&
            std::isfinite(*research.technical->atr) && *research.technical->atr > 0.0) {
            const double estimated_entry = candle.open *
                (1.0 + config_.risk.slippage_rate +
                 config_.signals.maximum_spread_percent / 2.0);
            const auto decision = risk_manager.assess({
                .symbol = symbol,
                .side = risk::OrderSide::buy,
                .entry_price = estimated_entry,
                .atr = *research.technical->atr,
                .spread_fraction = config_.signals.maximum_spread_percent
            }, account_snapshot());
            if (decision.emergency_shutdown) {
                halted = true;
            } else if (decision.approved && decision.plan) {
                const auto& plan = *decision.plan;
                const double fill_price = estimated_entry;
                const double notional = plan.position_quantity * fill_price;
                const double fee = notional * config_.risk.taker_fee_rate;
                if (!std::isfinite(notional) || !std::isfinite(fee) ||
                    notional + fee > cash) {
                    throw std::logic_error("Risk-approved backtest entry exceeds available cash");
                }
                cash -= notional + fee;
                entry_price = fill_price;
                entry_fee = fee;
                entry_time = candle.open_time_ms;
                open_position = plan;

                const bool stop_touched = candle.low <= plan.stop_loss_price;
                const bool target_touched = candle.high >= plan.take_profit_price;
                if (stop_touched) {
                    const double stop_fill = candle.open <= plan.stop_loss_price
                        ? candle.open : plan.stop_loss_price;
                    close_position(stop_fill, candle.close_time_ms,
                                   target_touched
                                       ? "stop (both levels touched; stop-first)"
                                       : "protective stop");
                } else if (target_touched) {
                    close_position(plan.take_profit_price, candle.close_time_ms,
                                   "take-profit");
                }
            }
        }

        mark_price = candle.close;
        equity = account_snapshot().equity;
        peak_equity = std::max(peak_equity, equity);
        status = risk_manager.monitor(account_snapshot());
        halted = halted || status.emergency_shutdown;
        maximum_drawdown = std::max(
            maximum_drawdown, (peak_equity - equity) / peak_equity);
        if (previous_bar_equity > 0.0) {
            const double bar_return = (equity - previous_bar_equity) / previous_bar_equity;
            sum_bar_returns += bar_return;
            sum_squared_bar_returns += bar_return * bar_return;
            ++bar_return_count;
        }
        previous_bar_equity = equity;
    }

    equity = account_snapshot().equity;
    double per_bar_sharpe = 0.0;
    if (bar_return_count > 1) {
        const double mean = sum_bar_returns / static_cast<double>(bar_return_count);
        const double variance = std::max(
            0.0, sum_squared_bar_returns / static_cast<double>(bar_return_count) - mean * mean);
        if (variance > 0.0) {
            per_bar_sharpe = mean / std::sqrt(variance);
        }
    }

    return {
        .symbol = symbol,
        .candles_processed = candles.size(),
        .initial_equity = config_.paper_initial_balance,
        .final_equity = equity,
        .total_return_fraction = equity / config_.paper_initial_balance - 1.0,
        .buy_and_hold_return_fraction = candles.back().close / candles.front().close - 1.0,
        .maximum_drawdown_fraction = maximum_drawdown,
        .per_bar_sharpe_ratio = per_bar_sharpe,
        .completed_trades = trades.size(),
        .winning_trades = wins,
        .losing_trades = losses,
        .profit_factor = gross_loss > 0.0
            ? std::optional<double>(gross_profit / gross_loss)
            : std::nullopt,
        .risk_shutdown = halted,
        .trades = std::move(trades)
    };
}

}  // namespace crypto_trader::backtest
