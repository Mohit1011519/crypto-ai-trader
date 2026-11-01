#pragma once

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace crypto_trader::exchange {

// Binance rejected a request with an HTTP 4xx status and an error body. The
// request is known not to have taken effect.
class BinanceApiError : public std::runtime_error {
public:
    BinanceApiError(long http_status, int code, const std::string& message);

    long http_status() const noexcept { return http_status_; }
    int code() const noexcept { return code_; }

private:
    long http_status_;
    int code_;
};

// The request may or may not have taken effect (transport failure, timeout,
// or HTTP 5xx). Order placement must be reconciled by client order id.
class BinanceUnknownOutcome : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ApiCredentials {
    std::string api_key;
    std::string api_secret;
};

// Reads BINANCE_TESTNET_API_KEY and BINANCE_TESTNET_API_SECRET.
ApiCredentials credentials_from_environment();

// Lower-case hex HMAC-SHA256 of `payload`, as required for SIGNED endpoints.
std::string hmac_sha256_hex(const std::string& secret, const std::string& payload);

using QueryParameters = std::vector<std::pair<std::string, std::string>>;

// Empty credentials give a public-data-only client; signed requests then throw.
class BinanceSignedClient {
public:
    BinanceSignedClient(std::string base_url,
                        ApiCredentials credentials,
                        int timeout_seconds,
                        int receive_window_ms);

    nlohmann::json public_get(const std::string& path, const QueryParameters& parameters) const;
    nlohmann::json signed_request(const std::string& method,
                                  const std::string& path,
                                  QueryParameters parameters);

private:
    nlohmann::json send(const std::string& method,
                        const std::string& path,
                        const std::string& query,
                        bool with_api_key) const;
    void synchronize_clock();

    std::string base_url_;
    ApiCredentials credentials_;
    int timeout_seconds_;
    int receive_window_ms_;
    std::mutex clock_mutex_;
    std::int64_t clock_offset_ms_ = 0;
    bool clock_synchronized_ = false;
};

}  // namespace crypto_trader::exchange
