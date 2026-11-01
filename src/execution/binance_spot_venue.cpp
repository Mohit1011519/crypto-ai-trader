#include "execution/spot_venue.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <spdlog/spdlog.h>

namespace crypto_trader::execution {
namespace {

constexpr int order_does_not_exist = -2013;
constexpr int unknown_order_on_cancel = -2011;

double number(const nlohmann::json& value) {
    const double parsed = value.is_string() ? std::stod(value.get<std::string>())
                                            : value.get<double>();
    if (!std::isfinite(parsed)) {
        throw std::runtime_error("Binance returned a non-finite number");
    }
    return parsed;
}

OrderState parse_state(const std::string& status) {
    if (status == "NEW" || status == "PARTIALLY_FILLED" || status == "PENDING_NEW") {
        return OrderState::open;
    }
    if (status == "FILLED") {
        return OrderState::filled;
    }
    if (status == "CANCELED" || status == "PENDING_CANCEL") {
        return OrderState::cancelled;
    }
    if (status == "REJECTED") {
        return OrderState::rejected;
    }
    if (status == "EXPIRED" || status == "EXPIRED_IN_MATCH") {
        return OrderState::expired;
    }
    throw std::runtime_error("Unrecognized Binance order status: " + status);
}

VenueOrder parse_order(const nlohmann::json& body) {
    return {
        .order_id = body.at("orderId").get<std::int64_t>(),
        .client_order_id = body.value("clientOrderId", body.value("origClientOrderId", "")),
        .state = parse_state(body.at("status").get<std::string>()),
        .executed_quantity = number(body.at("executedQty")),
        .quote_quantity = number(body.at("cummulativeQuoteQty")),
        .base_commission = 0.0,
        .quote_commission = 0.0
    };
}

std::string side_name(const risk::OrderSide side) {
    return side == risk::OrderSide::buy ? "BUY" : "SELL";
}

}  // namespace

double floor_to_increment(const double value, const double increment) {
    if (!std::isfinite(value) || !std::isfinite(increment) || increment <= 0.0) {
        throw std::invalid_argument("Cannot round to a non-positive increment");
    }
    // The epsilon keeps exact multiples (e.g. 0.3 / 0.1) from rounding down.
    return std::floor(value / increment + 1e-9) * increment;
}

std::string format_to_increment(const double value, const double increment) {
    int decimals = 0;
    for (double scaled = increment; decimals < 12 &&
         std::abs(scaled - std::round(scaled)) > 1e-9; scaled *= 10.0) {
        ++decimals;
    }
    std::ostringstream output;
    output << std::fixed << std::setprecision(decimals) << floor_to_increment(value, increment);
    return output.str();
}

BinanceSpotVenue::BinanceSpotVenue(exchange::BinanceSignedClient& client) : client_(client) {}

SymbolRules BinanceSpotVenue::symbol_rules(const std::string& symbol) {
    if (const auto cached = rules_.find(symbol); cached != rules_.end()) {
        return cached->second;
    }
    const auto info = client_.public_get("/api/v3/exchangeInfo", {{"symbol", symbol}});
    const auto& entry = info.at("symbols").at(0);
    if (entry.at("status").get<std::string>() != "TRADING") {
        throw std::runtime_error(symbol + " is not currently trading");
    }
    SymbolRules rules{
        .base_asset = entry.at("baseAsset").get<std::string>(),
        .quote_asset = entry.at("quoteAsset").get<std::string>(),
        .step_size = 0.0,
        .minimum_quantity = 0.0,
        .tick_size = 0.0,
        .minimum_notional = 0.0,
        .stop_market_supported = false
    };
    for (const auto& type : entry.at("orderTypes")) {
        rules.stop_market_supported = rules.stop_market_supported || type == "STOP_LOSS";
    }
    for (const auto& filter : entry.at("filters")) {
        const auto type = filter.at("filterType").get<std::string>();
        if (type == "LOT_SIZE") {
            rules.step_size = number(filter.at("stepSize"));
            rules.minimum_quantity = number(filter.at("minQty"));
        } else if (type == "PRICE_FILTER") {
            rules.tick_size = number(filter.at("tickSize"));
        } else if (type == "NOTIONAL" || type == "MIN_NOTIONAL") {
            rules.minimum_notional = number(filter.at("minNotional"));
        }
    }
    if (rules.step_size <= 0.0 || rules.tick_size <= 0.0) {
        throw std::runtime_error("Binance symbol filters are incomplete for " + symbol);
    }
    rules_.emplace(symbol, rules);
    return rules;
}

BookTop BinanceSpotVenue::book_top(const std::string& symbol) {
    const auto body = client_.public_get("/api/v3/ticker/bookTicker", {{"symbol", symbol}});
    const BookTop top{number(body.at("bidPrice")), number(body.at("askPrice"))};
    if (top.bid <= 0.0 || top.ask < top.bid) {
        throw std::runtime_error("Binance returned an invalid book ticker for " + symbol);
    }
    return top;
}

AssetBalance BinanceSpotVenue::balance(const std::string& asset) {
    const auto account = client_.signed_request(
        "GET", "/api/v3/account", {{"omitZeroBalances", "true"}});
    for (const auto& entry : account.at("balances")) {
        if (entry.at("asset").get<std::string>() == asset) {
            return {number(entry.at("free")), number(entry.at("locked"))};
        }
    }
    return {0.0, 0.0};
}

VenueOrder BinanceSpotVenue::place_order(const std::string& symbol,
                                         exchange::QueryParameters parameters,
                                         const std::string& client_order_id) {
    parameters.emplace_back("newClientOrderId", client_order_id);
    try {
        return parse_order(client_.signed_request("POST", "/api/v3/order", parameters));
    } catch (const exchange::BinanceUnknownOutcome& error) {
        spdlog::warn("Order {} outcome unknown ({}); reconciling by client order id",
                     client_order_id, error.what());
        for (int attempt = 0; attempt < 5; ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(1 + attempt));
            try {
                return parse_order(client_.signed_request(
                    "GET", "/api/v3/order",
                    {{"symbol", symbol}, {"origClientOrderId", client_order_id}}));
            } catch (const exchange::BinanceApiError& lookup) {
                if (lookup.code() == order_does_not_exist) {
                    throw exchange::BinanceApiError(
                        lookup.http_status(), lookup.code(),
                        "order " + client_order_id + " was not placed");
                }
                throw;
            } catch (const exchange::BinanceUnknownOutcome&) {
                // Keep retrying the lookup.
            }
        }
        throw;
    }
}

VenueOrder BinanceSpotVenue::with_commissions(const std::string& symbol, VenueOrder order) {
    if (order.executed_quantity <= 0.0) {
        return order;
    }
    const auto rules = symbol_rules(symbol);
    const auto trades = client_.signed_request(
        "GET", "/api/v3/myTrades",
        {{"symbol", symbol}, {"orderId", std::to_string(order.order_id)}});
    for (const auto& trade : trades) {
        const auto asset = trade.at("commissionAsset").get<std::string>();
        const double commission = number(trade.at("commission"));
        if (asset == rules.base_asset) {
            order.base_commission += commission;
        } else if (asset == rules.quote_asset) {
            order.quote_commission += commission;
        }
    }
    return order;
}

VenueOrder BinanceSpotVenue::market_order(const std::string& symbol,
                                          const risk::OrderSide side,
                                          const double quantity,
                                          const std::string& client_order_id) {
    const auto rules = symbol_rules(symbol);
    auto order = place_order(symbol, {
        {"symbol", symbol},
        {"side", side_name(side)},
        {"type", "MARKET"},
        {"quantity", format_to_increment(quantity, rules.step_size)},
        {"newOrderRespType", "RESULT"}
    }, client_order_id);
    return with_commissions(symbol, order);
}

VenueOrder BinanceSpotVenue::place_stop_market_sell(const std::string& symbol,
                                                    const double quantity,
                                                    const double stop_price,
                                                    const std::string& client_order_id) {
    const auto rules = symbol_rules(symbol);
    if (!rules.stop_market_supported) {
        throw std::runtime_error(symbol + " does not support exchange STOP_LOSS orders");
    }
    return place_order(symbol, {
        {"symbol", symbol},
        {"side", "SELL"},
        {"type", "STOP_LOSS"},
        {"quantity", format_to_increment(quantity, rules.step_size)},
        {"stopPrice", format_to_increment(stop_price, rules.tick_size)},
        {"newOrderRespType", "RESULT"}
    }, client_order_id);
}

VenueOrder BinanceSpotVenue::query_order(const std::string& symbol,
                                         const std::int64_t order_id) {
    auto order = parse_order(client_.signed_request(
        "GET", "/api/v3/order", {{"symbol", symbol}, {"orderId", std::to_string(order_id)}}));
    return order.state == OrderState::open ? order : with_commissions(symbol, order);
}

VenueOrder BinanceSpotVenue::cancel_order(const std::string& symbol,
                                          const std::int64_t order_id) {
    try {
        auto order = parse_order(client_.signed_request(
            "DELETE", "/api/v3/order",
            {{"symbol", symbol}, {"orderId", std::to_string(order_id)}}));
        return with_commissions(symbol, order);
    } catch (const exchange::BinanceApiError& error) {
        if (error.code() != unknown_order_on_cancel) {
            throw;
        }
        // Already filled or cancelled: report its final state.
        return query_order(symbol, order_id);
    }
}

const char* to_string(const OrderState state) noexcept {
    switch (state) {
    case OrderState::open: return "OPEN";
    case OrderState::filled: return "FILLED";
    case OrderState::cancelled: return "CANCELLED";
    case OrderState::rejected: return "REJECTED";
    case OrderState::expired: return "EXPIRED";
    }
    return "UNKNOWN";
}

}  // namespace crypto_trader::execution
