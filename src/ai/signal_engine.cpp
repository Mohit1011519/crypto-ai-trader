#include "ai/signal_engine.hpp"

#include "market/market_scanner.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace crypto_trader::ai {
namespace {

constexpr double trend_strength_reference = 0.01;
constexpr double confirmation_threshold = 0.15;

double clamp_score(const double score) {
    return std::clamp(score, -1.0, 1.0);
}

int direction(const double score) {
    if (score > confirmation_threshold) {
        return 1;
    }
    if (score < -confirmation_threshold) {
        return -1;
    }
    return 0;
}

void add_component(std::vector<SignalComponent>& components,
                   const char* name,
                   const double score,
                   const double weight) {
    if (weight > 0.0) {
        components.push_back({name, clamp_score(score), weight});
    }
}

void add_reason(std::vector<std::string>& reasons, std::string reason) {
    reasons.push_back(std::move(reason));
}

}  // namespace

SignalEngine::SignalEngine(common::SignalSettings settings) : settings_(std::move(settings)) {}

TradingSignal SignalEngine::evaluate(const market::ScanResult& research) {
    TradingSignal result{
        .metadata = {
            .timestamp_ms = 0,
            .sample_count = 0,
            .quality = 0.0
        },
        .action = Signal::hold,
        .research_score = 0.0,
        .confidence = 0.0,
        .research_coverage = 0.0,
        .confirmations = 0,
        .expected_reward_risk_ratio = std::nullopt,
        .estimated_round_trip_cost_percent = 0.0,
        .components = {},
        .reasons = {}
    };

    if (research.candle_error || !research.technical || !research.trend ||
        !research.volume || !research.volatility) {
        add_reason(result.reasons, research.candle_error
            ? "Market research failed: " + *research.candle_error
            : "Required market research components are unavailable");
        return result;
    }

    const auto& technical = *research.technical;
    const auto& trend = *research.trend;
    const auto& volume = *research.volume;
    const auto& volatility = *research.volatility;
    result.metadata.timestamp_ms = std::max({
        technical.metadata.timestamp_ms,
        trend.metadata.timestamp_ms,
        volume.metadata.timestamp_ms,
        volatility.metadata.timestamp_ms
    });
    result.metadata.sample_count = std::min({
        technical.metadata.sample_count,
        trend.metadata.sample_count,
        volume.metadata.sample_count,
        volatility.metadata.sample_count
    });
    result.metadata.quality = std::min({
        technical.metadata.quality,
        trend.metadata.quality,
        volume.metadata.quality,
        volatility.metadata.quality
    });

    if (!std::isfinite(result.metadata.quality) ||
        result.metadata.quality < settings_.minimum_research_quality) {
        add_reason(result.reasons, "Research quality is below the configured minimum");
        return result;
    }
    if (!std::isfinite(trend.strength) || trend.strength < 0.0 ||
        (technical.rsi && (!std::isfinite(*technical.rsi) ||
                           *technical.rsi < 0.0 || *technical.rsi > 100.0)) ||
        (technical.macd_histogram && !std::isfinite(*technical.macd_histogram)) ||
        (technical.atr && !std::isfinite(*technical.atr)) ||
        !std::isfinite(volume.buy_pressure) ||
        !std::isfinite(volume.sell_pressure) ||
        volume.buy_pressure < 0.0 || volume.buy_pressure > 1.0 ||
        volume.sell_pressure < 0.0 || volume.sell_pressure > 1.0 ||
        (volume.volume_ratio && (!std::isfinite(*volume.volume_ratio) ||
                                 *volume.volume_ratio < 0.0)) ||
        (volume.average_volume && (!std::isfinite(*volume.average_volume) ||
                                   *volume.average_volume < 0.0))) {
        add_reason(result.reasons, "Research contains invalid numeric values");
        return result;
    }

    // Long exits use the medium EMA: a fast-EMA cross below it with price
    // underneath means the uptrend that justified the entry has broken.
    result.exit_long = technical.ema_fast && technical.ema_medium &&
        *technical.ema_fast < *technical.ema_medium &&
        technical.close < *technical.ema_medium;

    double trend_score = 0.0;
    bool trend_available = false;
    switch (trend.direction) {
    case market::TrendDirection::bullish:
        trend_score = std::min(1.0, trend.strength / trend_strength_reference);
        trend_available = true;
        break;
    case market::TrendDirection::bearish:
        trend_score = -std::min(1.0, trend.strength / trend_strength_reference);
        trend_available = true;
        break;
    case market::TrendDirection::sideways:
        trend_available = true;
        break;
    case market::TrendDirection::unavailable:
        break;
    }
    if (trend_available) {
        add_component(result.components, "trend", trend_score, settings_.trend_weight);
    }

    if (technical.rsi && technical.macd_histogram &&
        technical.close > 0.0 && std::isfinite(technical.close)) {
        const double rsi_score = std::clamp((*technical.rsi - 50.0) / 25.0, -1.0, 1.0);
        const double macd_relative = *technical.macd_histogram / technical.close;
        const double macd_score = std::tanh(macd_relative * 500.0);
        add_component(
            result.components, "momentum",
            (rsi_score + macd_score) / 2.0, settings_.momentum_weight);
    }

    if (volume.average_volume && volume.volume_ratio &&
        std::isfinite(volume.buy_pressure) && std::isfinite(volume.sell_pressure)) {
        const double activity = std::clamp(*volume.volume_ratio, 0.0, 1.0);
        const double pressure = volume.buy_pressure - volume.sell_pressure;
        add_component(result.components, "volume", pressure * activity, settings_.volume_weight);
    }

    const double volatility_penalty =
        volatility.risk == market::VolatilityRisk::elevated ? 0.5 :
        volatility.risk == market::VolatilityRisk::high ? 1.0 :
        volatility.risk == market::VolatilityRisk::unavailable ? 1.0 : 0.0;
    const double volatility_score = 1.0 - volatility_penalty;
    if (volatility.risk != market::VolatilityRisk::unavailable) {
        add_component(
            result.components, "volatility_regime",
            volatility_score, settings_.volatility_weight);
    }

    if (research.order_book) {
        const auto& book = *research.order_book;
        if (!std::isfinite(book.imbalance) || !std::isfinite(book.spread_percent) ||
            book.imbalance < -1.0 || book.imbalance > 1.0 ||
            book.spread_percent < 0.0 ||
            !std::isfinite(book.metadata.quality) || book.metadata.quality < 0.0) {
            add_reason(result.reasons, "Order-book research contains invalid numeric values");
            return result;
        }
        add_component(
            result.components, "order_book",
            book.imbalance, settings_.order_book_weight);
        if (book.liquidity != market::LiquidityLevel::sufficient) {
            add_reason(result.reasons, "Order-book quote liquidity is below the configured minimum");
            return result;
        }
        if (book.spread_percent > settings_.maximum_spread_percent) {
            add_reason(result.reasons, "Order-book spread exceeds the configured maximum");
            return result;
        }
        if (book.metadata.quality < settings_.minimum_research_quality) {
            add_reason(result.reasons, "Order-book depth is insufficient for the configured quality");
            return result;
        }
    } else if (research.order_book_error) {
        add_reason(result.reasons, "Order book unavailable: " + *research.order_book_error);
    }

    double directional_weight = 0.0;
    double weighted_directional_score = 0.0;
    for (const auto& component : result.components) {
        result.research_coverage += component.weight;
        if (component.name == "volatility_regime") {
            continue;
        }
        directional_weight += component.weight;
        weighted_directional_score += component.score * component.weight;
        if (direction(component.score) != 0) {
            ++result.confirmations;
        }
    }
    if (directional_weight > 0.0) {
        result.research_score = clamp_score(
            weighted_directional_score / directional_weight *
            (1.0 - settings_.volatility_weight * volatility_penalty));
    }
    result.confidence = std::abs(result.research_score);

    if (result.research_coverage < settings_.minimum_research_coverage) {
        add_reason(result.reasons, "Available research does not meet minimum weighted coverage");
        return result;
    }
    if (volatility.risk == market::VolatilityRisk::unavailable) {
        add_reason(result.reasons, "Volatility regime is unavailable");
        return result;
    }
    if (volatility.risk == market::VolatilityRisk::high) {
        add_reason(result.reasons, "High-volatility conditions are not eligible for a signal");
        return result;
    }
    if (volatility.sudden_volatility) {
        add_reason(result.reasons, "Sudden volatility expansion makes the market unreliable");
        return result;
    }

    if (!technical.atr || !std::isfinite(*technical.atr) ||
        !std::isfinite(technical.close) || *technical.atr <= 0.0 ||
        technical.close <= 0.0) {
        add_reason(result.reasons, "ATR-based risk/reward is unavailable");
        return result;
    }

    const double spread_cost = research.order_book
        ? research.order_book->spread_percent : settings_.maximum_spread_percent;
    result.estimated_round_trip_cost_percent =
        2.0 * (settings_.taker_fee_rate + settings_.slippage_rate) + spread_cost;
    const double round_trip_cost = technical.close *
        result.estimated_round_trip_cost_percent;
    const double net_reward =
        *technical.atr * settings_.target_atr_multiple - round_trip_cost;
    const double net_risk =
        *technical.atr * settings_.stop_atr_multiple + round_trip_cost;
    if (net_reward <= 0.0 || net_risk <= 0.0) {
        add_reason(result.reasons, "Estimated fees and slippage consume the expected reward");
        return result;
    }
    result.expected_reward_risk_ratio = net_reward / net_risk;
    if (*result.expected_reward_risk_ratio < settings_.minimum_reward_risk_ratio) {
        add_reason(result.reasons, "Net expected reward/risk is below the configured minimum");
        return result;
    }

    if (result.confirmations < settings_.minimum_confirmations) {
        add_reason(result.reasons, "Too few independent research components confirm direction");
        return result;
    }
    if (result.confidence < settings_.minimum_signal_score) {
        add_reason(result.reasons, "Research score is below the configured signal threshold");
        return result;
    }

    if (result.research_score > 0.0 && settings_.require_bullish_trend_for_buy &&
        trend.direction != market::TrendDirection::bullish) {
        add_reason(result.reasons, "Buy requires an aligned bullish EMA trend");
        return result;
    }
    if (result.research_score > 0.0 && settings_.require_breakout_for_buy &&
        !trend.breakout_up) {
        add_reason(result.reasons, "Buy requires a close above the prior range high");
        return result;
    }

    result.action = result.research_score > 0.0 ? Signal::buy : Signal::sell;
    add_reason(result.reasons, "Weighted, multi-component research supports this direction");
    add_reason(result.reasons, research.order_book
        ? "Reward/risk estimate includes configured taker fees, slippage, and observed spread"
        : "Reward/risk estimate includes configured taker fees, slippage, and maximum allowed spread");
    add_reason(result.reasons, "Signal is informational; no position state or order execution is managed");

    {
        std::lock_guard lock(mutex_);
        const auto previous = last_emitted_signal_.find(research.symbol);
        const bool repeated = previous != last_emitted_signal_.end() &&
            previous->second.action == result.action &&
            (settings_.suppress_repeated_direction ||
             previous->second.timestamp_ms == result.metadata.timestamp_ms);
        if (repeated) {
            result.action = Signal::hold;
            add_reason(result.reasons, settings_.suppress_repeated_direction
                ? "Repeated signal suppressed; existing positions are not tracked"
                : "Signal already emitted for this closed candle");
        } else {
            last_emitted_signal_.insert_or_assign(
                research.symbol, EmittedSignal{result.action, result.metadata.timestamp_ms});
        }
    }

    return result;
}

const char* to_string(const Signal signal) noexcept {
    switch (signal) {
    case Signal::buy: return "BUY";
    case Signal::sell: return "SELL";
    case Signal::hold: return "HOLD";
    }
    return "HOLD";
}

}  // namespace crypto_trader::ai
