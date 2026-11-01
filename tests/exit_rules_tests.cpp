#include "risk/exit_rules.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

namespace {

crypto_trader::common::RiskSettings exit_settings() {
    return {
        .risk_per_trade_fraction = 0.005,
        .maximum_open_positions = 3,
        .daily_maximum_loss_fraction = 0.02,
        .maximum_drawdown_fraction = 0.10,
        .maximum_gross_exposure_fraction = 1.0,
        .stop_atr_multiple = 2.0,
        .target_atr_multiple = 10.0,
        .taker_fee_rate = 0.001,
        .slippage_rate = 0.0005,
        .trailing_stop_atr_multiple = 4.0,
        .breakeven_trigger_r = 1.5
    };
}

crypto_trader::risk::ProtectiveStopState entry_at_100() {
    return {
        .entry_price = 100.0,
        .initial_stop_price = 96.0,
        .stop_price = 96.0,
        .highest_close = 100.0
    };
}

}  // namespace

TEST(ExitRules, KeepsInitialStopUntilPriceMovesFavourably) {
    auto state = entry_at_100();
    EXPECT_DOUBLE_EQ(
        crypto_trader::risk::advance_protective_stop(state, 101.0, 2.0, exit_settings()),
        96.0);
    EXPECT_DOUBLE_EQ(state.highest_close, 101.0);
}

TEST(ExitRules, MovesToBreakEvenAfterConfiguredMultipleOfRisk) {
    auto state = entry_at_100();
    // 1.5R above entry with 4.0 initial risk is 106; a wide ATR keeps the trail lower.
    const double stop = crypto_trader::risk::advance_protective_stop(
        state, 106.0, 10.0, exit_settings());
    EXPECT_DOUBLE_EQ(stop, 100.0 * (1.0 + 2.0 * (0.001 + 0.0005)));
}

TEST(ExitRules, TrailsHighestCloseAndNeverLowersTheStop) {
    auto state = entry_at_100();
    const auto settings = exit_settings();
    EXPECT_DOUBLE_EQ(
        crypto_trader::risk::advance_protective_stop(state, 120.0, 2.0, settings), 112.0);
    EXPECT_DOUBLE_EQ(
        crypto_trader::risk::advance_protective_stop(state, 110.0, 2.0, settings), 112.0);
    EXPECT_DOUBLE_EQ(
        crypto_trader::risk::advance_protective_stop(state, 121.0, 1.0, settings), 117.0);
}

TEST(ExitRules, DisabledRulesLeaveTheStopUnchanged) {
    auto state = entry_at_100();
    auto settings = exit_settings();
    settings.trailing_stop_atr_multiple = 0.0;
    settings.breakeven_trigger_r = 0.0;
    EXPECT_DOUBLE_EQ(
        crypto_trader::risk::advance_protective_stop(state, 150.0, 2.0, settings), 96.0);
}

TEST(ExitRules, RejectsInvalidState) {
    auto state = entry_at_100();
    state.initial_stop_price = 101.0;
    EXPECT_THROW(
        crypto_trader::risk::advance_protective_stop(state, 100.0, 2.0, exit_settings()),
        std::invalid_argument);
}
