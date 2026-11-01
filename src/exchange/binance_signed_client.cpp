#include "exchange/binance_signed_client.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>

namespace crypto_trader::exchange {
namespace {

std::int64_t local_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

size_t write_response(char* data, size_t size, size_t count, void* user_data) {
    const auto bytes = size * count;
    static_cast<std::string*>(user_data)->append(data, bytes);
    return bytes;
}

bool unreserved(const char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9') || character == '-' || character == '_' ||
        character == '.' || character == '~';
}

std::string percent_encode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char character : value) {
        if (unreserved(static_cast<char>(character))) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4]);
            encoded.push_back(hex[character & 0x0F]);
        }
    }
    return encoded;
}

std::string build_query(const QueryParameters& parameters) {
    std::string query;
    for (const auto& [key, value] : parameters) {
        if (!query.empty()) {
            query.push_back('&');
        }
        query += percent_encode(key) + "=" + percent_encode(value);
    }
    return query;
}

}  // namespace

BinanceApiError::BinanceApiError(const long http_status, const int code,
                                 const std::string& message)
    : std::runtime_error("Binance API error " + std::to_string(code) +
                         " (HTTP " + std::to_string(http_status) + "): " + message),
      http_status_(http_status),
      code_(code) {}

ApiCredentials credentials_from_environment() {
    const char* key = std::getenv("BINANCE_TESTNET_API_KEY");
    const char* secret = std::getenv("BINANCE_TESTNET_API_SECRET");
    if (key == nullptr || secret == nullptr || *key == '\0' || *secret == '\0') {
        throw std::runtime_error(
            "Set BINANCE_TESTNET_API_KEY and BINANCE_TESTNET_API_SECRET "
            "(create keys at https://testnet.binance.vision)");
    }
    return {key, secret};
}

std::string hmac_sha256_hex(const std::string& secret, const std::string& payload) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(payload.data()), payload.size(),
             digest.data(), &digest_length) == nullptr) {
        throw std::runtime_error("HMAC-SHA256 signing failed");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(digest_length * 2);
    for (unsigned int index = 0; index < digest_length; ++index) {
        output.push_back(hex[digest[index] >> 4]);
        output.push_back(hex[digest[index] & 0x0F]);
    }
    return output;
}

BinanceSignedClient::BinanceSignedClient(std::string base_url,
                                         ApiCredentials credentials,
                                         const int timeout_seconds,
                                         const int receive_window_ms)
    : base_url_(std::move(base_url)),
      credentials_(std::move(credentials)),
      timeout_seconds_(timeout_seconds),
      receive_window_ms_(receive_window_ms) {
    if (base_url_.empty() || timeout_seconds_ < 1 || receive_window_ms_ < 1 ||
        credentials_.api_key.empty() != credentials_.api_secret.empty()) {
        throw std::invalid_argument("Binance signed client settings are invalid");
    }
}

nlohmann::json BinanceSignedClient::public_get(const std::string& path,
                                               const QueryParameters& parameters) const {
    return send("GET", path, build_query(parameters), false);
}

nlohmann::json BinanceSignedClient::signed_request(const std::string& method,
                                                   const std::string& path,
                                                   QueryParameters parameters) {
    if (credentials_.api_key.empty()) {
        throw std::logic_error("Signed Binance request attempted without API credentials");
    }
    std::int64_t offset = 0;
    synchronize_clock();
    {
        std::lock_guard lock(clock_mutex_);
        offset = clock_offset_ms_;
    }
    parameters.emplace_back("recvWindow", std::to_string(receive_window_ms_));
    parameters.emplace_back("timestamp", std::to_string(local_time_ms() + offset));
    auto query = build_query(parameters);
    query += "&signature=" + hmac_sha256_hex(credentials_.api_secret, query);
    try {
        return send(method, path, query, true);
    } catch (const BinanceApiError& error) {
        if (error.code() == -1021) {
            // Timestamp outside recvWindow: resynchronize on the next request.
            std::lock_guard lock(clock_mutex_);
            clock_synchronized_ = false;
        }
        throw;
    }
}

void BinanceSignedClient::synchronize_clock() {
    {
        std::lock_guard lock(clock_mutex_);
        if (clock_synchronized_) {
            return;
        }
    }
    const auto before = local_time_ms();
    const auto response = send("GET", "/api/v3/time", "", false);
    const auto after = local_time_ms();
    const auto server_time = response.at("serverTime").get<std::int64_t>();
    std::lock_guard lock(clock_mutex_);
    clock_offset_ms_ = server_time - (before + after) / 2;
    clock_synchronized_ = true;
}

nlohmann::json BinanceSignedClient::send(const std::string& method,
                                         const std::string& path,
                                         const std::string& query,
                                         const bool with_api_key) const {
    static const auto curl_initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_initialized != CURLE_OK) {
        throw std::runtime_error("curl_global_init failed");
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(
        curl_easy_init(), &curl_easy_cleanup);
    if (!handle) {
        throw std::runtime_error("curl_easy_init failed");
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
        nullptr, &curl_slist_free_all);
    if (with_api_key) {
        const std::string header = "X-MBX-APIKEY: " + credentials_.api_key;
        headers.reset(curl_slist_append(nullptr, header.c_str()));
    }

    const std::string url = base_url_ + path + (query.empty() ? "" : "?" + query);
    std::string response;
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    if (method == "POST") {
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, "");
    }
    if (headers) {
        curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    }
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, static_cast<long>(timeout_seconds_));
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds_));
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "crypto-ai-trader/0.2.0");
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode result = curl_easy_perform(handle.get());
    if (result != CURLE_OK) {
        throw BinanceUnknownOutcome(
            std::string("Binance request failed in transport: ") + curl_easy_strerror(result));
    }
    long status = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status >= 500) {
        throw BinanceUnknownOutcome(
            "Binance returned HTTP " + std::to_string(status) + "; outcome unknown");
    }
    const auto body = nlohmann::json::parse(response, nullptr, false);
    if (status < 200 || status >= 300) {
        if (!body.is_discarded() && body.is_object() && body.contains("code")) {
            throw BinanceApiError(status, body.at("code").get<int>(),
                                  body.value("msg", std::string("unknown error")));
        }
        throw BinanceApiError(status, 0, response.substr(0, 300));
    }
    if (body.is_discarded()) {
        throw std::runtime_error("Binance returned a non-JSON response");
    }
    return body;
}

}  // namespace crypto_trader::exchange
