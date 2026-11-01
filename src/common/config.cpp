#include "common/config.hpp"

#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace crypto_trader::common {
namespace {

void require_non_empty(const std::string& value, const char* name) {
    if (value.empty()) {
        throw std::invalid_argument(std::string("Configuration field '") + name + "' must not be empty");
    }
}

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

}  // namespace

AppConfig parse_config(const nlohmann::json& json) {
    const auto& research = json.at("research");
    const auto& signals = json.at("signals");
    const auto& weights = signals.at("weights");
    const auto& risk = json.at("risk");
    AppConfig config{
        .exchange = json.at("exchange").get<std::string>(),
        .mode = json.at("mode").get<std::string>(),
        .rest_base_url = json.at("rest_base_url").get<std::string>(),
        .websocket_host = json.at("websocket_host").get<std::string>(),
        .websocket_port = json.at("websocket_port").get<std::string>(),
        .websocket_path = json.at("websocket_path").get<std::string>(),
        .trading_pairs = json.at("trading_pairs").get<std::vector<std::string>>(),
        .candle_interval = json.at("candle_interval").get<std::string>(),
        .candle_limit = json.at("candle_limit").get<int>(),
        .request_timeout_seconds = json.at("request_timeout_seconds").get<int>(),
        .reconnect_delay_seconds = json.at("reconnect_delay_seconds").get<int>(),
        .log_level = json.at("log_level").get<std::string>(),
        .research = ResearchSettings{
            .ema_fast_period = research.at("ema_fast_period").get<int>(),
            .ema_medium_period = research.at("ema_medium_period").get<int>(),
            .ema_slow_period = research.at("ema_slow_period").get<int>(),
            .rsi_period = research.at("rsi_period").get<int>(),
            .macd_fast_period = research.at("macd_fast_period").get<int>(),
            .macd_slow_period = research.at("macd_slow_period").get<int>(),
            .macd_signal_period = research.at("macd_signal_period").get<int>(),
            .bollinger_period = research.at("bollinger_period").get<int>(),
            .bollinger_standard_deviations = research.at("bollinger_standard_deviations").get<double>(),
            .atr_period = research.at("atr_period").get<int>(),
            .stochastic_rsi_period = research.at("stochastic_rsi_period").get<int>(),
            .volume_average_period = research.at("volume_average_period").get<int>(),
            .volume_spike_multiple = research.at("volume_spike_multiple").get<double>(),
            .support_resistance_period = research.at("support_resistance_period").get<int>(),
            .historical_volatility_period = research.at("historical_volatility_period").get<int>(),
            .normal_volatility_threshold = research.at("normal_volatility_threshold").get<double>(),
            .high_volatility_threshold = research.at("high_volatility_threshold").get<double>(),
            .sudden_volatility_multiple = research.at("sudden_volatility_multiple").get<double>(),
            .order_book_depth = research.at("order_book_depth").get<int>(),
            .large_order_multiple = research.at("large_order_multiple").get<double>(),
            .minimum_quote_liquidity = research.at("minimum_quote_liquidity").get<double>(),
            .scan_interval_seconds = research.at("scan_interval_seconds").get<int>()
        },
        .signals = SignalSettings{
            .trend_weight = weights.at("trend").get<double>(),
            .momentum_weight = weights.at("momentum").get<double>(),
            .volume_weight = weights.at("volume").get<double>(),
            .volatility_weight = weights.at("volatility").get<double>(),
            .order_book_weight = weights.at("order_book").get<double>(),
            .sentiment_weight = weights.at("sentiment").get<double>(),
            .minimum_signal_score = signals.at("minimum_signal_score").get<double>(),
            .minimum_research_quality = signals.at("minimum_research_quality").get<double>(),
            .minimum_research_coverage = signals.at("minimum_research_coverage").get<double>(),
            .minimum_confirmations = signals.at("minimum_confirmations").get<int>(),
            .minimum_reward_risk_ratio = signals.at("minimum_reward_risk_ratio").get<double>(),
            .target_atr_multiple = signals.at("target_atr_multiple").get<double>(),
            .stop_atr_multiple = signals.at("stop_atr_multiple").get<double>(),
            .taker_fee_rate = signals.at("taker_fee_rate").get<double>(),
            .slippage_rate = signals.at("slippage_rate").get<double>(),
            .maximum_spread_percent = signals.at("maximum_spread_percent").get<double>()
        },
        .risk = RiskSettings{
            .risk_per_trade_fraction = risk.at("risk_per_trade_fraction").get<double>(),
            .maximum_open_positions = risk.at("maximum_open_positions").get<int>(),
            .daily_maximum_loss_fraction = risk.at("daily_maximum_loss_fraction").get<double>(),
            .maximum_drawdown_fraction = risk.at("maximum_drawdown_fraction").get<double>(),
            .maximum_gross_exposure_fraction = risk.at("maximum_gross_exposure_fraction").get<double>(),
            .stop_atr_multiple = risk.at("stop_atr_multiple").get<double>(),
            .target_atr_multiple = risk.at("target_atr_multiple").get<double>(),
            .taker_fee_rate = risk.at("taker_fee_rate").get<double>(),
            .slippage_rate = risk.at("slippage_rate").get<double>()
        },
        .paper_initial_balance = json.at("paper_initial_balance").get<double>(),
        .paper_state_path = json.at("paper_state_path").get<std::string>(),
        .live_order_submission_enabled =
            json.at("live_order_submission_enabled").get<bool>()
    };

    require_non_empty(config.exchange, "exchange");
    require_non_empty(config.mode, "mode");
    require_non_empty(config.rest_base_url, "rest_base_url");
    require_non_empty(config.websocket_host, "websocket_host");
    require_non_empty(config.websocket_port, "websocket_port");
    require_non_empty(config.websocket_path, "websocket_path");
    require_non_empty(config.candle_interval, "candle_interval");
    require_non_empty(config.log_level, "log_level");

    if (config.exchange != "binance") {
        throw std::invalid_argument("Only the 'binance' exchange is supported in Phase 1");
    }
    if (config.mode != "paper") {
        throw std::invalid_argument("Phase 1 supports only paper mode; live trading is not implemented");
    }
    if (config.trading_pairs.empty()) {
        throw std::invalid_argument("Configuration field 'trading_pairs' must contain at least one pair");
    }
    std::set<std::string> unique_pairs;
    for (const auto& pair : config.trading_pairs) {
        if (!valid_symbol(pair)) {
            throw std::invalid_argument("Trading pairs must contain only uppercase letters and digits: " + pair);
        }
        if (!unique_pairs.insert(pair).second) {
            throw std::invalid_argument("Duplicate trading pair in configuration: " + pair);
        }
    }
    if (config.candle_limit < 1 || config.candle_limit > 1000) {
        throw std::invalid_argument("candle_limit must be between 1 and 1000");
    }
    if (config.request_timeout_seconds < 1) {
        throw std::invalid_argument("request_timeout_seconds must be positive");
    }
    if (config.reconnect_delay_seconds < 1) {
        throw std::invalid_argument("reconnect_delay_seconds must be positive");
    }
    if (config.websocket_port.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("websocket_port must be numeric");
    }
    const auto port = std::stoul(config.websocket_port);
    if (port == 0 || port > std::numeric_limits<unsigned short>::max()) {
        throw std::invalid_argument("websocket_port must be between 1 and 65535");
    }
    if (config.websocket_path.front() != '/') {
        throw std::invalid_argument("websocket_path must begin with '/'");
    }
    if (config.websocket_path.size() > 1 && config.websocket_path.back() == '/') {
        throw std::invalid_argument("websocket_path must not end with '/'");
    }
    constexpr std::string_view https_prefix = "https://";
    if (config.rest_base_url.compare(0, https_prefix.size(), https_prefix) != 0) {
        throw std::invalid_argument("rest_base_url must use HTTPS");
    }
    while (config.rest_base_url.size() > https_prefix.size() &&
           config.rest_base_url.back() == '/') {
        config.rest_base_url.pop_back();
    }

    const auto& settings = config.research;
    if (settings.ema_fast_period < 1 ||
        settings.ema_medium_period <= settings.ema_fast_period ||
        settings.ema_slow_period <= settings.ema_medium_period ||
        settings.rsi_period < 2 ||
        settings.macd_fast_period < 1 ||
        settings.macd_slow_period <= settings.macd_fast_period ||
        settings.macd_signal_period < 1 ||
        settings.bollinger_period < 2 ||
        settings.bollinger_standard_deviations <= 0.0 ||
        settings.atr_period < 1 ||
        settings.stochastic_rsi_period < 2 ||
        settings.volume_average_period < 1 ||
        settings.volume_spike_multiple <= 0.0 ||
        settings.support_resistance_period < 2 ||
        settings.historical_volatility_period < 2 ||
        settings.normal_volatility_threshold <= 0.0 ||
        settings.high_volatility_threshold <= settings.normal_volatility_threshold ||
        settings.sudden_volatility_multiple <= 0.0 ||
        (settings.order_book_depth != 5 && settings.order_book_depth != 10 &&
         settings.order_book_depth != 20 && settings.order_book_depth != 50 &&
         settings.order_book_depth != 100 && settings.order_book_depth != 500 &&
         settings.order_book_depth != 1000) ||
        settings.large_order_multiple <= 0.0 ||
        settings.minimum_quote_liquidity < 0.0 ||
        settings.scan_interval_seconds < 1) {
        throw std::invalid_argument("Research settings contain invalid periods, thresholds, or limits");
    }
    if (!std::isfinite(settings.bollinger_standard_deviations) ||
        !std::isfinite(settings.volume_spike_multiple) ||
        !std::isfinite(settings.normal_volatility_threshold) ||
        !std::isfinite(settings.high_volatility_threshold) ||
        !std::isfinite(settings.sudden_volatility_multiple) ||
        !std::isfinite(settings.large_order_multiple) ||
        !std::isfinite(settings.minimum_quote_liquidity)) {
        throw std::invalid_argument("Research thresholds must be finite numbers");
    }

    const auto& signal_settings = config.signals;
    const double weight_sum = signal_settings.trend_weight +
        signal_settings.momentum_weight + signal_settings.volume_weight +
        signal_settings.volatility_weight + signal_settings.order_book_weight +
        signal_settings.sentiment_weight;
    if (signal_settings.trend_weight < 0.0 ||
        signal_settings.momentum_weight < 0.0 ||
        signal_settings.volume_weight < 0.0 ||
        signal_settings.volatility_weight < 0.0 ||
        signal_settings.order_book_weight < 0.0 ||
        signal_settings.sentiment_weight < 0.0 ||
        !std::isfinite(weight_sum) || std::abs(weight_sum - 1.0) > 1e-6 ||
        signal_settings.minimum_signal_score <= 0.0 ||
        signal_settings.minimum_signal_score > 1.0 ||
        signal_settings.minimum_research_quality <= 0.0 ||
        signal_settings.minimum_research_quality > 1.0 ||
        signal_settings.minimum_research_coverage <= 0.0 ||
        signal_settings.minimum_research_coverage > 1.0 ||
        signal_settings.minimum_confirmations < 2 ||
        signal_settings.minimum_reward_risk_ratio <= 0.0 ||
        signal_settings.target_atr_multiple <= 0.0 ||
        signal_settings.stop_atr_multiple <= 0.0 ||
        signal_settings.taker_fee_rate < 0.0 ||
        signal_settings.slippage_rate < 0.0 ||
        signal_settings.maximum_spread_percent <= 0.0 ||
        signal_settings.maximum_spread_percent > 1.0 ||
        signal_settings.taker_fee_rate >= 1.0 ||
        signal_settings.slippage_rate >= 1.0) {
        throw std::invalid_argument("Signal settings contain invalid weights, thresholds, or costs");
    }
    if (!std::isfinite(signal_settings.trend_weight) ||
        !std::isfinite(signal_settings.momentum_weight) ||
        !std::isfinite(signal_settings.volume_weight) ||
        !std::isfinite(signal_settings.volatility_weight) ||
        !std::isfinite(signal_settings.order_book_weight) ||
        !std::isfinite(signal_settings.sentiment_weight) ||
        !std::isfinite(signal_settings.minimum_signal_score) ||
        !std::isfinite(signal_settings.minimum_research_quality) ||
        !std::isfinite(signal_settings.minimum_research_coverage) ||
        !std::isfinite(signal_settings.minimum_reward_risk_ratio) ||
        !std::isfinite(signal_settings.target_atr_multiple) ||
        !std::isfinite(signal_settings.stop_atr_multiple) ||
        !std::isfinite(signal_settings.taker_fee_rate) ||
        !std::isfinite(signal_settings.slippage_rate) ||
        !std::isfinite(signal_settings.maximum_spread_percent)) {
        throw std::invalid_argument("Signal settings must be finite numbers");
    }

    const auto& risk_settings = config.risk;
    if (risk_settings.risk_per_trade_fraction <= 0.0 ||
        risk_settings.risk_per_trade_fraction > 0.05 ||
        risk_settings.maximum_open_positions < 1 ||
        risk_settings.daily_maximum_loss_fraction <= 0.0 ||
        risk_settings.daily_maximum_loss_fraction > 0.25 ||
        risk_settings.maximum_drawdown_fraction <= 0.0 ||
        risk_settings.maximum_drawdown_fraction > 0.5 ||
        risk_settings.maximum_gross_exposure_fraction <= 0.0 ||
        risk_settings.maximum_gross_exposure_fraction > 1.0 ||
        risk_settings.stop_atr_multiple <= 0.0 ||
        risk_settings.target_atr_multiple <= 0.0 ||
        risk_settings.taker_fee_rate < 0.0 || risk_settings.taker_fee_rate >= 1.0 ||
        risk_settings.slippage_rate < 0.0 || risk_settings.slippage_rate >= 1.0) {
        throw std::invalid_argument("Risk settings contain invalid limits or costs");
    }
    if (!std::isfinite(risk_settings.risk_per_trade_fraction) ||
        !std::isfinite(risk_settings.daily_maximum_loss_fraction) ||
        !std::isfinite(risk_settings.maximum_drawdown_fraction) ||
        !std::isfinite(risk_settings.maximum_gross_exposure_fraction) ||
        !std::isfinite(risk_settings.stop_atr_multiple) ||
        !std::isfinite(risk_settings.target_atr_multiple) ||
        !std::isfinite(risk_settings.taker_fee_rate) ||
        !std::isfinite(risk_settings.slippage_rate)) {
        throw std::invalid_argument("Risk settings must be finite numbers");
    }
    if (!std::isfinite(config.paper_initial_balance) ||
        config.paper_initial_balance <= 0.0 ||
        config.paper_state_path.empty()) {
        throw std::invalid_argument("Paper portfolio settings require a positive initial balance and state path");
    }
    if (config.live_order_submission_enabled) {
        throw std::invalid_argument(
            "Live order submission is unavailable; live_order_submission_enabled must remain false");
    }

    return config;
}

AppConfig load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Unable to open configuration file: " + path.string());
    }

    nlohmann::json json;
    input >> json;
    if (!json.is_object()) {
        throw std::invalid_argument("Configuration root must be a JSON object");
    }
    return parse_config(json);
}

}  // namespace crypto_trader::common
