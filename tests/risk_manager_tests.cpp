#include "risk/risk_manager.hpp"

#include <limits>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

crypto_trader::common::RiskSettings settings() {
    return {
        .risk_per_trade_fraction = 0.005,
        .maximum_open_positions = 3,
        .daily_maximum_loss_fraction = 0.02,
        .maximum_drawdown_fraction = 0.10,
        .maximum_gross_exposure_fraction = 1.0,
        .stop_atr_multiple = 1.5,
        .target_atr_multiple = 3.0,
        .taker_fee_rate = 0.001,
        .slippage_rate = 0.0005
    };
}

crypto_trader::risk::PortfolioRiskSnapshot snapshot() {
    return {
        .equity = 1000.0,
        .available_balance = 1000.0,
        .gross_exposure = 0.0,
        .daily_starting_equity = 1000.0,
        .daily_net_pnl = 0.0,
        .peak_equity = 1000.0,
        .open_positions = 0,
        .open_symbols = {}
    };
}

crypto_trader::risk::OrderIntent buy_order() {
    return {
        .symbol = "BTCUSDT",
        .side = crypto_trader::risk::OrderSide::buy,
        .entry_price = 100.0,
        .atr = 2.0,
        .spread_fraction = 0.001
    };
}

TEST(RiskManager, SizesPositionWithinRiskBudgetAndSetsProtectiveExits) {
    crypto_trader::risk::RiskManager manager(settings());
    const auto decision = manager.assess(buy_order(), snapshot());

    ASSERT_TRUE(decision.approved);
    ASSERT_TRUE(decision.plan.has_value());
    const auto& plan = *decision.plan;
    EXPECT_NEAR(plan.risk_budget, 5.0, 1e-12);
    EXPECT_LE(plan.maximum_planned_loss, plan.risk_budget);
    EXPECT_GT(plan.position_quantity, 0.0);
    EXPECT_NEAR(plan.stop_loss_price, 97.0, 1e-12);
    EXPECT_NEAR(plan.take_profit_price, 106.0, 1e-12);
    EXPECT_NEAR(plan.position_notional, plan.position_quantity * 100.0, 1e-12);
    EXPECT_GT(plan.estimated_round_trip_cost, 0.0);
    EXPECT_GE(plan.net_reward_risk_ratio, 1.0);
    EXPECT_NEAR(plan.gross_exposure_after, plan.position_notional, 1e-12);
}

TEST(RiskManager, CapsSizeByAvailableBalanceAndPortfolioExposure) {
    crypto_trader::risk::RiskManager manager(settings());
    auto low_balance = snapshot();
    low_balance.available_balance = 10.0;
    const auto balance_decision = manager.assess(buy_order(), low_balance);
    ASSERT_TRUE(balance_decision.approved);
    EXPECT_LE(balance_decision.plan->position_notional, 10.0);

    auto little_exposure_room = snapshot();
    little_exposure_room.gross_exposure = 995.0;
    const auto exposure_decision = manager.assess(buy_order(), little_exposure_room);
    ASSERT_TRUE(exposure_decision.approved);
    EXPECT_LE(exposure_decision.plan->gross_exposure_after, 1000.0);
    EXPECT_NEAR(exposure_decision.plan->position_notional, 5.0, 1e-12);
}

TEST(RiskManager, RejectsUnsupportedOrOverLimitOrders) {
    crypto_trader::risk::RiskManager manager(settings());
    const auto portfolio = snapshot();

    auto short_order = buy_order();
    short_order.side = crypto_trader::risk::OrderSide::sell;
    EXPECT_FALSE(manager.assess(short_order, portfolio).approved);

    auto no_atr = buy_order();
    no_atr.atr = 0.0;
    EXPECT_FALSE(manager.assess(no_atr, portfolio).approved);

    auto too_many_positions = portfolio;
    too_many_positions.open_positions = 3;
    EXPECT_FALSE(manager.assess(buy_order(), too_many_positions).approved);

    auto duplicate_position = portfolio;
    duplicate_position.open_positions = 1;
    duplicate_position.open_symbols = {"BTCUSDT"};
    EXPECT_FALSE(manager.assess(buy_order(), duplicate_position).approved);

    auto no_cash = portfolio;
    no_cash.available_balance = 0.0;
    EXPECT_FALSE(manager.assess(buy_order(), no_cash).approved);

    auto too_wide_spread = buy_order();
    too_wide_spread.spread_fraction = 0.2;
    EXPECT_FALSE(manager.assess(too_wide_spread, portfolio).approved);
}

TEST(RiskManager, LatchesDailyLossAndRequiresExplicitSafeReset) {
    crypto_trader::risk::RiskManager manager(settings());
    auto breached = snapshot();
    breached.daily_net_pnl = -20.0;

    const auto status = manager.monitor(breached);
    EXPECT_TRUE(status.emergency_shutdown);
    EXPECT_NEAR(status.daily_loss_fraction, 0.02, 1e-12);
    EXPECT_FALSE(manager.assess(buy_order(), snapshot()).approved);
    EXPECT_TRUE(manager.assess(buy_order(), snapshot()).emergency_shutdown);

    EXPECT_THROW(manager.reset_emergency_shutdown(breached), std::logic_error);
    manager.reset_emergency_shutdown(snapshot());
    EXPECT_TRUE(manager.assess(buy_order(), snapshot()).approved);
}

TEST(RiskManager, LatchesDrawdownAndExposesStatus) {
    crypto_trader::risk::RiskManager manager(settings());
    auto breached = snapshot();
    breached.equity = 900.0;
    breached.available_balance = 900.0;
    breached.peak_equity = 1000.0;
    const auto status = manager.monitor(breached);

    EXPECT_TRUE(status.emergency_shutdown);
    EXPECT_NEAR(status.drawdown_fraction, 0.10, 1e-12);
    ASSERT_FALSE(status.reasons.empty());
}

TEST(RiskManager, RejectsInvalidPortfolioSnapshotAndSettings) {
    EXPECT_THROW(
        crypto_trader::risk::RiskManager(crypto_trader::common::RiskSettings{}),
        std::invalid_argument);

    crypto_trader::risk::RiskManager manager(settings());
    auto invalid_snapshot = snapshot();
    invalid_snapshot.equity = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(manager.monitor(invalid_snapshot), std::invalid_argument);

    invalid_snapshot = snapshot();
    invalid_snapshot.daily_starting_equity = std::numeric_limits<double>::min();
    invalid_snapshot.daily_net_pnl = -std::numeric_limits<double>::max();
    EXPECT_THROW(manager.monitor(invalid_snapshot), std::invalid_argument);
}

}  // namespace
