#include "common/config.hpp"
#include "common/logger.hpp"
#include "common/notifier.hpp"
#include "backtest/backtester.hpp"
#include "execution/order_executor.hpp"
#include "exchange/binance_exchange.hpp"
#include "exchange/binance_market_stream.hpp"
#include "exchange/binance_signed_client.hpp"
#include "execution/paper_venue.hpp"
#include "execution/spot_venue.hpp"
#include "live/live_trader.hpp"
#include "market/market_scanner.hpp"
#include "ml/ml_evaluator.hpp"
#include "risk/risk_manager.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <memory>
#include <cmath>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

std::atomic_bool stop_requested{false};
static_assert(std::atomic_bool::is_always_lock_free);

void handle_signal(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

void print_usage() {
    std::cout << "Usage:\n"
              << "  crypto-ai-trader --candles SYMBOL [config-path]\n"
              << "  crypto-ai-trader --stream SYMBOL [config-path]\n"
              << "  crypto-ai-trader --scan-once [config-path]\n"
              << "  crypto-ai-trader --scan [config-path]\n"
              << "  crypto-ai-trader --paper-once [config-path]\n"
              << "  crypto-ai-trader --paper [config-path]\n"
              << "  crypto-ai-trader --paper-status [config-path]\n"
              << "  crypto-ai-trader --paper-reset [config-path]\n"
              << "  crypto-ai-trader --paper-flatten [config-path]\n"
              << "  crypto-ai-trader --backtest SYMBOL [config-path]\n"
              << "  crypto-ai-trader --backtest-all [config-path]\n"
              << "  crypto-ai-trader --ml-evaluate SYMBOL [config-path]\n"
              << "  crypto-ai-trader --execution-status [config-path]\n"
              << "  crypto-ai-trader --live-once [config-path]      (testnet orders)\n"
              << "  crypto-ai-trader --live [config-path]           (testnet orders)\n"
              << "  crypto-ai-trader --live-status [config-path]\n"
              << "  crypto-ai-trader --live-flatten [config-path]   (sell all, halt)\n"
              << "  crypto-ai-trader --live-reset [config-path]\n"
              << "  crypto-ai-trader --execution-dry-run SYMBOL buy|sell QUANTITY [config-path]\n"
              << "  crypto-ai-trader --help\n";
}

const char* trend_name(const crypto_trader::market::TrendDirection direction) {
    using crypto_trader::market::TrendDirection;
    switch (direction) {
    case TrendDirection::bullish: return "BULLISH";
    case TrendDirection::bearish: return "BEARISH";
    case TrendDirection::sideways: return "SIDEWAYS";
    case TrendDirection::unavailable: return "UNAVAILABLE";
    }
    return "UNAVAILABLE";
}

const char* volatility_name(const crypto_trader::market::VolatilityRisk risk) {
    using crypto_trader::market::VolatilityRisk;
    switch (risk) {
    case VolatilityRisk::normal: return "NORMAL";
    case VolatilityRisk::elevated: return "ELEVATED";
    case VolatilityRisk::high: return "HIGH";
    case VolatilityRisk::unavailable: return "UNAVAILABLE";
    }
    return "UNAVAILABLE";
}

const char* liquidity_name(const crypto_trader::market::LiquidityLevel liquidity) {
    using crypto_trader::market::LiquidityLevel;
    switch (liquidity) {
    case LiquidityLevel::sufficient: return "SUFFICIENT";
    case LiquidityLevel::low: return "LOW";
    case LiquidityLevel::unavailable: return "UNAVAILABLE";
    }
    return "UNAVAILABLE";
}

void print_optional(const std::optional<double>& value, const int precision = 2) {
    if (value) {
        std::cout << std::fixed << std::setprecision(precision) << *value;
    } else {
        std::cout << "n/a";
    }
}

void print_scan(const std::vector<crypto_trader::market::ScanResult>& results,
                const bool paper_mode = false) {
    std::cout << "\nMarket Scan (public data; research and advisory signals only)\n";
    for (const auto& result : results) {
        std::cout << result.symbol << ": ";
        if (result.candle_error) {
            std::cout << "RESEARCH UNAVAILABLE (" << *result.candle_error << ')';
            if (result.signal) {
                std::cout << " Signal=" << crypto_trader::ai::to_string(result.signal->action);
                for (const auto& reason : result.signal->reasons) {
                    std::cout << " Reason=[" << reason << ']';
                }
            }
            std::cout << '\n';
            continue;
        }

        std::cout << "Trend=" << trend_name(result.trend->direction)
                  << " strength=" << std::fixed << std::setprecision(3)
                  << result.trend->strength
                  << " EMA[fast/medium/slow]=";
        print_optional(result.technical->ema_fast, 4);
        std::cout << '/';
        print_optional(result.technical->ema_medium, 4);
        std::cout << '/';
        print_optional(result.technical->ema_slow, 4);
        std::cout << " RSI=";
        print_optional(result.technical->rsi);
        std::cout << " StochRSI=";
        print_optional(result.technical->stochastic_rsi);
        std::cout << " MACD=";
        print_optional(result.technical->macd_histogram);
        std::cout << " Bollinger[lower/upper]=";
        print_optional(result.technical->bollinger_lower, 4);
        std::cout << '/';
        print_optional(result.technical->bollinger_upper, 4);
        std::cout << " VolumeRatio=";
        print_optional(result.volume->volume_ratio);
        std::cout << " VolumeSpike=" << (result.volume->volume_spike ? "YES" : "NO")
                  << " Pressure[buy/sell]=" << std::fixed << std::setprecision(2)
                  << result.volume->buy_pressure << '/'
                  << result.volume->sell_pressure
                  << " VolumePriceDivergence="
                  << (result.volume->volume_price_divergence ? "YES" : "NO")
                  << " ATR%=";
        print_optional(result.volatility->atr_percent, 4);
        std::cout << " Volatility=" << volatility_name(result.volatility->risk)
                  << " SuddenVolatility="
                  << (result.volatility->sudden_volatility ? "YES" : "NO")
                  << " HistoricalVolatility=";
        print_optional(result.volatility->historical_volatility, 6);
        std::cout << " Support/Resistance=";
        print_optional(result.trend->support, 4);
        std::cout << '/';
        print_optional(result.trend->resistance, 4);
        if (result.trend->breakout_up) {
            std::cout << " Breakout=UP";
        } else if (result.trend->breakout_down) {
            std::cout << " Breakout=DOWN";
        }
        if (result.order_book) {
            std::cout << " BookImbalance=" << std::fixed << std::setprecision(3)
                      << result.order_book->imbalance
                      << " Spread%=" << std::setprecision(4)
                      << result.order_book->spread_percent * 100.0
                      << " DepthQuote=" << std::setprecision(2)
                      << result.order_book->quote_liquidity
                      << " LargeOrders[bid/ask]=" << result.order_book->large_bid_count
                      << '/' << result.order_book->large_ask_count
                      << " Liquidity=" << liquidity_name(result.order_book->liquidity)
                      << " OrderBookQuality=" << std::setprecision(2)
                      << result.order_book->metadata.quality;
        } else if (result.order_book_error) {
            std::cout << " OrderBook=UNAVAILABLE (" << *result.order_book_error << ')';
        }
        std::cout << " CandleResearchQuality=" << std::fixed << std::setprecision(2)
                  << std::min({result.technical->metadata.quality,
                               result.trend->metadata.quality,
                               result.volume->metadata.quality,
                               result.volatility->metadata.quality})
                  << " Signal=";
        if (result.signal) {
            std::cout << crypto_trader::ai::to_string(result.signal->action)
                      << " Score=" << std::fixed << std::setprecision(3)
                      << result.signal->research_score
                      << " Confidence=" << std::setprecision(2)
                      << result.signal->confidence
                      << " Coverage=" << result.signal->research_coverage
                      << " Confirmations=" << result.signal->confirmations
                      << " NetR/R=";
            print_optional(result.signal->expected_reward_risk_ratio, 2);
            std::cout << " EstCost%=" << std::setprecision(4)
                      << result.signal->estimated_round_trip_cost_percent * 100.0;
            for (const auto& component : result.signal->components) {
                std::cout << " " << component.name << '='
                          << std::setprecision(2) << component.score;
            }
            for (const auto& reason : result.signal->reasons) {
                std::cout << " Reason=[" << reason << ']';
            }
        } else {
            std::cout << "HOLD (signal unavailable)";
        }
        std::cout << '\n';
    }
    if (paper_mode) {
        std::cout << "Paper simulation only; fills are estimates and no exchange orders are sent.\n";
    } else {
        std::cout << "Signals are advisory only; positions are not tracked and no orders are placed.\n";
    }
}

void require_configured_symbol(const crypto_trader::common::AppConfig& config,
                               const std::string& symbol) {
    if (std::find(config.trading_pairs.begin(), config.trading_pairs.end(), symbol) ==
        config.trading_pairs.end()) {
        throw std::invalid_argument("Symbol is not listed in the configuration: " + symbol);
    }
}

int fetch_candles(const crypto_trader::common::AppConfig& config, const std::string& symbol) {
    crypto_trader::exchange::BinanceExchange exchange(config);
    const auto candles = exchange.fetch_candles(
        symbol, config.candle_interval, config.candle_limit);

    std::cout << "Mode: PAPER (public market data only)\n"
              << "Exchange: Binance\n"
              << "Market: " << symbol << '\n'
              << "Candles: " << candles.size() << "\n\n"
              << "Open time (ms)       Open       High        Low      Close     Volume\n";
    for (const auto& candle : candles) {
        std::cout << std::setw(18) << candle.open_time_ms << ' '
                  << std::fixed << std::setprecision(8)
                  << std::setw(10) << candle.open << ' '
                  << std::setw(10) << candle.high << ' '
                  << std::setw(10) << candle.low << ' '
                  << std::setw(10) << candle.close << ' '
                  << std::setw(10) << candle.volume << '\n';
    }
    return 0;
}

int stream_prices(const crypto_trader::common::AppConfig& config, const std::string& symbol) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    crypto_trader::exchange::BinanceMarketStream stream(config);
    spdlog::info("Streaming {}. Press Ctrl+C to stop.", symbol);
    stream.run(symbol, [](const crypto_trader::exchange::PriceUpdate& update) {
        std::cout << update.event_time_ms << ' ' << update.symbol << ' '
                  << std::fixed << std::setprecision(8) << update.price << '\n';
    }, stop_requested);
    return 0;
}

int scan_markets(const crypto_trader::common::AppConfig& config, const bool repeat) {
    if (repeat) {
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
    }
    crypto_trader::exchange::BinanceExchange exchange(config);
    crypto_trader::market::MarketScanner scanner(exchange, config);
    if (repeat) {
        spdlog::info("Scanning {} configured markets every {} seconds. Press Ctrl+C to stop.",
                     config.trading_pairs.size(), config.research.scan_interval_seconds);
        scanner.run(stop_requested, [](const auto& results) {
            print_scan(results);
        });
        return 0;
    }
    print_scan(scanner.scan_once());
    return 0;
}

// Plain-language explanation of what the bot decided for one market.
std::string decision_text(const crypto_trader::market::ScanResult& result) {
    using crypto_trader::market::TrendDirection;
    if (result.candle_error || !result.trend || !result.technical) {
        return "No data: " + result.candle_error.value_or("research unavailable");
    }
    if (result.signal && result.signal->action == crypto_trader::ai::Signal::buy) {
        return "BUY signal: uptrend breakout confirmed";
    }
    std::ostringstream text;
    switch (result.trend->direction) {
    case TrendDirection::bearish:
        return "Downtrend: not buying (open positions here are sold)";
    case TrendDirection::sideways:
        return result.signal && result.signal->exit_long
            ? "Trend weakening: not buying (open positions here are sold)"
            : "No clear trend: not buying";
    case TrendDirection::unavailable:
        return "Not enough history yet";
    case TrendDirection::bullish:
        break;
    }
    if (!result.trend->breakout_up && result.trend->resistance) {
        text << "Uptrend: waiting for breakout above " << std::fixed
             << std::setprecision(result.technical->close < 10.0 ? 4 : 2)
             << *result.trend->resistance;
        return text.str();
    }
    if (result.signal && !result.signal->reasons.empty()) {
        return "Uptrend, but filtered: " + result.signal->reasons.front();
    }
    return "Uptrend: no signal";
}

void print_market_summary(const std::vector<crypto_trader::market::ScanResult>& results,
                          const int next_check_seconds) {
    std::ostringstream table;
    table << "\n---- Market check (strategy: " << results.size()
          << " coins, buys only in uptrend breakouts) ----\n";
    for (const auto& result : results) {
        const auto action = result.signal &&
            result.signal->action == crypto_trader::ai::Signal::buy ? "BUY" : "WAIT";
        table << std::left << std::setw(9) << result.symbol << ' ';
        if (result.technical) {
            table << std::right << std::setw(12) << std::fixed
                  << std::setprecision(result.technical->close < 10.0 ? 4 : 2)
                  << result.technical->close;
        } else {
            table << std::setw(12) << "-";
        }
        table << "  " << std::left << std::setw(4) << action << "  "
              << decision_text(result) << '\n';
        spdlog::info("{} {} {}", result.symbol, action, decision_text(result));
    }
    if (next_check_seconds > 0) {
        table << "Next check in " << next_check_seconds
              << "s (signals update when each 4h candle closes).\n";
    }
    std::cout << table.str();
}

void print_live_account(const char* label,
                        const crypto_trader::live::LiveAccountView& account,
                        const std::vector<crypto_trader::live::LiveExecution>& executions) {
    std::cout << '\n' << label << " Account"
              << " Allocation=" << std::fixed << std::setprecision(2)
              << account.capital_allocation
              << " Cash=" << account.cash_balance
              << " Equity=" << account.equity
              << " Return=" << (account.equity / account.capital_allocation - 1.0) * 100.0 << '%'
              << " GrossExposure=" << account.gross_exposure
              << " DailyPnL=" << account.daily_net_pnl
              << " PeakEquity=" << account.peak_equity
              << " Trading=" << (account.trading_halted ? "HALTED" : "ENABLED") << '\n';
    for (const auto& position : account.positions) {
        std::cout << "  " << position.symbol
                  << " Qty=" << std::setprecision(8) << position.quantity
                  << " Entry=" << std::setprecision(4) << position.entry_price
                  << " Mark=" << position.mark_price
                  << " Stop=" << position.stop_loss_price
                  << " ExchangeStop=" << position.exchange_stop_price
                  << " (order " << position.stop_order_id << ")"
                  << " Target=" << position.take_profit_price
                  << " UnrealizedPnL=" << std::setprecision(2)
                  << position.quantity * position.mark_price - position.entry_cost << '\n';
    }
    for (const auto& execution : executions) {
        std::cout << "  " << label << ' ' << execution.side << ' ' << execution.symbol
                  << " Qty=" << std::setprecision(8) << execution.quantity
                  << " Price=" << std::setprecision(4) << execution.price
                  << " Fee=" << execution.fee
                  << " RealizedPnL=" << execution.realized_pnl
                  << " Order=" << execution.order_id
                  << " Reason=[" << execution.reason << "]\n";
    }
}

int run_trader(const crypto_trader::common::AppConfig& config, const std::string& command) {
    const bool paper = command.rfind("--paper", 0) == 0;
    const std::string action = command.substr(paper ? 7 : 6);
    if (!paper && config.mode != "testnet") {
        throw std::invalid_argument(
            "Live commands require mode 'testnet' and live_order_submission_enabled: true "
            "(see config/testnet.json)");
    }

    // Paper and testnet run the same LiveTrader; only the venue differs.
    auto settings = config.live;
    std::unique_ptr<crypto_trader::exchange::BinanceSignedClient> client;
    std::unique_ptr<crypto_trader::execution::SpotVenue> market_data;
    std::unique_ptr<crypto_trader::execution::SpotVenue> venue;
    if (paper) {
        settings.state_path = config.paper_state_path;
        settings.capital_allocation = config.paper_initial_balance;
        settings.maximum_order_notional =
            std::min(settings.maximum_order_notional, config.paper_initial_balance);
        client = std::make_unique<crypto_trader::exchange::BinanceSignedClient>(
            config.rest_base_url, crypto_trader::exchange::ApiCredentials{},
            config.request_timeout_seconds, settings.receive_window_ms);
        market_data = std::make_unique<crypto_trader::execution::BinanceSpotVenue>(*client);
        auto venue_path = config.paper_state_path;
        venue_path.replace_extension(".venue.json");
        venue = std::make_unique<crypto_trader::execution::PaperVenue>(
            *market_data, venue_path, settings.quote_asset, config.paper_initial_balance,
            config.risk.taker_fee_rate, config.risk.slippage_rate);
    } else {
        client = std::make_unique<crypto_trader::exchange::BinanceSignedClient>(
            settings.order_base_url, crypto_trader::exchange::credentials_from_environment(),
            config.request_timeout_seconds, settings.receive_window_ms);
        venue = std::make_unique<crypto_trader::execution::BinanceSpotVenue>(*client);
    }
    const char* label = paper ? "Paper" : "Testnet";
    crypto_trader::risk::RiskManager risk_manager(config.risk);
    crypto_trader::live::LiveTrader trader(settings, config.risk, *venue);

    if (action == "-status") {
        print_live_account(label, trader.view(), {});
        return 0;
    }
    if (action == "-flatten") {
        const auto executions = trader.flatten("manual flatten");
        print_live_account(label, trader.view(), executions);
        std::cout << "All bot positions sold and trading halted. Use "
                  << (paper ? "--paper-reset" : "--live-reset") << " to resume.\n";
        return 0;
    }
    if (action == "-reset") {
        trader.reset_risk_shutdown(risk_manager);
        print_live_account(label, trader.view(), {});
        std::cout << "Trading re-enabled.\n";
        return 0;
    }

    // Signals always use real (mainnet) public market data.
    crypto_trader::exchange::BinanceExchange exchange(config);
    crypto_trader::market::MarketScanner scanner(exchange, config);
    const auto notifier = crypto_trader::common::Notifier::from_environment();
    const bool repeat = action.empty();
    bool was_halted = trader.view().trading_halted;
    std::string last_error;
    const auto on_scan = [&](const std::vector<crypto_trader::market::ScanResult>& results) {
        try {
            print_market_summary(results, repeat ? config.research.scan_interval_seconds : 0);
            const auto executions = trader.process_scan(results, risk_manager);
            const auto account = trader.view();
            print_live_account(label, account, executions);
            for (const auto& execution : executions) {
                std::ostringstream alert;
                alert << label << ' ' << execution.side << ' ' << execution.symbol
                      << " qty " << execution.quantity << " @ " << execution.price
                      << " (" << execution.reason << ")";
                if (execution.side == "SELL") {
                    alert << " P/L " << std::fixed << std::setprecision(2)
                          << execution.realized_pnl;
                }
                alert << std::fixed << std::setprecision(2) << " | equity " << account.equity;
                notifier.send(alert.str());
            }
            if (account.trading_halted && !was_halted) {
                notifier.send(std::string(label) + " bot HALTED by risk limits; equity " +
                              std::to_string(account.equity) + ". Review before resetting.");
            }
            was_halted = account.trading_halted;
            last_error.clear();
        } catch (const std::exception& error) {
            // Exchange-resident stops keep positions protected while the next
            // cycle retries; a transient network error must not stop the bot.
            spdlog::error("Trading cycle failed: {}", error.what());
            if (error.what() != last_error) {
                notifier.send(std::string(label) + " bot error: " + error.what());
                last_error = error.what();
            }
        }
    };
    if (repeat) {
        notifier.send(std::string(label) + " bot started: " +
                      std::to_string(config.trading_pairs.size()) + " markets, equity " +
                      std::to_string(trader.view().equity));
    }
    if (repeat) {
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        spdlog::info("{} trading {} markets. Press Ctrl+C to stop; stops stay active.",
                     label, config.trading_pairs.size());
        scanner.run(stop_requested, on_scan);
        return 0;
    }
    on_scan(scanner.scan_once());
    return 0;
}

void print_backtest_summary(const crypto_trader::backtest::BacktestResult& result) {
    std::cout << "Symbol=" << result.symbol
              << " Candles=" << result.candles_processed
              << " StartingEquity=" << std::fixed << std::setprecision(2)
              << result.initial_equity
              << " FinalEquity=" << result.final_equity
              << " StrategyReturn=" << std::setprecision(2)
              << result.total_return_fraction * 100.0 << '%'
              << " BuyAndHold=" << result.buy_and_hold_return_fraction * 100.0 << '%'
              << " MaxDrawdown=" << result.maximum_drawdown_fraction * 100.0 << '%'
              << " PerBarSharpe=" << result.per_bar_sharpe_ratio
              << " Trades=" << result.completed_trades
              << " Wins=" << result.winning_trades
              << " Losses=" << result.losing_trades
              << " ProfitFactor=";
    print_optional(result.profit_factor);
    std::cout << " RiskShutdown=" << (result.risk_shutdown ? "YES" : "NO") << '\n';
}

void print_backtest_footnote(const crypto_trader::common::AppConfig& config) {
    const double spread = config.backtest.spread_fraction.value_or(
        config.signals.maximum_spread_percent);
    std::cout << "Signals use completed bars; entries are at the next bar open. "
                 "If stop and target both touch within one bar, stop is assumed first. "
                 "Order-book history is unavailable; an assumed spread of "
              << std::setprecision(4) << spread * 100.0
              << "% is charged on every fill. "
                 "Results are historical simulation, not a profit forecast.\n";
}

int run_backtest(const crypto_trader::common::AppConfig& config,
                 const std::string& symbol) {
    crypto_trader::exchange::BinanceExchange exchange(config);
    const auto candles = exchange.fetch_candle_history(
        symbol, config.candle_interval, config.backtest.candle_count);
    const auto result = crypto_trader::backtest::Backtester(config).run(symbol, candles);

    std::cout << "\nHistorical Backtest (public candles; simulated fills only)\n";
    print_backtest_summary(result);
    for (const auto& trade : result.trades) {
        std::cout << trade.symbol << ' ' << trade.exit_reason
                  << " Qty=" << std::setprecision(8) << trade.quantity
                  << " Entry=" << std::setprecision(4) << trade.entry_price
                  << " Exit=" << trade.exit_price
                  << " Fees=" << trade.fees
                  << " NetPnL=" << trade.net_pnl << '\n';
    }
    print_backtest_footnote(config);
    return 0;
}

int run_backtest_all(const crypto_trader::common::AppConfig& config) {
    crypto_trader::exchange::BinanceExchange exchange(config);
    std::vector<std::vector<crypto_trader::market::Candle>> candles;
    std::vector<crypto_trader::backtest::MarketHistory> markets;
    candles.reserve(config.trading_pairs.size());
    for (const auto& symbol : config.trading_pairs) {
        candles.push_back(exchange.fetch_candle_history(
            symbol, config.candle_interval, config.backtest.candle_count));
    }
    for (std::size_t index = 0; index < candles.size(); ++index) {
        markets.push_back({config.trading_pairs[index], candles[index]});
    }
    const auto result = crypto_trader::backtest::Backtester(config).run_portfolio(markets);

    std::cout << "\nPortfolio Backtest: all pairs share one account (simulated fills only)\n"
              << std::fixed << std::setprecision(2)
              << "StartingEquity=" << result.initial_equity
              << " FinalEquity=" << result.final_equity
              << " Return=" << result.total_return_fraction * 100.0 << '%'
              << " EqualWeightBuyAndHold=" << result.buy_and_hold_return_fraction * 100.0 << '%'
              << "\nMaxDrawdown=" << result.maximum_drawdown_fraction * 100.0 << '%'
              << " WorstDay=" << result.worst_day_return_fraction * 100.0 << '%'
              << " MaxConcurrentPositions=" << result.maximum_concurrent_positions
              << " Trades=" << result.completed_trades
              << " Wins=" << result.winning_trades
              << " Losses=" << result.losing_trades
              << " ProfitFactor=";
    print_optional(result.profit_factor);
    std::cout << " RiskShutdown=" << (result.risk_shutdown ? "YES" : "NO") << '\n';
    std::cout << "Return by year:";
    for (const auto& year : result.yearly_returns) {
        std::cout << ' ' << year.year << '=' << year.return_fraction * 100.0 << '%';
    }
    std::cout << "\nNet P/L by pair:";
    for (const auto& symbol : config.trading_pairs) {
        double pnl = 0.0;
        for (const auto& trade : result.trades) {
            pnl += trade.symbol == symbol ? trade.net_pnl : 0.0;
        }
        std::cout << ' ' << symbol << '=' << pnl;
    }
    std::cout << '\n';
    print_backtest_footnote(config);
    return 0;
}

int run_ml_evaluation(const crypto_trader::common::AppConfig& config,
                      const std::string& symbol) {
    crypto_trader::exchange::BinanceExchange exchange(config);
    const auto candles = exchange.fetch_candles(
        symbol, config.candle_interval, config.candle_limit);
    const auto result = crypto_trader::ml::MlEvaluator(config).evaluate(candles);

    std::cout << "\nMachine-Learning Walk-Forward Holdout (advisory research only)\n"
              << "Symbol=" << symbol
              << " Candles=" << result.candles_processed
              << " Horizon=" << result.horizon_candles
              << " TrainSamples=" << result.training_samples
              << " ValidationSamples=" << result.validation_samples
              << " ValidationPositiveRate=" << std::fixed << std::setprecision(3)
              << result.validation_positive_rate
              << " Accuracy=" << result.validation_accuracy
              << " TrainingMajorityBaseline="
              << result.majority_class_baseline_accuracy
              << " Precision=" << result.precision
              << " Recall=" << result.recall
              << " LogLoss=" << result.log_loss
              << " BrierScore=" << result.brier_score
              << " ROC-AUC=";
    print_optional(result.roc_auc, 3);
    std::cout << '\n'
              << "Uses a five-candle forward label net of estimated costs, "
                 "past-only features, and a purged chronological holdout. "
                 "Metrics are not calibrated profitability estimates.\n";
    return 0;
}

crypto_trader::risk::OrderSide parse_order_side(const std::string& value) {
    if (value == "buy") {
        return crypto_trader::risk::OrderSide::buy;
    }
    if (value == "sell") {
        return crypto_trader::risk::OrderSide::sell;
    }
    throw std::invalid_argument("Order side must be 'buy' or 'sell'");
}

double parse_quantity(const std::string& value) {
    double quantity = 0.0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), quantity);
    if (error != std::errc{} || end != value.data() + value.size() ||
        !std::isfinite(quantity) || quantity <= 0.0) {
        throw std::invalid_argument("Order quantity must be a finite positive number");
    }
    return quantity;
}

int show_execution_status(const crypto_trader::common::AppConfig& config) {
    std::cout << "Execution safety status\n"
              << "Configured mode: " << config.mode << '\n'
              << "Dry-run interface: available (simulated preview only)\n"
              << "Live order submission: "
              << (config.live_order_submission_enabled ? "ENABLED" : "DISABLED") << '\n'
              << "Testnet order adapter: "
              << (config.mode == "testnet" ? config.live.order_base_url : "inactive (paper mode)")
              << '\n'
              << "Mainnet order submission: not enabled; configuration rejects it\n";
    return 0;
}

int run_execution_dry_run(const crypto_trader::common::AppConfig& config,
                          const std::string& symbol,
                          const std::string& side_text,
                          const std::string& quantity_text) {
    const auto side = parse_order_side(side_text);
    const double quantity = parse_quantity(quantity_text);
    crypto_trader::exchange::BinanceExchange exchange(config);
    const auto candles = exchange.fetch_candles(
        symbol, config.candle_interval, config.candle_limit);
    if (candles.empty()) {
        throw std::runtime_error("No market candle available for the dry-run preview");
    }

    const crypto_trader::execution::DryRunOrderExecutor executor;
    const auto result = executor.submit({
        .symbol = symbol,
        .side = side,
        .quantity = quantity,
        .reference_price = candles.back().close
    });
    std::cout << "DRY RUN ONLY — no order submitted\n"
              << "Status=" << crypto_trader::execution::to_string(result.status)
              << " Side=" << crypto_trader::execution::to_string(result.request.side)
              << " Symbol=" << result.request.symbol
              << " Quantity=" << std::setprecision(8) << result.request.quantity
              << " ReferencePrice=" << std::setprecision(4) << result.request.reference_price
              << " ReferenceNotional=" << result.reference_notional
              << "\n" << result.message << '\n';
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        print_usage();
        return 0;
    }
    if (argc < 2) {
        print_usage();
        return 2;
    }

    try {
        const std::string command = argv[1];
        const bool scanner_command = command == "--scan" || command == "--scan-once" ||
            command == "--paper" || command == "--paper-once" ||
            command == "--paper-status" || command == "--paper-reset" ||
            command == "--paper-flatten" ||
            command == "--execution-status" || command == "--backtest-all" ||
            command == "--live-once" || command == "--live" || command == "--live-status" ||
            command == "--live-flatten" || command == "--live-reset";
        const bool dry_run_command = command == "--execution-dry-run";
        if (scanner_command && (argc < 2 || argc > 3)) {
            print_usage();
            return 2;
        }
        if (dry_run_command && (argc < 5 || argc > 6)) {
            print_usage();
            return 2;
        }
        if (!scanner_command && !dry_run_command && (argc < 3 || argc > 4)) {
            print_usage();
            return 2;
        }
        const std::string config_path = scanner_command
            ? (argc == 3 ? argv[2] : "config/config.json")
            : dry_run_command
                ? (argc == 6 ? argv[5] : "config/config.json")
                : (argc == 4 ? argv[3] : "config/config.json");
        const auto config = crypto_trader::common::load_config(config_path);
        crypto_trader::common::initialize_logging(config.log_level, config.log_file);

        if (command == "--scan-once" || command == "--scan") {
            return scan_markets(config, command == "--scan");
        }
        if (command.rfind("--paper", 0) == 0) {
            return run_trader(config, command);
        }
        if (command == "--execution-status") {
            return show_execution_status(config);
        }
        if (command == "--backtest-all") {
            return run_backtest_all(config);
        }
        if (command.rfind("--live", 0) == 0) {
            return run_trader(config, command);
        }
        if (dry_run_command) {
            const std::string symbol = argv[2];
            require_configured_symbol(config, symbol);
            return run_execution_dry_run(config, symbol, argv[3], argv[4]);
        }

        const std::string symbol = argv[2];
        require_configured_symbol(config, symbol);

        if (command == "--backtest") {
            return run_backtest(config, symbol);
        }
        if (command == "--ml-evaluate") {
            return run_ml_evaluation(config, symbol);
        }
        if (command == "--candles") {
            return fetch_candles(config, symbol);
        }
        if (command == "--stream") {
            return stream_prices(config, symbol);
        }
        print_usage();
        return 2;
    } catch (const std::exception& error) {
        spdlog::critical("Application failed: {}", error.what());
        return 1;
    }
}
