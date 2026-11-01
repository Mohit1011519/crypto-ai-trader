#include "live/live_trader.hpp"

#include "ai/signal_engine.hpp"
#include "market/market_scanner.hpp"
#include "risk/exit_rules.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace crypto_trader::live {
using json = nlohmann::json;

namespace {

std::int64_t current_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string utc_day(const std::int64_t timestamp_ms) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm utc_time{};
#ifdef _WIN32
    if (gmtime_s(&utc_time, &seconds) != 0) {
        throw std::invalid_argument("Unable to convert live timestamp to UTC");
    }
#else
    if (gmtime_r(&seconds, &utc_time) == nullptr) {
        throw std::invalid_argument("Unable to convert live timestamp to UTC");
    }
#endif
    char buffer[11]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &utc_time) == 0) {
        throw std::invalid_argument("Unable to format live UTC date");
    }
    return buffer;
}

bool finite_positive(const double value) {
    return std::isfinite(value) && value > 0.0;
}

}  // namespace

void to_json(json& output, const LivePosition& position) {
    output = {
        {"symbol", position.symbol},
        {"base_asset", position.base_asset},
        {"quantity", position.quantity},
        {"entry_price", position.entry_price},
        {"entry_cost", position.entry_cost},
        {"mark_price", position.mark_price},
        {"initial_stop_loss_price", position.initial_stop_loss_price},
        {"stop_loss_price", position.stop_loss_price},
        {"exchange_stop_price", position.exchange_stop_price},
        {"take_profit_price", position.take_profit_price},
        {"highest_close", position.highest_close},
        {"stop_order_id", position.stop_order_id},
        {"opened_at_ms", position.opened_at_ms}
    };
}

void from_json(const json& input, LivePosition& position) {
    position = {
        .symbol = input.at("symbol").get<std::string>(),
        .base_asset = input.at("base_asset").get<std::string>(),
        .quantity = input.at("quantity").get<double>(),
        .entry_price = input.at("entry_price").get<double>(),
        .entry_cost = input.at("entry_cost").get<double>(),
        .mark_price = input.at("mark_price").get<double>(),
        .initial_stop_loss_price = input.at("initial_stop_loss_price").get<double>(),
        .stop_loss_price = input.at("stop_loss_price").get<double>(),
        .exchange_stop_price = input.at("exchange_stop_price").get<double>(),
        .take_profit_price = input.at("take_profit_price").get<double>(),
        .highest_close = input.at("highest_close").get<double>(),
        .stop_order_id = input.at("stop_order_id").get<std::int64_t>(),
        .opened_at_ms = input.at("opened_at_ms").get<std::int64_t>()
    };
}

void to_json(json& output, const LiveExecution& execution) {
    output = {
        {"id", execution.id},
        {"symbol", execution.symbol},
        {"side", execution.side},
        {"reason", execution.reason},
        {"quantity", execution.quantity},
        {"price", execution.price},
        {"fee", execution.fee},
        {"realized_pnl", execution.realized_pnl},
        {"order_id", execution.order_id},
        {"timestamp_ms", execution.timestamp_ms}
    };
}

void from_json(const json& input, LiveExecution& execution) {
    execution = {
        .id = input.at("id").get<std::uint64_t>(),
        .symbol = input.at("symbol").get<std::string>(),
        .side = input.at("side").get<std::string>(),
        .reason = input.at("reason").get<std::string>(),
        .quantity = input.at("quantity").get<double>(),
        .price = input.at("price").get<double>(),
        .fee = input.at("fee").get<double>(),
        .realized_pnl = input.at("realized_pnl").get<double>(),
        .order_id = input.at("order_id").get<std::int64_t>(),
        .timestamp_ms = input.at("timestamp_ms").get<std::int64_t>()
    };
}

LiveTrader::LiveTrader(common::LiveSettings live_settings,
                       common::RiskSettings risk_settings,
                       execution::SpotVenue& venue)
    : live_settings_(std::move(live_settings)),
      risk_settings_(std::move(risk_settings)),
      venue_(venue) {
    risk::RiskManager validate_risk_settings(risk_settings_);
    if (live_settings_.state_path.empty() || !finite_positive(live_settings_.capital_allocation)) {
        throw std::invalid_argument("Live trader state path or capital allocation is invalid");
    }
    if (std::filesystem::exists(live_settings_.state_path)) {
        std::ifstream input(live_settings_.state_path);
        if (!input) {
            throw std::runtime_error("Unable to open live state: " +
                                     live_settings_.state_path.string());
        }
        load(json::parse(input));
        if (state_.capital_allocation != live_settings_.capital_allocation) {
            spdlog::warn("Live state keeps its original capital allocation {:.2f}; "
                         "configured {:.2f} applies only to a new state file",
                         state_.capital_allocation, live_settings_.capital_allocation);
        }
    } else {
        state_ = {
            .cash_balance = live_settings_.capital_allocation,
            .capital_allocation = live_settings_.capital_allocation,
            .daily_starting_equity = live_settings_.capital_allocation,
            .peak_equity = live_settings_.capital_allocation,
            .utc_day = utc_day(current_time_ms()),
            .trading_halted = false,
            .next_execution_id = 1,
            .next_client_sequence = 1,
            .positions = {},
            .executions = {},
            .last_entry_candle_ms = {}
        };
        persist();
    }
}

double LiveTrader::equity_unlocked() const {
    double equity = state_.cash_balance;
    for (const auto& position : state_.positions) {
        equity += position.quantity * position.mark_price;
    }
    return equity;
}

risk::PortfolioRiskSnapshot LiveTrader::snapshot_unlocked(const double quote_free) const {
    const double equity = equity_unlocked();
    double exposure = 0.0;
    std::vector<std::string> symbols;
    for (const auto& position : state_.positions) {
        exposure += position.quantity * position.mark_price;
        symbols.push_back(position.symbol);
    }
    return {
        .equity = equity,
        .available_balance = std::max(0.0, std::min({state_.cash_balance, quote_free, equity})),
        .gross_exposure = exposure,
        .daily_starting_equity = state_.daily_starting_equity,
        .daily_net_pnl = equity - state_.daily_starting_equity,
        .peak_equity = std::max(state_.peak_equity, equity),
        .open_positions = static_cast<int>(state_.positions.size()),
        .open_symbols = std::move(symbols),
        .utc_day = state_.utc_day
    };
}

std::string LiveTrader::next_client_order_id() {
    // Persisted before use so an id is never reused after a restart.
    const auto sequence = state_.next_client_sequence++;
    persist();
    // The timestamp keeps ids unique even if the state file is ever recreated.
    return "cat-" + std::to_string(sequence) + "-" + std::to_string(current_time_ms());
}

void LiveTrader::record(LiveExecution execution, std::vector<LiveExecution>& executions) {
    execution.id = state_.next_execution_id++;
    state_.executions.push_back(execution);
    executions.push_back(std::move(execution));
}

bool LiveTrader::protect_position(LivePosition& position) {
    try {
        const auto rules = venue_.symbol_rules(position.symbol);
        const auto client_order_id = next_client_order_id();
        const auto order = venue_.place_stop_market_sell(
            position.symbol, position.quantity, position.stop_loss_price, client_order_id);
        position.stop_order_id = order.order_id;
        position.exchange_stop_price =
            execution::floor_to_increment(position.stop_loss_price, rules.tick_size);
        persist();
        spdlog::info("{} protected by exchange stop {} at {}", position.symbol,
                     order.order_id, position.exchange_stop_price);
        return true;
    } catch (const std::exception& error) {
        spdlog::error("Placing exchange stop for {} failed: {}", position.symbol, error.what());
        return false;
    }
}

void LiveTrader::close_position(const std::size_t index,
                                const execution::VenueOrder& order,
                                const std::string& reason,
                                const std::int64_t timestamp_ms,
                                std::vector<LiveExecution>& executions) {
    const auto position = state_.positions[index];
    const double sold = order.executed_quantity;
    const double price = sold > 0.0 ? order.quote_quantity / sold : position.mark_price;
    const double fee = order.quote_commission + order.base_commission * price;
    const double proceeds = order.quote_quantity - fee;
    const double cost_basis = position.entry_cost * std::min(1.0, sold / position.quantity);
    state_.cash_balance += proceeds;
    state_.positions.erase(state_.positions.begin() + static_cast<std::ptrdiff_t>(index));
    record({
        .id = 0,
        .symbol = position.symbol,
        .side = "SELL",
        .reason = reason,
        .quantity = sold,
        .price = price,
        .fee = fee,
        .realized_pnl = proceeds - cost_basis,
        .order_id = order.order_id,
        .timestamp_ms = timestamp_ms
    }, executions);
    persist();
    spdlog::info("SELL {} qty={} price={} reason='{}' pnl={:.2f}", position.symbol, sold,
                 price, reason, proceeds - cost_basis);
}

bool LiveTrader::market_exit(const std::size_t index,
                             const std::string& reason,
                             const std::int64_t timestamp_ms,
                             std::vector<LiveExecution>& executions) {
    auto& position = state_.positions[index];
    try {
        if (position.stop_order_id != 0) {
            const auto cancelled = venue_.cancel_order(position.symbol, position.stop_order_id);
            if (cancelled.state == execution::OrderState::filled) {
                close_position(index, cancelled, "exchange stop-loss", timestamp_ms, executions);
                return true;
            }
            position.stop_order_id = 0;
            persist();
        }
        const auto rules = venue_.symbol_rules(position.symbol);
        const auto held = venue_.balance(position.base_asset);
        const double quantity = execution::floor_to_increment(
            std::min(position.quantity, held.free), rules.step_size);
        if (quantity <= 0.0 || quantity < rules.minimum_quantity) {
            throw std::runtime_error("free " + position.base_asset +
                                     " balance is below the sellable minimum");
        }
        const auto client_order_id = next_client_order_id();
        const auto order = venue_.market_order(
            position.symbol, risk::OrderSide::sell, quantity, client_order_id);
        close_position(index, order, reason, timestamp_ms, executions);
        return true;
    } catch (const std::exception& error) {
        spdlog::critical("Exit of {} failed ({}); new entries are halted", position.symbol,
                         error.what());
        state_.trading_halted = true;
        if (position.stop_order_id == 0) {
            (void)protect_position(position);
        }
        persist();
        return false;
    }
}

bool LiveTrader::manage_position(const std::size_t index,
                                 const market::ScanResult* result,
                                 const std::int64_t timestamp_ms,
                                 std::vector<LiveExecution>& executions) {
    auto& position = state_.positions[index];
    if (position.stop_order_id == 0) {
        if (!protect_position(position)) {
            return market_exit(index, "exchange stop could not be restored",
                               timestamp_ms, executions);
        }
    } else {
        const auto stop = venue_.query_order(position.symbol, position.stop_order_id);
        if (stop.state == execution::OrderState::filled) {
            close_position(index, stop, "exchange stop-loss", timestamp_ms, executions);
            return true;
        }
        if (stop.state != execution::OrderState::open) {
            spdlog::warn("Exchange stop for {} is {}; replacing it", position.symbol,
                         execution::to_string(stop.state));
            position.stop_order_id = 0;
            if (!protect_position(position)) {
                return market_exit(index, "exchange stop could not be restored",
                                   timestamp_ms, executions);
            }
        }
    }

    if (result != nullptr && result->technical && result->technical->atr &&
        finite_positive(result->technical->close)) {
        risk::ProtectiveStopState stop_state{
            .entry_price = position.entry_price,
            .initial_stop_price = position.initial_stop_loss_price,
            .stop_price = position.stop_loss_price,
            .highest_close = position.highest_close
        };
        position.stop_loss_price = risk::advance_protective_stop(
            stop_state, result->technical->close, *result->technical->atr, risk_settings_);
        position.highest_close = stop_state.highest_close;
    }

    const auto top = venue_.book_top(position.symbol);
    position.mark_price = top.bid;
    const bool trailed = position.stop_loss_price > position.initial_stop_loss_price;
    std::string reason;
    if (top.bid <= position.stop_loss_price) {
        reason = trailed ? "trailing stop" : "protective stop";
    } else if (top.bid >= position.take_profit_price) {
        reason = "take-profit";
    } else if (result != nullptr && result->signal &&
               result->signal->action == ai::Signal::sell) {
        reason = "advisory sell signal";
    } else if (result != nullptr && result->signal && result->signal->exit_long) {
        reason = "trend break";
    }
    if (!reason.empty()) {
        return market_exit(index, reason, timestamp_ms, executions);
    }

    if (position.stop_loss_price >
        position.exchange_stop_price * (1.0 + live_settings_.stop_update_minimum_fraction)) {
        const auto cancelled = venue_.cancel_order(position.symbol, position.stop_order_id);
        if (cancelled.state == execution::OrderState::filled) {
            close_position(index, cancelled, "exchange stop-loss", timestamp_ms, executions);
            return true;
        }
        position.stop_order_id = 0;
        persist();
        if (!protect_position(position)) {
            return market_exit(index, "raised stop could not be placed", timestamp_ms, executions);
        }
    }
    persist();
    return false;
}

void LiveTrader::open_position(const market::ScanResult& result,
                               risk::RiskManager& risk_manager,
                               const std::int64_t timestamp_ms,
                               std::vector<LiveExecution>& executions) {
    const auto& technical = *result.technical;
    const auto rules = venue_.symbol_rules(result.symbol);
    if (!rules.stop_market_supported || rules.quote_asset != live_settings_.quote_asset) {
        spdlog::warn("{} skipped: exchange stops unsupported or quote asset mismatch",
                     result.symbol);
        return;
    }
    const auto top = venue_.book_top(result.symbol);
    // ATR is measured on the signal market; express it at the venue price.
    const double atr = *technical.atr / technical.close * top.ask;
    const double spread = (top.ask - top.bid) / ((top.ask + top.bid) / 2.0);
    const auto quote = venue_.balance(live_settings_.quote_asset);
    const auto decision = risk_manager.assess({
        .symbol = result.symbol,
        .side = risk::OrderSide::buy,
        .entry_price = top.ask * (1.0 + risk_settings_.slippage_rate),
        .atr = atr,
        .spread_fraction = spread
    }, snapshot_unlocked(quote.free));
    if (decision.emergency_shutdown) {
        state_.trading_halted = true;
        return;
    }
    if (!decision.approved || !decision.plan) {
        spdlog::info("{} buy not approved: {}", result.symbol,
                     decision.reasons.empty() ? "no reason" : decision.reasons.back());
        return;
    }

    const double quantity = execution::floor_to_increment(
        std::min(decision.plan->position_quantity,
                 live_settings_.maximum_order_notional / top.ask),
        rules.step_size);
    if (quantity <= 0.0 || quantity < rules.minimum_quantity ||
        quantity * top.ask < rules.minimum_notional * 1.02) {
        spdlog::info("{} buy skipped: size {} is below exchange minimums", result.symbol, quantity);
        return;
    }

    const auto client_order_id = next_client_order_id();
    execution::VenueOrder order{};
    try {
        order = venue_.market_order(result.symbol, risk::OrderSide::buy, quantity, client_order_id);
    } catch (const exchange::BinanceApiError& error) {
        spdlog::warn("{} buy rejected: {}", result.symbol, error.what());
        return;
    } catch (const exchange::BinanceUnknownOutcome& error) {
        state_.trading_halted = true;
        persist();
        spdlog::critical("{} buy outcome unknown ({}); trading halted until the account is "
                         "checked manually", result.symbol, error.what());
        return;
    }
    if (order.executed_quantity <= 0.0) {
        return;
    }

    const double fill_price = order.quote_quantity / order.executed_quantity;
    const double held = execution::floor_to_increment(
        order.executed_quantity - order.base_commission, rules.step_size);
    const double fee = order.quote_commission + order.base_commission * fill_price;
    const double cost = order.quote_quantity + order.quote_commission;
    state_.cash_balance = std::max(0.0, state_.cash_balance - cost);
    const double stop_price = fill_price - risk_settings_.stop_atr_multiple * atr;
    state_.positions.push_back({
        .symbol = result.symbol,
        .base_asset = rules.base_asset,
        .quantity = held,
        .entry_price = fill_price,
        .entry_cost = cost,
        .mark_price = fill_price,
        .initial_stop_loss_price = stop_price,
        .stop_loss_price = stop_price,
        .exchange_stop_price = 0.0,
        .take_profit_price = fill_price + risk_settings_.target_atr_multiple * atr,
        .highest_close = fill_price,
        .stop_order_id = 0,
        .opened_at_ms = timestamp_ms
    });
    record({
        .id = 0,
        .symbol = result.symbol,
        .side = "BUY",
        .reason = "risk-approved trend breakout",
        .quantity = held,
        .price = fill_price,
        .fee = fee,
        .realized_pnl = 0.0,
        .order_id = order.order_id,
        .timestamp_ms = timestamp_ms
    }, executions);
    persist();
    spdlog::info("BUY {} qty={} price={} stop={}", result.symbol, held, fill_price, stop_price);

    if (!protect_position(state_.positions.back())) {
        // Never hold an unprotected position: exit immediately and stop trading.
        (void)market_exit(state_.positions.size() - 1, "protective stop rejected",
                          timestamp_ms, executions);
        state_.trading_halted = true;
        persist();
    }
}

std::vector<LiveExecution> LiveTrader::process_scan(
    const std::vector<market::ScanResult>& results,
    risk::RiskManager& risk_manager,
    std::int64_t timestamp_ms) {
    std::lock_guard lock(mutex_);
    if (timestamp_ms == 0) {
        timestamp_ms = current_time_ms();
    }
    if (timestamp_ms < 0) {
        throw std::invalid_argument("Live scan timestamp must not be negative");
    }
    std::set<std::string> symbols;
    for (const auto& result : results) {
        if (result.symbol.empty() || !symbols.insert(result.symbol).second) {
            throw std::invalid_argument("Live scan contains an empty or duplicate market symbol");
        }
    }
    const auto result_for = [&](const std::string& symbol) -> const market::ScanResult* {
        for (const auto& result : results) {
            if (result.symbol == symbol) {
                return &result;
            }
        }
        return nullptr;
    };

    std::vector<LiveExecution> executions;
    for (std::size_t index = 0; index < state_.positions.size();) {
        if (!manage_position(index, result_for(state_.positions[index].symbol),
                             timestamp_ms, executions)) {
            ++index;
        }
    }

    const double equity = equity_unlocked();
    const auto day = utc_day(timestamp_ms);
    if (day != state_.utc_day) {
        state_.utc_day = day;
        state_.daily_starting_equity = equity;
    }
    state_.peak_equity = std::max(state_.peak_equity, equity);

    const auto quote = venue_.balance(live_settings_.quote_asset);
    const auto status = risk_manager.monitor(snapshot_unlocked(quote.free));
    if (status.emergency_shutdown && !state_.trading_halted) {
        spdlog::critical("Risk shutdown: {}", status.reasons.empty()
            ? std::string("limit reached") : status.reasons.front());
        state_.trading_halted = true;
        if (live_settings_.flatten_on_risk_shutdown && !state_.positions.empty()) {
            const auto closed = flatten_unlocked("risk shutdown", timestamp_ms);
            executions.insert(executions.end(), closed.begin(), closed.end());
        }
    }

    for (const auto& result : results) {
        if (state_.trading_halted) {
            break;
        }
        if (!result.signal || result.signal->action != ai::Signal::buy ||
            !result.technical || !result.technical->atr ||
            !finite_positive(result.technical->close) ||
            !finite_positive(*result.technical->atr)) {
            continue;
        }
        if (std::any_of(state_.positions.begin(), state_.positions.end(),
                        [&](const LivePosition& position) {
                            return position.symbol == result.symbol;
                        })) {
            continue;
        }
        const auto candle_ms = result.signal->metadata.timestamp_ms;
        const auto previous = state_.last_entry_candle_ms.find(result.symbol);
        if (previous != state_.last_entry_candle_ms.end() && candle_ms <= previous->second) {
            continue;
        }
        const auto positions_before = state_.positions.size();
        open_position(result, risk_manager, timestamp_ms, executions);
        if (state_.positions.size() > positions_before ||
            (!executions.empty() && executions.back().symbol == result.symbol &&
             executions.back().timestamp_ms == timestamp_ms)) {
            state_.last_entry_candle_ms[result.symbol] = candle_ms;
            persist();
        }
    }

    state_.peak_equity = std::max(state_.peak_equity, equity_unlocked());
    persist();
    return executions;
}

std::vector<LiveExecution> LiveTrader::flatten_unlocked(const std::string& reason,
                                                        const std::int64_t timestamp_ms) {
    std::vector<LiveExecution> executions;
    for (std::size_t index = 0; index < state_.positions.size();) {
        if (!market_exit(index, reason, timestamp_ms, executions)) {
            ++index;
        }
    }
    state_.trading_halted = true;
    persist();
    return executions;
}

std::vector<LiveExecution> LiveTrader::flatten(const std::string& reason,
                                               std::int64_t timestamp_ms) {
    std::lock_guard lock(mutex_);
    return flatten_unlocked(reason, timestamp_ms == 0 ? current_time_ms() : timestamp_ms);
}

void LiveTrader::reset_risk_shutdown(risk::RiskManager& risk_manager) {
    std::lock_guard lock(mutex_);
    const auto quote = venue_.balance(live_settings_.quote_asset);
    risk_manager.reset_emergency_shutdown(snapshot_unlocked(quote.free));
    state_.trading_halted = false;
    persist();
}

LiveAccountView LiveTrader::view() const {
    std::lock_guard lock(mutex_);
    double exposure = 0.0;
    for (const auto& position : state_.positions) {
        exposure += position.quantity * position.mark_price;
    }
    const double equity = equity_unlocked();
    return {
        .cash_balance = state_.cash_balance,
        .capital_allocation = state_.capital_allocation,
        .equity = equity,
        .gross_exposure = exposure,
        .daily_net_pnl = equity - state_.daily_starting_equity,
        .peak_equity = std::max(state_.peak_equity, equity),
        .trading_halted = state_.trading_halted,
        .positions = state_.positions
    };
}

std::vector<LiveExecution> LiveTrader::execution_history() const {
    std::lock_guard lock(mutex_);
    return state_.executions;
}

json LiveTrader::serialize() const {
    return {
        {"version", 1},
        {"cash_balance", state_.cash_balance},
        {"capital_allocation", state_.capital_allocation},
        {"daily_starting_equity", state_.daily_starting_equity},
        {"peak_equity", state_.peak_equity},
        {"utc_day", state_.utc_day},
        {"trading_halted", state_.trading_halted},
        {"next_execution_id", state_.next_execution_id},
        {"next_client_sequence", state_.next_client_sequence},
        {"positions", state_.positions},
        {"executions", state_.executions},
        {"last_entry_candle_ms", state_.last_entry_candle_ms}
    };
}

void LiveTrader::load(const json& input) {
    if (input.at("version").get<int>() != 1) {
        throw std::invalid_argument("Unsupported live state version");
    }
    State state{
        .cash_balance = input.at("cash_balance").get<double>(),
        .capital_allocation = input.at("capital_allocation").get<double>(),
        .daily_starting_equity = input.at("daily_starting_equity").get<double>(),
        .peak_equity = input.at("peak_equity").get<double>(),
        .utc_day = input.at("utc_day").get<std::string>(),
        .trading_halted = input.at("trading_halted").get<bool>(),
        .next_execution_id = input.at("next_execution_id").get<std::uint64_t>(),
        .next_client_sequence = input.at("next_client_sequence").get<std::uint64_t>(),
        .positions = input.at("positions").get<std::vector<LivePosition>>(),
        .executions = input.at("executions").get<std::vector<LiveExecution>>(),
        .last_entry_candle_ms = input.value(
            "last_entry_candle_ms", std::map<std::string, std::int64_t>{})
    };
    if (!std::isfinite(state.cash_balance) || state.cash_balance < 0.0 ||
        !finite_positive(state.capital_allocation) ||
        !finite_positive(state.daily_starting_equity) ||
        !finite_positive(state.peak_equity) || state.utc_day.size() != 10 ||
        state.next_execution_id == 0 || state.next_client_sequence == 0) {
        throw std::invalid_argument("Persisted live state contains invalid account values");
    }
    std::set<std::string> symbols;
    for (const auto& position : state.positions) {
        if (position.symbol.empty() || position.base_asset.empty() ||
            !symbols.insert(position.symbol).second ||
            !finite_positive(position.quantity) || !finite_positive(position.entry_price) ||
            !finite_positive(position.entry_cost) || !finite_positive(position.mark_price) ||
            !finite_positive(position.initial_stop_loss_price) ||
            position.initial_stop_loss_price >= position.entry_price ||
            position.stop_loss_price < position.initial_stop_loss_price ||
            !finite_positive(position.take_profit_price) ||
            !finite_positive(position.highest_close) || position.stop_order_id < 0) {
            throw std::invalid_argument("Persisted live state contains an invalid position");
        }
    }
    state_ = std::move(state);
}

void LiveTrader::persist() const {
    const auto& path = live_settings_.state_path;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    auto temporary_path = path;
    temporary_path += ".tmp";
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Unable to create live state: " + temporary_path.string());
        }
        output << serialize().dump(2) << '\n';
        output.flush();
        if (!output) {
            throw std::runtime_error("Unable to write live state: " + temporary_path.string());
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary_path.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to atomically replace live state");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary_path, path, error);
    if (error) {
        throw std::system_error(error, "Unable to atomically replace live state");
    }
#endif
}

}  // namespace crypto_trader::live
