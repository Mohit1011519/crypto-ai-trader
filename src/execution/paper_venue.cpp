#include "execution/paper_venue.hpp"

#include <cmath>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

namespace crypto_trader::execution {
namespace {

// Same codes Binance uses, so LiveTrader treats simulated and real
// rejections identically.
constexpr int new_order_rejected = -2010;
constexpr int unknown_order = -2011;
constexpr double quantity_tolerance = 1e-9;

exchange::BinanceApiError rejected(const std::string& message) {
    return exchange::BinanceApiError(400, new_order_rejected, message);
}

}  // namespace

PaperVenue::PaperVenue(SpotVenue& market_data,
                       std::filesystem::path state_path,
                       std::string quote_asset,
                       const double initial_quote_balance,
                       const double taker_fee_rate,
                       const double slippage_rate)
    : market_data_(market_data),
      state_path_(std::move(state_path)),
      quote_asset_(std::move(quote_asset)),
      taker_fee_rate_(taker_fee_rate),
      slippage_rate_(slippage_rate) {
    if (state_path_.empty() || quote_asset_.empty() ||
        !std::isfinite(initial_quote_balance) || initial_quote_balance <= 0.0 ||
        !std::isfinite(taker_fee_rate_) || taker_fee_rate_ < 0.0 ||
        !std::isfinite(slippage_rate_) || slippage_rate_ < 0.0) {
        throw std::invalid_argument("Paper venue settings are invalid");
    }
    if (!std::filesystem::exists(state_path_)) {
        balances_[quote_asset_] = {initial_quote_balance, 0.0};
        persist();
        return;
    }
    std::ifstream input(state_path_);
    const auto state = nlohmann::json::parse(input);
    next_order_id_ = state.at("next_order_id").get<std::int64_t>();
    for (const auto& [asset, value] : state.at("balances").items()) {
        balances_[asset] = {value.at("free").get<double>(), value.at("locked").get<double>()};
    }
    for (const auto& entry : state.at("orders")) {
        const auto id = entry.at("order_id").get<std::int64_t>();
        orders_[id] = {
            .order = {
                .order_id = id,
                .client_order_id = entry.at("client_order_id").get<std::string>(),
                .state = static_cast<OrderState>(entry.at("state").get<int>()),
                .executed_quantity = entry.at("executed_quantity").get<double>(),
                .quote_quantity = entry.at("quote_quantity").get<double>(),
                .base_commission = 0.0,
                .quote_commission = entry.at("quote_commission").get<double>()
            },
            .symbol = entry.at("symbol").get<std::string>(),
            .base_asset = entry.at("base_asset").get<std::string>(),
            .quantity = entry.at("quantity").get<double>(),
            .stop_price = entry.at("stop_price").get<double>()
        };
    }
}

SymbolRules PaperVenue::symbol_rules(const std::string& symbol) {
    return market_data_.symbol_rules(symbol);
}

BookTop PaperVenue::book_top(const std::string& symbol) {
    return market_data_.book_top(symbol);
}

AssetBalance PaperVenue::balance(const std::string& asset) {
    const auto found = balances_.find(asset);
    return found == balances_.end() ? AssetBalance{0.0, 0.0} : found->second;
}

VenueOrder PaperVenue::market_order(const std::string& symbol,
                                    const risk::OrderSide side,
                                    const double quantity,
                                    const std::string& client_order_id) {
    const auto rules = symbol_rules(symbol);
    const auto top = book_top(symbol);
    if (!std::isfinite(quantity) || quantity < rules.minimum_quantity || quantity <= 0.0) {
        throw rejected("quantity below the exchange minimum");
    }
    auto& base = balances_[rules.base_asset];
    auto& quote = balances_[rules.quote_asset];
    VenueOrder order{next_order_id_++, client_order_id, OrderState::filled,
                     quantity, 0.0, 0.0, 0.0};
    if (side == risk::OrderSide::buy) {
        order.quote_quantity = quantity * top.ask * (1.0 + slippage_rate_);
        order.quote_commission = order.quote_quantity * taker_fee_rate_;
        if (order.quote_quantity + order.quote_commission > quote.free + quantity_tolerance) {
            throw rejected("insufficient " + rules.quote_asset + " balance");
        }
        quote.free -= order.quote_quantity + order.quote_commission;
        base.free += quantity;
    } else {
        if (quantity > base.free + quantity_tolerance) {
            throw rejected("insufficient " + rules.base_asset + " balance");
        }
        order.quote_quantity = quantity * top.bid * (1.0 - slippage_rate_);
        order.quote_commission = order.quote_quantity * taker_fee_rate_;
        base.free = std::max(0.0, base.free - quantity);
        quote.free += order.quote_quantity - order.quote_commission;
    }
    persist();
    return order;
}

VenueOrder PaperVenue::place_stop_market_sell(const std::string& symbol,
                                              const double quantity,
                                              const double stop_price,
                                              const std::string& client_order_id) {
    const auto rules = symbol_rules(symbol);
    const auto top = book_top(symbol);
    auto& base = balances_[rules.base_asset];
    if (!std::isfinite(stop_price) || stop_price <= 0.0 || stop_price >= top.bid) {
        throw rejected("stop price would trigger immediately");
    }
    if (quantity > base.free + quantity_tolerance) {
        throw rejected("insufficient " + rules.base_asset + " balance for stop");
    }
    const double locked = std::min(quantity, base.free);
    base.free -= locked;
    base.locked += locked;
    const auto id = next_order_id_++;
    orders_[id] = {
        .order = {id, client_order_id, OrderState::open, 0.0, 0.0, 0.0, 0.0},
        .symbol = symbol,
        .base_asset = rules.base_asset,
        .quantity = locked,
        .stop_price = stop_price
    };
    persist();
    return orders_[id].order;
}

void PaperVenue::trigger_if_crossed(RestingStop& stop) {
    if (stop.order.state != OrderState::open) {
        return;
    }
    const auto top = book_top(stop.symbol);
    if (top.bid > stop.stop_price) {
        return;
    }
    auto& base = balances_[stop.base_asset];
    auto& quote = balances_[quote_asset_];
    const double price = std::min(top.bid, stop.stop_price) * (1.0 - slippage_rate_);
    stop.order.state = OrderState::filled;
    stop.order.executed_quantity = stop.quantity;
    stop.order.quote_quantity = stop.quantity * price;
    stop.order.quote_commission = stop.order.quote_quantity * taker_fee_rate_;
    base.locked = std::max(0.0, base.locked - stop.quantity);
    quote.free += stop.order.quote_quantity - stop.order.quote_commission;
    persist();
}

VenueOrder PaperVenue::query_order(const std::string&, const std::int64_t order_id) {
    const auto found = orders_.find(order_id);
    if (found == orders_.end()) {
        throw exchange::BinanceApiError(400, -2013, "Order does not exist");
    }
    trigger_if_crossed(found->second);
    return found->second.order;
}

VenueOrder PaperVenue::cancel_order(const std::string&, const std::int64_t order_id) {
    const auto found = orders_.find(order_id);
    if (found == orders_.end()) {
        throw exchange::BinanceApiError(400, unknown_order, "Unknown order sent");
    }
    auto& stop = found->second;
    trigger_if_crossed(stop);
    if (stop.order.state == OrderState::open) {
        auto& base = balances_[stop.base_asset];
        base.locked = std::max(0.0, base.locked - stop.quantity);
        base.free += stop.quantity;
        stop.order.state = OrderState::cancelled;
        persist();
    }
    return stop.order;
}

void PaperVenue::persist() const {
    nlohmann::json balances = nlohmann::json::object();
    for (const auto& [asset, value] : balances_) {
        balances[asset] = {{"free", value.free}, {"locked", value.locked}};
    }
    // Closed stops are kept so a restarted trader can still look up a stop
    // that filled just before it stopped.
    nlohmann::json orders = nlohmann::json::array();
    for (const auto& [id, stop] : orders_) {
        orders.push_back({
            {"order_id", id},
            {"client_order_id", stop.order.client_order_id},
            {"state", static_cast<int>(stop.order.state)},
            {"executed_quantity", stop.order.executed_quantity},
            {"quote_quantity", stop.order.quote_quantity},
            {"quote_commission", stop.order.quote_commission},
            {"symbol", stop.symbol},
            {"base_asset", stop.base_asset},
            {"quantity", stop.quantity},
            {"stop_price", stop.stop_price}
        });
    }
    const nlohmann::json state{
        {"version", 1},
        {"next_order_id", next_order_id_},
        {"balances", balances},
        {"orders", orders}
    };
    if (!state_path_.parent_path().empty()) {
        std::filesystem::create_directories(state_path_.parent_path());
    }
    auto temporary_path = state_path_;
    temporary_path += ".tmp";
    {
        std::ofstream output(temporary_path, std::ios::trunc);
        output << state.dump(2) << '\n';
        if (!output) {
            throw std::runtime_error("Unable to write paper venue state");
        }
    }
    std::filesystem::rename(temporary_path, state_path_);
}

}  // namespace crypto_trader::execution
