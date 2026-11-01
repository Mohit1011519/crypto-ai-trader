#pragma once

#include "common/config.hpp"
#include "market/order_book.hpp"
#include "market/research.hpp"

namespace crypto_trader::market {

enum class LiquidityLevel {
    sufficient,
    low,
    unavailable
};

struct OrderBookAnalysis {
    ResearchMetadata metadata;
    double best_bid;
    double best_ask;
    double spread;
    double spread_percent;
    double bid_quantity;
    double ask_quantity;
    double imbalance;
    double quote_liquidity;
    std::size_t large_bid_count;
    std::size_t large_ask_count;
    LiquidityLevel liquidity;
};

OrderBookAnalysis analyze_order_book(const OrderBook& order_book,
                                     const common::ResearchSettings& settings);

}  // namespace crypto_trader::market
