#include "market/market_scanner.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

namespace crypto_trader::market {

MarketScanner::MarketScanner(const exchange::Exchange& exchange,
                             const common::AppConfig& config)
    : exchange_(exchange), config_(config), signal_engine_(config.signals) {}

std::vector<ScanResult> MarketScanner::scan_once() {
    std::vector<ScanResult> results;
    results.reserve(config_.trading_pairs.size());

    for (const auto& symbol : config_.trading_pairs) {
        ScanResult result{.symbol = symbol};
        try {
            const auto candles = exchange_.fetch_candles(
                symbol, config_.candle_interval, config_.candle_limit);
            result.technical = indicators::calculate(candles, config_.research);
            result.trend = analyze_trend(candles, *result.technical, config_.research);
            result.volume = analyze_volume(candles, config_.research);
            result.volatility = analyze_volatility(
                candles, result.technical->atr, config_.research);
        } catch (const std::exception& error) {
            result.candle_error = error.what();
            spdlog::error("Market research failed for {}: {}", symbol, error.what());
            result.signal = signal_engine_.evaluate(result);
            results.push_back(std::move(result));
            continue;
        }

        try {
            const auto depth = exchange_.fetch_order_book(
                symbol, config_.research.order_book_depth);
            result.order_book = analyze_order_book(depth, config_.research);
        } catch (const std::exception& error) {
            result.order_book_error = error.what();
            spdlog::error("Order book research failed for {}: {}", symbol, error.what());
        }
        result.signal = signal_engine_.evaluate(result);
        results.push_back(std::move(result));
    }

    return results;
}

void MarketScanner::run(const std::atomic_bool& stop_requested,
                        const ScanHandler& on_scan) {
    if (!on_scan) {
        throw std::invalid_argument("Market scanner callback must be set");
    }

    while (!stop_requested.load()) {
        on_scan(scan_once());
        const auto wait_until = std::chrono::steady_clock::now() +
            std::chrono::seconds(config_.research.scan_interval_seconds);
        while (!stop_requested.load() && std::chrono::steady_clock::now() < wait_until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}  // namespace crypto_trader::market
