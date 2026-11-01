#include "risk/exit_rules.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace crypto_trader::risk {

double advance_protective_stop(ProtectiveStopState& state,
                               const double closed_price,
                               const double atr,
                               const common::RiskSettings& settings) {
    if (!std::isfinite(state.entry_price) || state.entry_price <= 0.0 ||
        !std::isfinite(state.initial_stop_price) || state.initial_stop_price <= 0.0 ||
        state.initial_stop_price >= state.entry_price ||
        !std::isfinite(state.stop_price) || state.stop_price <= 0.0 ||
        !std::isfinite(state.highest_close) || state.highest_close <= 0.0) {
        throw std::invalid_argument("Protective stop state is invalid");
    }
    if (std::isfinite(closed_price) && closed_price > 0.0) {
        state.highest_close = std::max(state.highest_close, closed_price);
    }

    const double initial_risk = state.entry_price - state.initial_stop_price;
    if (settings.breakeven_trigger_r > 0.0 &&
        state.highest_close >= state.entry_price + settings.breakeven_trigger_r * initial_risk) {
        const double round_trip_cost_fraction =
            2.0 * (settings.taker_fee_rate + settings.slippage_rate);
        state.stop_price = std::max(
            state.stop_price, state.entry_price * (1.0 + round_trip_cost_fraction));
    }
    if (settings.trailing_stop_atr_multiple > 0.0 && std::isfinite(atr) && atr > 0.0) {
        state.stop_price = std::max(
            state.stop_price,
            state.highest_close - settings.trailing_stop_atr_multiple * atr);
    }
    return state.stop_price;
}

}  // namespace crypto_trader::risk
