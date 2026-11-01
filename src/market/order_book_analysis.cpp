#include "market/order_book_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace crypto_trader::market {
namespace {

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    if (values.size() % 2 != 0) {
        return *middle;
    }
    const auto lower = std::max_element(values.begin(), middle);
    return (*lower + *middle) / 2.0;
}

void validate_levels(const std::vector<PriceLevel>& levels,
                     const bool bids,
                     const char* side) {
    for (std::size_t index = 0; index < levels.size(); ++index) {
        const auto& level = levels[index];
        if (!std::isfinite(level.price) || !std::isfinite(level.quantity) ||
            level.price <= 0.0 || level.quantity <= 0.0) {
            throw std::invalid_argument("Order book contains invalid price levels");
        }
        if (index > 0) {
            const bool incorrectly_sorted = bids
                ? levels[index - 1].price < level.price
                : levels[index - 1].price > level.price;
            if (incorrectly_sorted) {
                throw std::invalid_argument(
                    std::string("Order book ") + side + " levels are not sorted by price");
            }
        }
    }
}

}  // namespace

OrderBookAnalysis analyze_order_book(const OrderBook& order_book,
                                     const common::ResearchSettings& settings) {
    validate_levels(order_book.bids, true, "bids");
    validate_levels(order_book.asks, false, "asks");

    if (order_book.bids.empty() || order_book.asks.empty()) {
        return {
            .metadata = {
                .timestamp_ms = order_book.timestamp_ms,
                .sample_count = order_book.bids.size() + order_book.asks.size(),
                .quality = 0.0
            },
            .best_bid = 0.0,
            .best_ask = 0.0,
            .spread = 0.0,
            .spread_percent = 0.0,
            .bid_quantity = 0.0,
            .ask_quantity = 0.0,
            .imbalance = 0.0,
            .quote_liquidity = 0.0,
            .large_bid_count = 0,
            .large_ask_count = 0,
            .liquidity = LiquidityLevel::unavailable
        };
    }

    const auto depth = static_cast<std::size_t>(settings.order_book_depth);
    const auto bids_count = std::min(depth, order_book.bids.size());
    const auto asks_count = std::min(depth, order_book.asks.size());
    double bid_quantity = 0.0;
    double ask_quantity = 0.0;
    double quote_liquidity = 0.0;
    std::vector<double> bid_sizes;
    std::vector<double> ask_sizes;
    bid_sizes.reserve(bids_count);
    ask_sizes.reserve(asks_count);

    for (std::size_t index = 0; index < bids_count; ++index) {
        const auto& level = order_book.bids[index];
        bid_quantity += level.quantity;
        quote_liquidity += level.price * level.quantity;
        bid_sizes.push_back(level.quantity);
    }
    for (std::size_t index = 0; index < asks_count; ++index) {
        const auto& level = order_book.asks[index];
        ask_quantity += level.quantity;
        quote_liquidity += level.price * level.quantity;
        ask_sizes.push_back(level.quantity);
    }

    const double total_quantity = bid_quantity + ask_quantity;
    const double imbalance = total_quantity > 0.0
        ? (bid_quantity - ask_quantity) / total_quantity
        : 0.0;
    const double best_bid = order_book.bids.front().price;
    const double best_ask = order_book.asks.front().price;
    if (best_bid >= best_ask) {
        throw std::invalid_argument("Order book best bid must be below best ask");
    }

    const double bid_median = median(bid_sizes);
    const double ask_median = median(ask_sizes);
    std::size_t large_bid_count = 0;
    std::size_t large_ask_count = 0;
    if (bid_median > 0.0) {
        large_bid_count = static_cast<std::size_t>(std::count_if(
            bid_sizes.begin(), bid_sizes.end(), [&](const double quantity) {
                return quantity >= bid_median * settings.large_order_multiple;
            }));
    }
    if (ask_median > 0.0) {
        large_ask_count = static_cast<std::size_t>(std::count_if(
            ask_sizes.begin(), ask_sizes.end(), [&](const double quantity) {
                return quantity >= ask_median * settings.large_order_multiple;
            }));
    }

    const auto levels = bids_count + asks_count;
    const auto requested_levels = depth * 2;
    const double quality = std::min(
        1.0, static_cast<double>(levels) / static_cast<double>(requested_levels));
    return {
        .metadata = {
            .timestamp_ms = order_book.timestamp_ms,
            .sample_count = levels,
            .quality = quality
        },
        .best_bid = best_bid,
        .best_ask = best_ask,
        .spread = best_ask - best_bid,
        .spread_percent = (best_ask - best_bid) / ((best_ask + best_bid) / 2.0),
        .bid_quantity = bid_quantity,
        .ask_quantity = ask_quantity,
        .imbalance = imbalance,
        .quote_liquidity = quote_liquidity,
        .large_bid_count = large_bid_count,
        .large_ask_count = large_ask_count,
        .liquidity = quote_liquidity >= settings.minimum_quote_liquidity
            ? LiquidityLevel::sufficient
            : LiquidityLevel::low
    };
}

}  // namespace crypto_trader::market
