#pragma once

#include <string>
#include <vector>

#include "market/candle.hpp"

namespace crypto_trader::exchange {

class Exchange {
public:
    virtual ~Exchange() = default;

    virtual std::vector<market::Candle> fetch_candles(
        const std::string& symbol,
        const std::string& interval,
        int limit) const = 0;
};

}  // namespace crypto_trader::exchange
