#pragma once

#include <cstdint>

namespace crypto_trader::market {

struct Candle {
    std::int64_t open_time_ms;
    double open;
    double high;
    double low;
    double close;
    double volume;
    std::int64_t close_time_ms;
    double quote_volume;
};

}  // namespace crypto_trader::market
