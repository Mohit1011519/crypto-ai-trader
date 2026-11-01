#include "execution/order_executor.hpp"

#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace {

crypto_trader::execution::OrderRequest valid_request() {
    return {
        .symbol = "BTCUSDT",
        .side = crypto_trader::risk::OrderSide::buy,
        .quantity = 0.25,
        .reference_price = 40000.0
    };
}

TEST(OrderExecutor, DryRunReturnsExplicitSimulationWithoutExchangeAcceptance) {
    const crypto_trader::execution::DryRunOrderExecutor executor;
    const auto result = executor.submit(valid_request());

    EXPECT_EQ(result.status, crypto_trader::execution::SubmissionStatus::simulated);
    EXPECT_EQ(result.reference_notional, 10000.0);
    EXPECT_NE(result.message.find("no order was sent"), std::string::npos);
}

TEST(OrderExecutor, DryRunRejectsInvalidSymbolQuantityAndReferencePrice) {
    const crypto_trader::execution::DryRunOrderExecutor executor;

    auto invalid = valid_request();
    invalid.symbol = "BTC/USDT";
    EXPECT_THROW(executor.submit(invalid), std::invalid_argument);

    invalid = valid_request();
    invalid.quantity = 0.0;
    EXPECT_THROW(executor.submit(invalid), std::invalid_argument);

    invalid = valid_request();
    invalid.reference_price = std::numeric_limits<double>::infinity();
    EXPECT_THROW(executor.submit(invalid), std::invalid_argument);
}

TEST(OrderExecutor, LiveAdapterAlwaysRejectsAndCannotSubmit) {
    const crypto_trader::execution::DisabledLiveOrderExecutor executor;
    const auto result = executor.submit(valid_request());

    EXPECT_EQ(result.status, crypto_trader::execution::SubmissionStatus::rejected);
    EXPECT_DOUBLE_EQ(result.reference_notional, 0.0);
    EXPECT_NE(result.message.find("disabled"), std::string::npos);

    const auto malformed_request = crypto_trader::execution::OrderRequest{
        .symbol = "",
        .side = crypto_trader::risk::OrderSide::buy,
        .quantity = -1.0,
        .reference_price = 0.0
    };
    EXPECT_EQ(executor.submit(malformed_request).status,
              crypto_trader::execution::SubmissionStatus::rejected);
}

}  // namespace
