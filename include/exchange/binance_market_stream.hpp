#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "common/config.hpp"

namespace crypto_trader::exchange {

struct PriceUpdate {
    std::string symbol;
    double price;
    long long event_time_ms;
};

class BinanceMarketStream {
public:
    using PriceHandler = std::function<void(const PriceUpdate&)>;

    explicit BinanceMarketStream(common::AppConfig config);

    void run(const std::string& symbol,
             const PriceHandler& on_price,
             const std::atomic_bool& stop_requested) const;

private:
    common::AppConfig config_;
};

}  // namespace crypto_trader::exchange
