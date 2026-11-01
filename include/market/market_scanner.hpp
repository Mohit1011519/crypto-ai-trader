#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ai/signal_engine.hpp"
#include "common/config.hpp"
#include "exchange/exchange.hpp"
#include "indicators/technical_indicators.hpp"
#include "market/order_book_analysis.hpp"
#include "market/trend_analysis.hpp"
#include "market/volatility_analysis.hpp"
#include "market/volume_analysis.hpp"

namespace crypto_trader::market {

struct ScanResult {
    std::string symbol;
    std::optional<indicators::TechnicalIndicators> technical;
    std::optional<TrendAnalysis> trend;
    std::optional<VolumeAnalysis> volume;
    std::optional<VolatilityAnalysis> volatility;
    std::optional<OrderBookAnalysis> order_book;
    std::optional<ai::TradingSignal> signal;
    std::optional<std::string> candle_error;
    std::optional<std::string> order_book_error;
};

class MarketScanner {
public:
    using ScanHandler = std::function<void(const std::vector<ScanResult>&)>;

    MarketScanner(const exchange::Exchange& exchange, const common::AppConfig& config);

    std::vector<ScanResult> scan_once();
    void run(const std::atomic_bool& stop_requested, const ScanHandler& on_scan);

private:
    const exchange::Exchange& exchange_;
    const common::AppConfig& config_;
    ai::SignalEngine signal_engine_;
};

}  // namespace crypto_trader::market
