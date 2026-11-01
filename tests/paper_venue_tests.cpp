#include "execution/paper_venue.hpp"

#include <chrono>
#include <filesystem>
#include <system_error>

#include <gtest/gtest.h>

namespace {

class PriceFeed final : public crypto_trader::execution::SpotVenue {
public:
    double bid = 100.0;

    crypto_trader::execution::SymbolRules symbol_rules(const std::string&) override {
        return {"BTC", "USDT", 0.0001, 0.0001, 0.01, 5.0, true};
    }
    crypto_trader::execution::BookTop book_top(const std::string&) override {
        return {bid, bid + 0.01};
    }
    crypto_trader::execution::AssetBalance balance(const std::string&) override {
        throw std::logic_error("unused");
    }
    crypto_trader::execution::VenueOrder market_order(
        const std::string&, crypto_trader::risk::OrderSide, double, const std::string&) override {
        throw std::logic_error("unused");
    }
    crypto_trader::execution::VenueOrder place_stop_market_sell(
        const std::string&, double, double, const std::string&) override {
        throw std::logic_error("unused");
    }
    crypto_trader::execution::VenueOrder query_order(const std::string&, std::int64_t) override {
        throw std::logic_error("unused");
    }
    crypto_trader::execution::VenueOrder cancel_order(const std::string&, std::int64_t) override {
        throw std::logic_error("unused");
    }
};

class TemporaryFile {
public:
    TemporaryFile() {
        path_ = std::filesystem::temp_directory_path() /
            ("paper-venue-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    }
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST(PaperVenue, FillsMarketOrdersWithFeesAndSlippageAgainstLiveBook) {
    TemporaryFile file;
    PriceFeed feed;
    crypto_trader::execution::PaperVenue venue(feed, file.path(), "USDT", 100.0, 0.001, 0.0005);
    const auto buy = venue.market_order("BTCUSDT", crypto_trader::risk::OrderSide::buy, 0.2, "a");
    EXPECT_NEAR(buy.quote_quantity, 0.2 * 100.01 * 1.0005, 1e-9);
    EXPECT_NEAR(venue.balance("USDT").free, 100.0 - buy.quote_quantity - buy.quote_commission, 1e-9);
    EXPECT_DOUBLE_EQ(venue.balance("BTC").free, 0.2);
    EXPECT_THROW(venue.market_order("BTCUSDT", crypto_trader::risk::OrderSide::buy, 5.0, "b"),
                 crypto_trader::exchange::BinanceApiError);
}

TEST(PaperVenue, RestingStopLocksBalanceTriggersBelowStopAndSurvivesRestart) {
    TemporaryFile file;
    PriceFeed feed;
    std::int64_t stop_id = 0;
    {
        crypto_trader::execution::PaperVenue venue(feed, file.path(), "USDT", 100.0, 0.001, 0.0);
        venue.market_order("BTCUSDT", crypto_trader::risk::OrderSide::buy, 0.2, "a");
        EXPECT_THROW(venue.place_stop_market_sell("BTCUSDT", 0.2, 101.0, "bad"),
                     crypto_trader::exchange::BinanceApiError);
        stop_id = venue.place_stop_market_sell("BTCUSDT", 0.2, 96.0, "s").order_id;
        EXPECT_DOUBLE_EQ(venue.balance("BTC").locked, 0.2);
        EXPECT_EQ(venue.query_order("BTCUSDT", stop_id).state,
                  crypto_trader::execution::OrderState::open);
    }
    feed.bid = 94.0;
    crypto_trader::execution::PaperVenue restarted(feed, file.path(), "USDT", 100.0, 0.001, 0.0);
    const auto filled = restarted.query_order("BTCUSDT", stop_id);
    EXPECT_EQ(filled.state, crypto_trader::execution::OrderState::filled);
    EXPECT_NEAR(filled.quote_quantity, 0.2 * 94.0, 1e-9);
    EXPECT_DOUBLE_EQ(restarted.balance("BTC").locked, 0.0);
}
