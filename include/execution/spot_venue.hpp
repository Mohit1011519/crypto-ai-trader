#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "exchange/binance_signed_client.hpp"
#include "risk/risk_manager.hpp"

namespace crypto_trader::execution {

struct SymbolRules {
    std::string base_asset;
    std::string quote_asset;
    double step_size;
    double minimum_quantity;
    double tick_size;
    double minimum_notional;
    bool stop_market_supported;
};

struct BookTop {
    double bid;
    double ask;
};

struct AssetBalance {
    double free;
    double locked;
};

enum class OrderState {
    open,
    filled,
    cancelled,
    rejected,
    expired
};

struct VenueOrder {
    std::int64_t order_id;
    std::string client_order_id;
    OrderState state;
    double executed_quantity;
    double quote_quantity;
    double base_commission;
    double quote_commission;
};

// Authenticated spot trading operations. The live trader depends only on this
// interface so that its order-handling rules can be tested without a network.
// Order placement either returns the exchange's view of the order or throws:
// exchange::BinanceApiError when the order was definitely not placed, and
// exchange::BinanceUnknownOutcome when its existence could not be determined.
class SpotVenue {
public:
    virtual ~SpotVenue() = default;

    virtual SymbolRules symbol_rules(const std::string& symbol) = 0;
    virtual BookTop book_top(const std::string& symbol) = 0;
    virtual AssetBalance balance(const std::string& asset) = 0;
    virtual VenueOrder market_order(const std::string& symbol,
                                    risk::OrderSide side,
                                    double quantity,
                                    const std::string& client_order_id) = 0;
    // Exchange-resident stop-market sell. It stays active if the bot stops.
    virtual VenueOrder place_stop_market_sell(const std::string& symbol,
                                              double quantity,
                                              double stop_price,
                                              const std::string& client_order_id) = 0;
    virtual VenueOrder query_order(const std::string& symbol, std::int64_t order_id) = 0;
    // Returns the order's final state; a stop that filled before the cancel
    // arrived is reported as filled rather than as an error.
    virtual VenueOrder cancel_order(const std::string& symbol, std::int64_t order_id) = 0;
};

double floor_to_increment(double value, double increment);
std::string format_to_increment(double value, double increment);

class BinanceSpotVenue final : public SpotVenue {
public:
    explicit BinanceSpotVenue(exchange::BinanceSignedClient& client);

    SymbolRules symbol_rules(const std::string& symbol) override;
    BookTop book_top(const std::string& symbol) override;
    AssetBalance balance(const std::string& asset) override;
    VenueOrder market_order(const std::string& symbol,
                            risk::OrderSide side,
                            double quantity,
                            const std::string& client_order_id) override;
    VenueOrder place_stop_market_sell(const std::string& symbol,
                                      double quantity,
                                      double stop_price,
                                      const std::string& client_order_id) override;
    VenueOrder query_order(const std::string& symbol, std::int64_t order_id) override;
    VenueOrder cancel_order(const std::string& symbol, std::int64_t order_id) override;

private:
    VenueOrder place_order(const std::string& symbol,
                           exchange::QueryParameters parameters,
                           const std::string& client_order_id);
    VenueOrder with_commissions(const std::string& symbol, VenueOrder order);

    exchange::BinanceSignedClient& client_;
    std::map<std::string, SymbolRules> rules_;
};

const char* to_string(OrderState state) noexcept;

}  // namespace crypto_trader::execution
