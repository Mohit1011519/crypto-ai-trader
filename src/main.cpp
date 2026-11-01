#include "common/config.hpp"
#include "common/logger.hpp"
#include "exchange/binance_exchange.hpp"
#include "exchange/binance_market_stream.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

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
              << "  crypto-ai-trader --help\n";
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

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        print_usage();
        return 0;
    }
    if (argc < 3 || argc > 4) {
        print_usage();
        return 2;
    }

    try {
        const std::string command = argv[1];
        const std::string symbol = argv[2];
        const std::string config_path = argc == 4 ? argv[3] : "config/config.json";
        const auto config = crypto_trader::common::load_config(config_path);
        crypto_trader::common::initialize_logging(config.log_level);
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
