#include "live/live_trader.hpp"

#include "ai/signal_engine.hpp"
#include "exchange/binance_signed_client.hpp"
#include "market/market_scanner.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

namespace {

using crypto_trader::execution::OrderState;
using crypto_trader::execution::VenueOrder;

class FakeVenue final : public crypto_trader::execution::SpotVenue {
public:
    double bid = 100.0;
    double ask = 100.01;
    double base_free = 0.0;
    double base_locked = 0.0;
    double quote_free = 10000.0;
    bool reject_stops = false;
    int stops_placed = 0;
    int cancels = 0;
    std::map<std::int64_t, VenueOrder> orders;
    std::map<std::int64_t, double> stop_prices;

    crypto_trader::execution::SymbolRules symbol_rules(const std::string&) override {
        return {
            .base_asset = "BTC",
            .quote_asset = "USDT",
            .step_size = 0.0001,
            .minimum_quantity = 0.0001,
            .tick_size = 0.01,
            .minimum_notional = 5.0,
            .stop_market_supported = true
        };
    }
    crypto_trader::execution::BookTop book_top(const std::string&) override {
        return {bid, ask};
    }
    crypto_trader::execution::AssetBalance balance(const std::string& asset) override {
        return asset == "USDT" ? crypto_trader::execution::AssetBalance{quote_free, 0.0}
                               : crypto_trader::execution::AssetBalance{base_free, base_locked};
    }
    VenueOrder market_order(const std::string&, const crypto_trader::risk::OrderSide side,
                            const double quantity, const std::string& client_id) override {
        VenueOrder order{next_id_++, client_id, OrderState::filled, quantity, 0.0, 0.0, 0.0};
        if (side == crypto_trader::risk::OrderSide::buy) {
            order.quote_quantity = quantity * ask;
            order.base_commission = quantity * 0.001;
            quote_free -= order.quote_quantity;
            base_free += quantity - order.base_commission;
        } else {
            order.quote_quantity = quantity * bid;
            order.quote_commission = order.quote_quantity * 0.001;
            base_free -= quantity;
            quote_free += order.quote_quantity - order.quote_commission;
        }
        orders[order.order_id] = order;
        return order;
    }
    VenueOrder place_stop_market_sell(const std::string&, const double quantity,
                                      const double stop_price,
                                      const std::string& client_id) override {
        if (reject_stops || stop_price >= bid) {
            throw crypto_trader::exchange::BinanceApiError(400, -2010, "stop rejected");
        }
        ++stops_placed;
        VenueOrder order{next_id_++, client_id, OrderState::open, 0.0, 0.0, 0.0, 0.0};
        orders[order.order_id] = order;
        stop_prices[order.order_id] = stop_price;
        quantities_[order.order_id] = quantity;
        base_free -= quantity;
        base_locked += quantity;
        return order;
    }
    VenueOrder query_order(const std::string&, const std::int64_t order_id) override {
        return orders.at(order_id);
    }
    VenueOrder cancel_order(const std::string&, const std::int64_t order_id) override {
        auto& order = orders.at(order_id);
        if (order.state == OrderState::open) {
            ++cancels;
            order.state = OrderState::cancelled;
            base_locked -= quantities_[order_id];
            base_free += quantities_[order_id];
        }
        return order;
    }

    // Simulates the exchange triggering resting stops at the current bid.
    void move_to(const double new_bid) {
        bid = new_bid;
        ask = new_bid + 0.01;
        for (auto& [id, order] : orders) {
            if (order.state == OrderState::open && bid <= stop_prices[id]) {
                order.state = OrderState::filled;
                order.executed_quantity = quantities_[id];
                order.quote_quantity = quantities_[id] * bid;
                order.quote_commission = order.quote_quantity * 0.001;
                base_locked -= quantities_[id];
                quote_free += order.quote_quantity - order.quote_commission;
            }
        }
    }

    std::int64_t open_stop_id() const {
        for (const auto& [id, order] : orders) {
            if (order.state == OrderState::open) {
                return id;
            }
        }
        return 0;
    }

private:
    std::int64_t next_id_ = 1;
    std::map<std::int64_t, double> quantities_;
};

class TemporaryState {
public:
    TemporaryState() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() /
            ("crypto-trader-live-tests-" + std::to_string(nonce));
    }
    ~TemporaryState() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
    std::filesystem::path path() const { return directory_ / "live.json"; }

private:
    std::filesystem::path directory_;
};

crypto_trader::common::RiskSettings risk_settings() {
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

crypto_trader::common::LiveSettings live_settings(const TemporaryState& file) {
    crypto_trader::common::LiveSettings settings;
    settings.state_path = file.path();
    settings.capital_allocation = 1000.0;
    settings.maximum_order_notional = 250.0;
    return settings;
}

crypto_trader::market::ScanResult scan(const double close,
                                       const crypto_trader::ai::Signal action,
                                       const double atr = 2.0) {
    crypto_trader::market::ScanResult result{.symbol = "BTCUSDT"};
    result.technical = crypto_trader::indicators::TechnicalIndicators{
        .metadata = {.timestamp_ms = 1'800'000'000'000, .sample_count = 100, .quality = 1.0},
        .close = close,
        .ema_fast = std::nullopt,
        .ema_medium = std::nullopt,
        .ema_slow = std::nullopt,
        .rsi = std::nullopt,
        .macd = std::nullopt,
        .macd_signal = std::nullopt,
        .macd_histogram = std::nullopt,
        .bollinger_middle = std::nullopt,
        .bollinger_upper = std::nullopt,
        .bollinger_lower = std::nullopt,
        .atr = atr,
        .stochastic_rsi = std::nullopt
    };
    result.signal = crypto_trader::ai::TradingSignal{
        .metadata = result.technical->metadata,
        .action = action,
        .research_score = 0.8,
        .confidence = 0.8,
        .research_coverage = 1.0,
        .confirmations = 3,
        .expected_reward_risk_ratio = 4.0,
        .estimated_round_trip_cost_percent = 0.003,
        .components = {},
        .reasons = {}
    };
    return result;
}

constexpr std::int64_t t0 = 1'800'000'000'000;

}  // namespace

TEST(LiveTrader, BuyIsSizedWithinCapitalLimitsAndProtectedByExchangeStop) {
    TemporaryState file;
    FakeVenue venue;
    crypto_trader::risk::RiskManager risk_manager(risk_settings());
    crypto_trader::live::LiveTrader trader(live_settings(file), risk_settings(), venue);

    const auto executions = trader.process_scan(
        {scan(100.0, crypto_trader::ai::Signal::buy)}, risk_manager, t0);

    ASSERT_EQ(executions.size(), 1);
    EXPECT_EQ(executions.front().side, "BUY");
    const auto view = trader.view();
    ASSERT_EQ(view.positions.size(), 1);
    const auto& position = view.positions.front();
    EXPECT_LE(position.quantity * position.entry_price, 250.0);
    // 0.5% of the 1000 allocation is risked, not 0.5% of the 10000 account.
    EXPECT_LE(position.quantity * (position.entry_price - position.stop_loss_price), 5.0);
    EXPECT_NEAR(position.stop_loss_price, position.entry_price - 4.0, 0.05);
    EXPECT_NE(position.stop_order_id, 0);
    EXPECT_EQ(venue.stops_placed, 1);
    EXPECT_LT(view.cash_balance, 1000.0);

    crypto_trader::live::LiveTrader restored(live_settings(file), risk_settings(), venue);
    EXPECT_EQ(restored.view().positions.size(), 1);
    EXPECT_EQ(restored.view().positions.front().stop_order_id, position.stop_order_id);
}

TEST(LiveTrader, MaximumOrderNotionalCapsTheEntry) {
    TemporaryState file;
    FakeVenue venue;
    auto settings = live_settings(file);
    settings.maximum_order_notional = 50.0;
    crypto_trader::risk::RiskManager risk_manager(risk_settings());
    crypto_trader::live::LiveTrader trader(settings, risk_settings(), venue);
    trader.process_scan({scan(100.0, crypto_trader::ai::Signal::buy)}, risk_manager, t0);

    ASSERT_EQ(trader.view().positions.size(), 1);
    const auto& position = trader.view().positions.front();
    EXPECT_LE(position.quantity * venue.ask, 50.0 + 1e-9);
}

TEST(LiveTrader, ExchangeStopFillIsRecordedAsBoundedLoss) {
    TemporaryState file;
    FakeVenue venue;
    crypto_trader::risk::RiskManager risk_manager(risk_settings());
    crypto_trader::live::LiveTrader trader(live_settings(file), risk_settings(), venue);
    trader.process_scan({scan(100.0, crypto_trader::ai::Signal::buy)}, risk_manager, t0);

    venue.move_to(95.0);
    const auto exits = trader.process_scan(
        {scan(95.0, crypto_trader::ai::Signal::hold)}, risk_manager, t0 + 60'000);

    ASSERT_EQ(exits.size(), 1);
    EXPECT_EQ(exits.front().reason, "exchange stop-loss");
    EXPECT_LT(exits.front().realized_pnl, 0.0);
    // The loss stays near the 5 USDT risk budget (plus gap and fees).
    EXPECT_GT(exits.front().realized_pnl, -8.0);
    EXPECT_TRUE(trader.view().positions.empty());
}

TEST(LiveTrader, RatchetedStopReplacesTheExchangeOrder) {
    TemporaryState file;
    FakeVenue venue;
    crypto_trader::risk::RiskManager risk_manager(risk_settings());
    crypto_trader::live::LiveTrader trader(live_settings(file), risk_settings(), venue);
    trader.process_scan({scan(100.0, crypto_trader::ai::Signal::buy)}, risk_manager, t0);
    const auto first_stop = venue.open_stop_id();

    venue.move_to(120.0);
    EXPECT_TRUE(trader.process_scan(
        {scan(120.0, crypto_trader::ai::Signal::hold)}, risk_manager, t0 + 60'000).empty());

    ASSERT_EQ(trader.view().positions.size(), 1);
    EXPECT_DOUBLE_EQ(trader.view().positions.front().stop_loss_price, 112.0);
    EXPECT_EQ(venue.cancels, 1);
    EXPECT_EQ(venue.stops_placed, 2);
    EXPECT_NE(venue.open_stop_id(), first_stop);
    EXPECT_DOUBLE_EQ(venue.stop_prices[venue.open_stop_id()], 112.0);
}

TEST(LiveTrader, RejectedStopForcesImmediateExitAndHaltsTrading) {
    TemporaryState file;
    FakeVenue venue;
    venue.reject_stops = true;
    crypto_trader::risk::RiskManager risk_manager(risk_settings());
    crypto_trader::live::LiveTrader trader(live_settings(file), risk_settings(), venue);

    const auto executions = trader.process_scan(
        {scan(100.0, crypto_trader::ai::Signal::buy)}, risk_manager, t0);

    ASSERT_EQ(executions.size(), 2);
    EXPECT_EQ(executions.back().side, "SELL");
    EXPECT_EQ(executions.back().reason, "protective stop rejected");
    EXPECT_TRUE(trader.view().positions.empty());
    EXPECT_TRUE(trader.view().trading_halted);
}

TEST(LiveTrader, TrendBreakAndFlattenSellAtMarketAndCancelStops) {
    TemporaryState file;
    FakeVenue venue;
    crypto_trader::risk::RiskManager risk_manager(risk_settings());
    crypto_trader::live::LiveTrader trader(live_settings(file), risk_settings(), venue);
    trader.process_scan({scan(100.0, crypto_trader::ai::Signal::buy)}, risk_manager, t0);

    auto broken = scan(101.0, crypto_trader::ai::Signal::hold);
    broken.signal->exit_long = true;
    venue.move_to(101.0);
    const auto exits = trader.process_scan({broken}, risk_manager, t0 + 60'000);
    ASSERT_EQ(exits.size(), 1);
    EXPECT_EQ(exits.front().reason, "trend break");
    EXPECT_EQ(venue.open_stop_id(), 0);

    auto next_candle = scan(101.0, crypto_trader::ai::Signal::buy);
    next_candle.signal->metadata.timestamp_ms += 14'400'000;
    trader.process_scan({next_candle}, risk_manager, t0 + 14'460'000);
    ASSERT_EQ(trader.view().positions.size(), 1);
    const auto flattened = trader.flatten("manual flatten", t0 + 14'520'000);
    ASSERT_EQ(flattened.size(), 1);
    EXPECT_TRUE(trader.view().positions.empty());
    EXPECT_TRUE(trader.view().trading_halted);
    EXPECT_EQ(venue.open_stop_id(), 0);
}

TEST(BinanceSignedClient, SignsQueriesWithDocumentedHmacVector) {
    // Example from the Binance Spot API documentation.
    EXPECT_EQ(crypto_trader::exchange::hmac_sha256_hex(
                  "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j",
                  "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1"
                  "&recvWindow=5000&timestamp=1499827319559"),
              "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71");
}

TEST(SpotVenue, RoundsDownToExchangeIncrements) {
    EXPECT_EQ(crypto_trader::execution::format_to_increment(0.123456, 0.0001), "0.1234");
    EXPECT_EQ(crypto_trader::execution::format_to_increment(0.3, 0.1), "0.3");
    EXPECT_EQ(crypto_trader::execution::format_to_increment(77901.239, 0.01), "77901.23");
    EXPECT_EQ(crypto_trader::execution::format_to_increment(12.9, 1.0), "12");
}

TEST(LiveTrader, RestartDoesNotRebuyOnAnAlreadyUsedCandleSignal) {
    TemporaryState file;
    FakeVenue venue;
    crypto_trader::risk::RiskManager risk_manager(risk_settings());
    {
        crypto_trader::live::LiveTrader trader(live_settings(file), risk_settings(), venue);
        trader.process_scan({scan(100.0, crypto_trader::ai::Signal::buy)}, risk_manager, t0);
        venue.move_to(95.0);
        trader.process_scan({scan(95.0, crypto_trader::ai::Signal::hold)}, risk_manager,
                            t0 + 60'000);
        ASSERT_TRUE(trader.view().positions.empty());
    }
    venue.move_to(100.0);
    crypto_trader::live::LiveTrader restarted(live_settings(file), risk_settings(), venue);
    // Same closed candle's BUY is replayed after the restart: it must be ignored.
    EXPECT_TRUE(restarted.process_scan(
        {scan(100.0, crypto_trader::ai::Signal::buy)}, risk_manager, t0 + 120'000).empty());

    auto next_candle = scan(100.0, crypto_trader::ai::Signal::buy);
    next_candle.signal->metadata.timestamp_ms += 14'400'000;
    EXPECT_EQ(restarted.process_scan({next_candle}, risk_manager, t0 + 14'460'000).size(), 1);
}

TEST(LiveTrader, DailyLossPausesNewEntriesUntilTheNextDayWithoutHalting) {
    TemporaryState file;
    FakeVenue venue;
    auto settings = risk_settings();
    settings.daily_maximum_loss_fraction = 0.004;
    crypto_trader::risk::RiskManager risk_manager(settings);
    crypto_trader::live::LiveTrader trader(live_settings(file), settings, venue);
    trader.process_scan({scan(100.0, crypto_trader::ai::Signal::buy)}, risk_manager, t0);
    venue.move_to(95.0);
    trader.process_scan({scan(95.0, crypto_trader::ai::Signal::hold)}, risk_manager, t0 + 60'000);
    EXPECT_FALSE(trader.view().trading_halted);

    venue.move_to(100.0);
    auto later_candle = scan(100.0, crypto_trader::ai::Signal::buy);
    later_candle.signal->metadata.timestamp_ms += 14'400'000;
    EXPECT_TRUE(trader.process_scan({later_candle}, risk_manager, t0 + 120'000).empty());

    later_candle.signal->metadata.timestamp_ms += 86'400'000;
    EXPECT_EQ(trader.process_scan({later_candle}, risk_manager, t0 + 86'520'000).size(), 1);
}
