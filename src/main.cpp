#include "common/config.hpp"
#include "common/logger.hpp"
#include "exchange/binance_exchange.hpp"
#include "exchange/binance_market_stream.hpp"
#include "market/market_scanner.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <optional>
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

void print_scan(const std::vector<crypto_trader::market::ScanResult>& results) {
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
    std::cout << "Signals are advisory only; positions are not tracked and no orders are placed.\n";
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
        scanner.run(stop_requested, print_scan);
        return 0;
    }
    print_scan(scanner.scan_once());
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
        const bool scanner_command = command == "--scan" || command == "--scan-once";
        if (scanner_command && (argc < 2 || argc > 3)) {
            print_usage();
            return 2;
        }
        if (!scanner_command && (argc < 3 || argc > 4)) {
            print_usage();
            return 2;
        }
        const std::string config_path = scanner_command
            ? (argc == 3 ? argv[2] : "config/config.json")
            : (argc == 4 ? argv[3] : "config/config.json");
        const auto config = crypto_trader::common::load_config(config_path);
        crypto_trader::common::initialize_logging(config.log_level);

        if (command == "--scan-once" || command == "--scan") {
            return scan_markets(config, command == "--scan");
        }

        const std::string symbol = argv[2];
        require_configured_symbol(config, symbol);

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
