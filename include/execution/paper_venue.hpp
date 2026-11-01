#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "execution/spot_venue.hpp"

namespace crypto_trader::execution {

// Simulated spot exchange for paper trading. Prices and symbol filters come
// from a real market-data venue (public Binance data); balances, market fills
// and resting stop-market orders are simulated locally and persisted. Paper
// mode therefore runs the exact LiveTrader order flow used on the testnet.
// Stops trigger when a later query or cancel observes bid <= stop and fill at
// the lower of the bid and the stop, less slippage.
class PaperVenue final : public SpotVenue {
public:
    PaperVenue(SpotVenue& market_data,
               std::filesystem::path state_path,
               std::string quote_asset,
               double initial_quote_balance,
               double taker_fee_rate,
               double slippage_rate);

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
    struct RestingStop {
        VenueOrder order;
        std::string symbol;
        std::string base_asset;
        double quantity;
        double stop_price;
    };

    void trigger_if_crossed(RestingStop& stop);
    void persist() const;

    SpotVenue& market_data_;
    std::filesystem::path state_path_;
    std::string quote_asset_;
    double taker_fee_rate_;
    double slippage_rate_;
    std::int64_t next_order_id_ = 1;
    std::map<std::string, AssetBalance> balances_;
    std::map<std::int64_t, RestingStop> orders_;
};

}  // namespace crypto_trader::execution
