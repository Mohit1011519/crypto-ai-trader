#pragma once

#include "common/config.hpp"

namespace crypto_trader::risk {

// Protective-stop state for an open long position. The backtester and the
// paper portfolio both advance it through advance_protective_stop so that
// simulated and paper exits follow identical rules.
struct ProtectiveStopState {
    double entry_price;
    double initial_stop_price;
    double stop_price;
    double highest_close;
};

// Records a newly closed candle and returns the ratcheted stop. The stop never
// moves down. Break-even (entry plus estimated round-trip costs) applies once
// the highest close reaches entry + breakeven_trigger_r * initial risk; the
// chandelier trail is highest close - trailing_stop_atr_multiple * atr.
double advance_protective_stop(ProtectiveStopState& state,
                               double closed_price,
                               double atr,
                               const common::RiskSettings& settings);

}  // namespace crypto_trader::risk
