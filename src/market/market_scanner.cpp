#include "market/market_scanner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

namespace crypto_trader::market {
namespace {

// Candle length for Binance interval strings such as "1m", "4h", "1d".
std::int64_t interval_milliseconds(const std::string& interval) {
    if (interval.size() < 2) {
        return 0;
    }
    const auto count = std::stoll(interval.substr(0, interval.size() - 1));
    switch (interval.back()) {
    case 'm': return count * 60'000;
    case 'h': return count * 3'600'000;
    case 'd': return count * 86'400'000;
    case 'w': return count * 604'800'000;
    default: return 0;
    }
}

}  // namespace

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

    const auto candle_ms = interval_milliseconds(config_.candle_interval);
    while (!stop_requested.load()) {
        on_scan(scan_once());
        // Wake at the regular interval, or a few seconds after the current
        // candle closes if that is sooner, so new signals are acted on at once.
        auto delay = std::chrono::milliseconds(
            static_cast<std::int64_t>(config_.research.scan_interval_seconds) * 1000);
        if (candle_ms > 0) {
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto until_close = std::chrono::milliseconds(
                (now_ms / candle_ms + 1) * candle_ms - now_ms + 3000);
            delay = std::min(delay, until_close);
        }
        const auto wait_until = std::chrono::steady_clock::now() + delay;
        while (!stop_requested.load() && std::chrono::steady_clock::now() < wait_until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}  // namespace crypto_trader::market
