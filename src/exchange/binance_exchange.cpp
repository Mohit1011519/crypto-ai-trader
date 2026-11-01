#include "exchange/binance_exchange.hpp"

#include <curl/curl.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <cmath>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace crypto_trader::exchange {
namespace {

class CurlGlobal final {
public:
    CurlGlobal() {
        const auto result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK) {
            throw std::runtime_error(std::string("curl_global_init failed: ") + curl_easy_strerror(result));
        }
    }
    ~CurlGlobal() { curl_global_cleanup(); }

    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
};

void ensure_curl_initialized() {
    static const CurlGlobal curl_global;
    (void)curl_global;
}

size_t write_response(char* data, size_t size, size_t count, void* user_data) {
    const auto bytes = size * count;
    auto* response = static_cast<std::string*>(user_data);
    response->append(data, bytes);
    return bytes;
}

double parse_number(const nlohmann::json& value, const std::string& field) {
    try {
        double number = 0.0;
        if (value.is_string()) {
            const auto text = value.get<std::string>();
            std::size_t parsed_characters = 0;
            number = std::stod(text, &parsed_characters);
            if (parsed_characters != text.size()) {
                throw std::runtime_error("value contains trailing characters");
            }
        } else {
            number = value.get<double>();
        }
        if (!std::isfinite(number)) {
            throw std::runtime_error("value must be finite");
        }
        return number;
    } catch (const std::exception& error) {
        throw std::runtime_error("Invalid Binance candle field '" + field + "': " + error.what());
    }
}

std::vector<market::Candle> parse_candles(const nlohmann::json& payload) {
    if (!payload.is_array()) {
        throw std::runtime_error("Binance klines response must be a JSON array");
    }

    std::vector<market::Candle> candles;
    candles.reserve(payload.size());
    for (const auto& row : payload) {
        if (!row.is_array() || row.size() < 8) {
            throw std::runtime_error("Binance returned a malformed kline row");
        }
        candles.push_back({
            .open_time_ms = row.at(0).get<std::int64_t>(),
            .open = parse_number(row.at(1), "open"),
            .high = parse_number(row.at(2), "high"),
            .low = parse_number(row.at(3), "low"),
            .close = parse_number(row.at(4), "close"),
            .volume = parse_number(row.at(5), "volume"),
            .close_time_ms = row.at(6).get<std::int64_t>(),
            .quote_volume = parse_number(row.at(7), "quote volume")
        });
        const auto& candle = candles.back();
        if (candle.open <= 0.0 || candle.high <= 0.0 || candle.low <= 0.0 ||
            candle.close <= 0.0 || candle.volume < 0.0 || candle.quote_volume < 0.0 ||
            candle.high < candle.low || candle.high < candle.open || candle.high < candle.close ||
            candle.low > candle.open || candle.low > candle.close ||
            candle.open_time_ms > candle.close_time_ms) {
            throw std::runtime_error("Binance returned an invalid OHLCV candle");
        }
    }
    return candles;
}

}  // namespace

BinanceExchange::BinanceExchange(common::AppConfig config) : config_(std::move(config)) {}

std::vector<market::Candle> BinanceExchange::fetch_candles(
    const std::string& symbol,
    const std::string& interval,
    int limit) const {
    if (symbol.empty() || interval.empty()) {
        throw std::invalid_argument("symbol and interval must not be empty");
    }
    if (limit < 1 || limit > 1000) {
        throw std::invalid_argument("Binance candle limit must be between 1 and 1000");
    }
    ensure_curl_initialized();

    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        throw std::runtime_error("curl_easy_init failed");
    }

    std::string response;
    char* escaped_symbol = curl_easy_escape(handle, symbol.c_str(), static_cast<int>(symbol.size()));
    char* escaped_interval = curl_easy_escape(handle, interval.c_str(), static_cast<int>(interval.size()));
    if (escaped_symbol == nullptr || escaped_interval == nullptr) {
        if (escaped_symbol != nullptr) {
            curl_free(escaped_symbol);
        }
        if (escaped_interval != nullptr) {
            curl_free(escaped_interval);
        }
        curl_easy_cleanup(handle);
        throw std::runtime_error("Failed to URL-encode Binance request parameters");
    }

    const std::string url = config_.rest_base_url + "/api/v3/klines?symbol=" +
        escaped_symbol + "&interval=" + escaped_interval + "&limit=" + std::to_string(limit);
    curl_free(escaped_symbol);
    curl_free(escaped_interval);

    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(config_.request_timeout_seconds));
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(config_.request_timeout_seconds));
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "crypto-ai-trader/0.1.0");
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode result = curl_easy_perform(handle);
    long status_code = 0;
    if (result == CURLE_OK) {
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status_code);
    }
    curl_easy_cleanup(handle);

    if (result != CURLE_OK) {
        throw std::runtime_error(std::string("Binance REST request failed: ") + curl_easy_strerror(result));
    }
    if (status_code == 418 || status_code == 429) {
        throw std::runtime_error(
            "Binance rate limit or IP ban response (HTTP " + std::to_string(status_code) +
            "); wait before retrying. Response: " + response);
    }
    if (status_code < 200 || status_code >= 300) {
        throw std::runtime_error("Binance REST returned HTTP " + std::to_string(status_code) + ": " + response);
    }

    auto candles = parse_candles(nlohmann::json::parse(response));
    spdlog::info("Fetched {} candles for {} at {}", candles.size(), symbol, interval);
    return candles;
}

}  // namespace crypto_trader::exchange
