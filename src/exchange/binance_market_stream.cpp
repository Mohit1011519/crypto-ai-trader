#include "exchange/binance_market_stream.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace crypto_trader::exchange {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using TlsWebSocket = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

PriceUpdate parse_trade(const std::string& message) {
    const auto json = nlohmann::json::parse(message);
    if (json.at("e").get<std::string>() != "trade") {
        throw std::runtime_error("Unexpected Binance WebSocket event type");
    }
    return {
        .symbol = json.at("s").get<std::string>(),
        .price = std::stod(json.at("p").get<std::string>()),
        .event_time_ms = json.at("E").get<long long>()
    };
}

void connect_and_read(const common::AppConfig& config,
                      const std::string& symbol,
                      const BinanceMarketStream::PriceHandler& on_price,
                      const std::atomic_bool& stop_requested) {
    asio::io_context io_context;
    asio::ssl::context ssl_context(asio::ssl::context::tlsv12_client);
    ssl_context.set_default_verify_paths();
    ssl_context.set_verify_mode(asio::ssl::verify_peer);

    tcp::resolver resolver(io_context);
    auto endpoints = resolver.resolve(config.websocket_host, config.websocket_port);
    TlsWebSocket stream(io_context, ssl_context);

    stream.next_layer().set_verify_callback(asio::ssl::host_name_verification(config.websocket_host));
    if (!SSL_set_tlsext_host_name(stream.next_layer().native_handle(), config.websocket_host.c_str())) {
        const auto error = static_cast<int>(ERR_get_error());
        throw beast::system_error(
            beast::error_code(error, asio::error::get_ssl_category()),
            "Failed to set WebSocket TLS server name");
    }

    beast::get_lowest_layer(stream).connect(endpoints);
    stream.next_layer().handshake(asio::ssl::stream_base::client);
    stream.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    stream.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& request) {
            request.set(beast::http::field::user_agent, "crypto-ai-trader/0.1.0");
        }));

    const auto path = config.websocket_path + "/" + lowercase(symbol) + "@trade";
    stream.handshake(config.websocket_host + ":" + config.websocket_port, path);
    spdlog::info("Connected to Binance trade stream for {}", symbol);

    beast::flat_buffer buffer;
    while (!stop_requested.load()) {
        stream.read(buffer);
        const auto message = beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());
        on_price(parse_trade(message));
    }

    beast::error_code close_error;
    stream.close(websocket::close_code::normal, close_error);
    if (close_error && close_error != websocket::error::closed) {
        throw beast::system_error(close_error, "Failed to close Binance WebSocket cleanly");
    }
}

}  // namespace

BinanceMarketStream::BinanceMarketStream(common::AppConfig config) : config_(std::move(config)) {}

void BinanceMarketStream::run(const std::string& symbol,
                              const PriceHandler& on_price,
                              const std::atomic_bool& stop_requested) const {
    if (symbol.empty()) {
        throw std::invalid_argument("WebSocket symbol must not be empty");
    }
    if (!on_price) {
        throw std::invalid_argument("WebSocket price handler must be set");
    }

    while (!stop_requested.load()) {
        try {
            connect_and_read(config_, symbol, on_price, stop_requested);
        } catch (const std::exception& error) {
            if (stop_requested.load()) {
                break;
            }
            spdlog::error("Binance WebSocket disconnected: {}", error.what());
        }
        if (!stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(config_.reconnect_delay_seconds));
        }
    }
}

}  // namespace crypto_trader::exchange
