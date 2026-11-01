#include "execution/order_executor.hpp"

#include <cmath>
#include <stdexcept>

namespace crypto_trader::execution {
namespace {

bool valid_symbol(const std::string& symbol) {
    if (symbol.empty()) {
        return false;
    }
    for (const char character : symbol) {
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9'))) {
            return false;
        }
    }
    return true;
}

void validate_request(const OrderRequest& request) {
    if (!valid_symbol(request.symbol)) {
        throw std::invalid_argument("Order request symbol must use uppercase letters and digits");
    }
    if (request.side != risk::OrderSide::buy &&
        request.side != risk::OrderSide::sell) {
        throw std::invalid_argument("Order request side is invalid");
    }
    if (!std::isfinite(request.quantity) || request.quantity <= 0.0 ||
        !std::isfinite(request.reference_price) || request.reference_price <= 0.0 ||
        !std::isfinite(request.quantity * request.reference_price)) {
        throw std::invalid_argument("Order request quantity, reference price, or notional is invalid");
    }
}

}  // namespace

SubmissionResult DryRunOrderExecutor::submit(const OrderRequest& request) const {
    validate_request(request);
    return {
        .status = SubmissionStatus::simulated,
        .request = request,
        .reference_notional = request.quantity * request.reference_price,
        .message = "Dry run only; no order was sent, accepted by an exchange, or persisted"
    };
}

SubmissionResult DisabledLiveOrderExecutor::submit(const OrderRequest& request) const {
    return {
        .status = SubmissionStatus::rejected,
        .request = request,
        .reference_notional = 0.0,
        .message = "Live order submission is disabled and is not implemented"
    };
}

const char* to_string(const SubmissionStatus status) noexcept {
    switch (status) {
    case SubmissionStatus::simulated: return "SIMULATED";
    case SubmissionStatus::rejected: return "REJECTED";
    }
    return "REJECTED";
}

const char* to_string(const risk::OrderSide side) noexcept {
    switch (side) {
    case risk::OrderSide::buy: return "BUY";
    case risk::OrderSide::sell: return "SELL";
    }
    return "UNKNOWN";
}

}  // namespace crypto_trader::execution
