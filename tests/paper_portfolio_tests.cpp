#include "paper/paper_portfolio.hpp"

#include "ai/signal_engine.hpp"
#include "market/market_scanner.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

namespace {

class TemporaryState {
public:
    TemporaryState() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() /
            ("crypto-trader-paper-tests-" + std::to_string(nonce));
        path_ = directory_ / "portfolio.json";
    }

    ~TemporaryState() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

crypto_trader::common::RiskSettings risk_settings() {
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

crypto_trader::market::ScanResult scan_result(
    const double close,
    const crypto_trader::ai::Signal action,
    const double atr = 2.0) {
    crypto_trader::ai::TradingSignal signal{
        .metadata = {.timestamp_ms = 1'800'000'000'000, .sample_count = 100, .quality = 1.0},
        .action = action,
        .research_score = 0.8,
        .confidence = 0.8,
        .research_coverage = 1.0,
        .confirmations = 3,
        .expected_reward_risk_ratio = 1.5,
        .estimated_round_trip_cost_percent = 0.008,
        .components = {},
        .reasons = {}
    };
    crypto_trader::indicators::TechnicalIndicators technical{
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
    return {
        .symbol = "BTCUSDT",
        .technical = technical,
        .trend = std::nullopt,
        .volume = std::nullopt,
        .volatility = std::nullopt,
        .order_book = std::nullopt,
        .signal = signal,
        .candle_error = std::nullopt,
        .order_book_error = std::nullopt
    };
}

crypto_trader::paper::PaperPortfolio portfolio(const TemporaryState& file) {
    return crypto_trader::paper::PaperPortfolio(
        file.path(), 10000.0, risk_settings(), 0.005);
}

TEST(PaperPortfolio, PersistsRiskApprovedBuyAndReloadsAccountAndLedger) {
    TemporaryState file;
    auto account = portfolio(file);
    crypto_trader::risk::RiskManager risk_manager(risk_settings());
    const auto executions = account.process_scan(
        {scan_result(100.0, crypto_trader::ai::Signal::buy)},
        risk_manager, 1'800'000'000'000);

    ASSERT_EQ(executions.size(), 1);
    EXPECT_EQ(executions.front().side, "BUY");
    EXPECT_GT(executions.front().fee, 0.0);
    const auto view = account.view();
    ASSERT_EQ(view.positions.size(), 1);
    EXPECT_LT(view.cash_balance, 10000.0);
    EXPECT_GT(view.gross_exposure, 0.0);
    EXPECT_TRUE(std::filesystem::exists(file.path()));

    auto restored = crypto_trader::paper::PaperPortfolio(
        file.path(), 5000.0, risk_settings(), 0.005);
    EXPECT_EQ(restored.view().positions.size(), 1);
    EXPECT_DOUBLE_EQ(restored.view().cash_balance, view.cash_balance);
    EXPECT_EQ(restored.execution_history().size(), 1);
    EXPECT_DOUBLE_EQ(restored.execution_history().front().quantity,
                     executions.front().quantity);
}

TEST(PaperPortfolio, ClosesPositionOnSellSignalAndRecordsNetRealizedPnl) {
    TemporaryState file;
    auto account = portfolio(file);
    crypto_trader::risk::RiskManager risk_manager(risk_settings());
    account.process_scan(
        {scan_result(100.0, crypto_trader::ai::Signal::buy)},
        risk_manager, 1'800'000'000'000);

    const auto exits = account.process_scan(
        {scan_result(104.0, crypto_trader::ai::Signal::sell)},
        risk_manager, 1'800'000'060'000);
    ASSERT_EQ(exits.size(), 1);
    EXPECT_EQ(exits.front().side, "SELL");
    EXPECT_EQ(exits.front().reason, "advisory sell signal");
    EXPECT_GT(exits.front().realized_pnl, 0.0);
    EXPECT_TRUE(account.view().positions.empty());
    EXPECT_GT(account.view().cash_balance, 10000.0);
    EXPECT_EQ(account.execution_history().size(), 2);
}

TEST(PaperPortfolio, ClosesAtConfiguredTakeProfitWithSimulatedCosts) {
    TemporaryState file;
    auto account = portfolio(file);
    crypto_trader::risk::RiskManager risk_manager(risk_settings());
    account.process_scan(
        {scan_result(100.0, crypto_trader::ai::Signal::buy)},
        risk_manager, 1'800'000'000'000);

    const auto exits = account.process_scan(
        {scan_result(110.0, crypto_trader::ai::Signal::hold)},
        risk_manager, 1'800'000'060'000);
    ASSERT_EQ(exits.size(), 1);
    EXPECT_EQ(exits.front().reason, "take-profit at candle close");
    EXPECT_LT(exits.front().price, 110.0);
    EXPECT_TRUE(account.view().positions.empty());
}

TEST(PaperPortfolio, StopLossClosesPositionAndPersistsRiskShutdown) {
    TemporaryState file;
    auto account = portfolio(file);
    crypto_trader::risk::RiskManager risk_manager(risk_settings());
    account.process_scan(
        {scan_result(100.0, crypto_trader::ai::Signal::buy)},
        risk_manager, 1'800'000'000'000);

    const auto executions = account.process_scan(
        {scan_result(70.0, crypto_trader::ai::Signal::buy)},
        risk_manager, 1'800'000'060'000);
    ASSERT_EQ(executions.size(), 1);
    EXPECT_EQ(executions.front().side, "SELL");
    EXPECT_EQ(executions.front().reason, "protective stop at candle close");
    EXPECT_TRUE(account.view().positions.empty());
    EXPECT_TRUE(account.view().trading_halted);

    auto restored = portfolio(file);
    EXPECT_TRUE(restored.view().trading_halted);
    EXPECT_EQ(restored.execution_history().size(), 2);
    EXPECT_THROW(restored.reset_risk_shutdown(risk_manager), std::logic_error);
}

TEST(PaperPortfolio, RejectsCorruptedPersistedStateWithoutResettingIt) {
    TemporaryState file;
    {
        auto account = portfolio(file);
        EXPECT_DOUBLE_EQ(account.view().equity, 10000.0);
    }
    {
        std::ofstream corrupt(file.path(), std::ios::trunc);
        corrupt << "{invalid json";
    }
    EXPECT_THROW(
        crypto_trader::paper::PaperPortfolio(file.path(), 10000.0, risk_settings(), 0.005),
        nlohmann::json::parse_error);
}

}  // namespace
