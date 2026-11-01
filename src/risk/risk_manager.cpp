#include "risk/risk_manager.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace crypto_trader::risk {
namespace {

bool valid_settings(const common::RiskSettings& settings) {
    return std::isfinite(settings.risk_per_trade_fraction) &&
        settings.risk_per_trade_fraction > 0.0 &&
        settings.risk_per_trade_fraction <= 0.05 &&
        settings.maximum_open_positions > 0 &&
        std::isfinite(settings.daily_maximum_loss_fraction) &&
        settings.daily_maximum_loss_fraction > 0.0 &&
        settings.daily_maximum_loss_fraction <= 0.25 &&
        std::isfinite(settings.maximum_drawdown_fraction) &&
        settings.maximum_drawdown_fraction > 0.0 &&
        settings.maximum_drawdown_fraction <= 0.5 &&
        std::isfinite(settings.maximum_gross_exposure_fraction) &&
        settings.maximum_gross_exposure_fraction > 0.0 &&
        settings.maximum_gross_exposure_fraction <= 1.0 &&
        std::isfinite(settings.stop_atr_multiple) &&
        settings.stop_atr_multiple > 0.0 &&
        std::isfinite(settings.target_atr_multiple) &&
        settings.target_atr_multiple > 0.0 &&
        std::isfinite(settings.taker_fee_rate) &&
        settings.taker_fee_rate >= 0.0 && settings.taker_fee_rate < 1.0 &&
        std::isfinite(settings.slippage_rate) &&
        settings.slippage_rate >= 0.0 && settings.slippage_rate < 1.0;
}

void validate_snapshot(const PortfolioRiskSnapshot& snapshot) {
    if (!std::isfinite(snapshot.equity) || snapshot.equity <= 0.0 ||
        !std::isfinite(snapshot.available_balance) || snapshot.available_balance < 0.0 ||
        snapshot.available_balance > snapshot.equity ||
        !std::isfinite(snapshot.gross_exposure) || snapshot.gross_exposure < 0.0 ||
        !std::isfinite(snapshot.daily_starting_equity) ||
        snapshot.daily_starting_equity <= 0.0 ||
        !std::isfinite(snapshot.daily_net_pnl) ||
        !std::isfinite(snapshot.peak_equity) ||
        snapshot.peak_equity < snapshot.equity ||
        snapshot.open_positions < 0 ||
        snapshot.open_symbols.size() > static_cast<std::size_t>(snapshot.open_positions)) {
        throw std::invalid_argument("Portfolio risk snapshot contains invalid account values");
    }

    std::set<std::string> unique_symbols;
    for (const auto& symbol : snapshot.open_symbols) {
        if (symbol.empty() || !unique_symbols.insert(symbol).second) {
            throw std::invalid_argument("Portfolio risk snapshot contains an empty or duplicate symbol");
        }
    }
}

std::vector<std::string> current_limit_breaches(
    const PortfolioRiskSnapshot& snapshot,
    const common::RiskSettings& settings) {
    std::vector<std::string> reasons;
    const double daily_loss_limit =
        snapshot.daily_starting_equity * settings.daily_maximum_loss_fraction;
    const double daily_loss = std::max(0.0, -snapshot.daily_net_pnl);
    const double drawdown = (snapshot.peak_equity - snapshot.equity) / snapshot.peak_equity;

    if (daily_loss >= daily_loss_limit) {
        reasons.push_back("Daily maximum loss reached; trading is shut down");
    }
    if (drawdown >= settings.maximum_drawdown_fraction) {
        reasons.push_back("Maximum account drawdown reached; trading is shut down");
    }
    return reasons;
}

}  // namespace

RiskManager::RiskManager(common::RiskSettings settings) : settings_(std::move(settings)) {
    if (!valid_settings(settings_)) {
        throw std::invalid_argument("Risk manager settings are invalid");
    }
}

RiskStatus RiskManager::monitor(const PortfolioRiskSnapshot& snapshot) {
    validate_snapshot(snapshot);
    const double daily_loss_fraction = std::max(0.0, -snapshot.daily_net_pnl) /
        snapshot.daily_starting_equity;
    const double drawdown_fraction =
        (snapshot.peak_equity - snapshot.equity) / snapshot.peak_equity;
    if (!std::isfinite(daily_loss_fraction) || !std::isfinite(drawdown_fraction)) {
        throw std::invalid_argument("Portfolio risk snapshot produces non-finite risk metrics");
    }
    auto breaches = current_limit_breaches(snapshot, settings_);

    std::lock_guard lock(mutex_);
    const bool daily_breach =
        daily_loss_fraction >= settings_.daily_maximum_loss_fraction;
    const bool drawdown_breach = drawdown_fraction >= settings_.maximum_drawdown_fraction;
    if (!snapshot.utc_day.empty() && daily_breach && !drawdown_breach) {
        // Daily loss: stop opening positions for the rest of this UTC day.
        paused_day_ = snapshot.utc_day;
    } else if (!breaches.empty()) {
        emergency_shutdown_ = true;
        shutdown_reasons_ = std::move(breaches);
    }
    const bool paused = !snapshot.utc_day.empty() && paused_day_ == snapshot.utc_day;
    auto reasons = shutdown_reasons_;
    if (paused && !emergency_shutdown_) {
        reasons.push_back("Daily maximum loss reached; new entries paused until the next UTC day");
    }
    return {
        .emergency_shutdown = emergency_shutdown_,
        .daily_loss_fraction = daily_loss_fraction,
        .drawdown_fraction = drawdown_fraction,
        .reasons = std::move(reasons),
        .daily_pause = paused
    };
}

RiskDecision RiskManager::assess(const OrderIntent& order,
                                 const PortfolioRiskSnapshot& snapshot) {
    validate_snapshot(snapshot);
    const auto status = monitor(snapshot);
    RiskDecision decision{
        .approved = false,
        .emergency_shutdown = status.emergency_shutdown,
        .plan = std::nullopt,
        .reasons = status.reasons
    };
    if (decision.emergency_shutdown || status.daily_pause) {
        return decision;
    }

    const auto reject = [&](std::string reason) {
        decision.reasons.push_back(std::move(reason));
        return decision;
    };
    if (order.symbol.empty()) {
        return reject("Order symbol must not be empty");
    }
    if (order.side != OrderSide::buy) {
        return reject("Short positions and sell-to-close execution are unavailable in this phase");
    }
    if (!std::isfinite(order.entry_price) || order.entry_price <= 0.0 ||
        !std::isfinite(order.atr) || order.atr <= 0.0 ||
        !std::isfinite(order.spread_fraction) || order.spread_fraction < 0.0 ||
        order.spread_fraction >= 1.0) {
        return reject("Order entry price, ATR, or spread is invalid");
    }
    if (snapshot.open_positions >= settings_.maximum_open_positions) {
        return reject("Maximum open position count reached");
    }
    if (std::find(snapshot.open_symbols.begin(), snapshot.open_symbols.end(), order.symbol) !=
        snapshot.open_symbols.end()) {
        return reject("A position already exists for this symbol");
    }

    const double maximum_exposure =
        snapshot.equity * settings_.maximum_gross_exposure_fraction;
    const double exposure_capacity = maximum_exposure - snapshot.gross_exposure;
    if (exposure_capacity <= 0.0) {
        return reject("Maximum gross exposure reached");
    }

    const double stop_distance = order.atr * settings_.stop_atr_multiple;
    const double target_distance = order.atr * settings_.target_atr_multiple;
    if (stop_distance >= order.entry_price) {
        return reject("Protective stop would be at or below zero");
    }

    const double round_trip_cost_fraction =
        2.0 * (settings_.taker_fee_rate + settings_.slippage_rate) +
        order.spread_fraction;
    const double estimated_cost_per_unit =
        order.entry_price * round_trip_cost_fraction;
    const double net_reward_per_unit = target_distance - estimated_cost_per_unit;
    const double planned_loss_per_unit = stop_distance + estimated_cost_per_unit;
    if (!std::isfinite(net_reward_per_unit) ||
        !std::isfinite(planned_loss_per_unit) ||
        net_reward_per_unit <= 0.0 || planned_loss_per_unit <= 0.0) {
        return reject("Estimated fees, slippage, and spread consume the planned reward");
    }
    const double net_reward_risk_ratio = net_reward_per_unit / planned_loss_per_unit;
    if (!std::isfinite(net_reward_risk_ratio) || net_reward_risk_ratio < 1.0) {
        return reject("Net reward/risk after estimated costs is below 1:1");
    }

    const double risk_budget = snapshot.equity * settings_.risk_per_trade_fraction;
    const double quantity_by_risk = risk_budget / planned_loss_per_unit;
    const double quantity_by_exposure = exposure_capacity / order.entry_price;
    const double entry_cost_fraction =
        settings_.taker_fee_rate + settings_.slippage_rate;
    const double quantity_by_balance = snapshot.available_balance /
        (order.entry_price * (1.0 + entry_cost_fraction));
    const double quantity = std::min({
        quantity_by_risk, quantity_by_exposure, quantity_by_balance
    });
    if (!std::isfinite(quantity) || quantity <= 0.0) {
        return reject("Insufficient balance or exposure capacity for a positive position size");
    }

    const double notional = quantity * order.entry_price;
    const double maximum_planned_loss = quantity * planned_loss_per_unit;
    const double round_trip_cost = quantity * estimated_cost_per_unit;
    const double gross_exposure_after = snapshot.gross_exposure + notional;
    if (!std::isfinite(notional) || !std::isfinite(maximum_planned_loss) ||
        !std::isfinite(round_trip_cost) || !std::isfinite(gross_exposure_after) ||
        maximum_planned_loss > risk_budget * (1.0 + 1e-12)) {
        return reject("Calculated position exceeds the per-trade risk budget");
    }

    decision.approved = true;
    decision.plan = RiskPlan{
        .position_quantity = quantity,
        .position_notional = notional,
        .risk_budget = risk_budget,
        .maximum_planned_loss = maximum_planned_loss,
        .stop_loss_price = order.entry_price - stop_distance,
        .take_profit_price = order.entry_price + target_distance,
        .estimated_round_trip_cost = round_trip_cost,
        .net_reward_risk_ratio = net_reward_risk_ratio,
        .gross_exposure_after = gross_exposure_after
    };
    decision.reasons.push_back(
        "Approved buy-only spot risk plan; protective stop, target, fees, slippage, and spread included");
    return decision;
}

void RiskManager::reset_emergency_shutdown(const PortfolioRiskSnapshot& snapshot) {
    validate_snapshot(snapshot);
    const auto breaches = current_limit_breaches(snapshot, settings_);
    if (!breaches.empty()) {
        throw std::logic_error("Cannot reset risk shutdown while a configured limit is still breached");
    }

    std::lock_guard lock(mutex_);
    emergency_shutdown_ = false;
    shutdown_reasons_.clear();
}

}  // namespace crypto_trader::risk
