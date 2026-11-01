#pragma once

#include <string>
#include <vector>

#include "common/config.hpp"
#include "exchange/exchange.hpp"

namespace crypto_trader::exchange {

class BinanceExchange final : public Exchange {
public:
    explicit BinanceExchange(common::AppConfig config);

    std::vector<market::Candle> fetch_candles(
        const std::string& symbol,
        const std::string& interval,
        int limit) const override;
    market::OrderBook fetch_order_book(
        const std::string& symbol,
        int depth) const override;

private:
    common::AppConfig config_;
};

}  // namespace crypto_trader::exchange
