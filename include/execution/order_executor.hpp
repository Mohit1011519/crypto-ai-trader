#pragma once

#include <string>

#include "risk/risk_manager.hpp"

namespace crypto_trader::execution {

struct OrderRequest {
    std::string symbol;
    risk::OrderSide side;
    double quantity;
    double reference_price;
};

enum class SubmissionStatus {
    simulated,
    rejected
};

struct SubmissionResult {
    SubmissionStatus status;
    OrderRequest request;
    double reference_notional;
    std::string message;
};

class OrderExecutor {
public:
    virtual ~OrderExecutor() = default;
    virtual SubmissionResult submit(const OrderRequest& request) const = 0;
};

class DryRunOrderExecutor final : public OrderExecutor {
public:
    SubmissionResult submit(const OrderRequest& request) const override;
};

class DisabledLiveOrderExecutor final : public OrderExecutor {
public:
    SubmissionResult submit(const OrderRequest& request) const override;
};

const char* to_string(SubmissionStatus status) noexcept;
const char* to_string(risk::OrderSide side) noexcept;

}  // namespace crypto_trader::execution
