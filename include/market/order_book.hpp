#pragma once

#include <cstdint>
#include <vector>

namespace crypto_trader::market {

struct PriceLevel {
    double price;
    double quantity;
};

struct OrderBook {
    std::int64_t timestamp_ms;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
};

}  // namespace crypto_trader::market
