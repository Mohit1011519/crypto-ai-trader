#include "paper/paper_portfolio.hpp"

#include "ai/signal_engine.hpp"
#include "market/market_scanner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace crypto_trader::paper {
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
        throw std::invalid_argument("Unable to convert paper portfolio timestamp to UTC");
    }
#else
    if (gmtime_r(&seconds, &utc_time) == nullptr) {
        throw std::invalid_argument("Unable to convert paper portfolio timestamp to UTC");
    }
#endif
    char buffer[11]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &utc_time) == 0) {
        throw std::invalid_argument("Unable to format paper portfolio UTC date");
    }
    return buffer;
}

double equity_of(const double cash, const std::vector<PaperPosition>& positions) {
    double equity = cash;
    for (const auto& position : positions) {
        equity += position.quantity * position.mark_price;
    }
    if (!std::isfinite(equity) || equity <= 0.0) {
        throw std::invalid_argument("Paper portfolio equity is not finite and positive");
    }
    return equity;
}

double exposure_of(const std::vector<PaperPosition>& positions) {
    double exposure = 0.0;
    for (const auto& position : positions) {
        exposure += position.quantity * position.mark_price;
    }
    if (!std::isfinite(exposure) || exposure < 0.0) {
        throw std::invalid_argument("Paper portfolio exposure is invalid");
    }
    return exposure;
}

bool valid_position(const PaperPosition& position) {
    return !position.symbol.empty() &&
        std::isfinite(position.quantity) && position.quantity > 0.0 &&
        std::isfinite(position.entry_price) && position.entry_price > 0.0 &&
        std::isfinite(position.mark_price) && position.mark_price > 0.0 &&
        std::isfinite(position.stop_loss_price) && position.stop_loss_price > 0.0 &&
        std::isfinite(position.take_profit_price) &&
        position.take_profit_price > position.entry_price &&
        std::isfinite(position.entry_fee) && position.entry_fee >= 0.0 &&
        position.opened_at_ms > 0;
}

}  // namespace

void to_json(json& output, const PaperPosition& position) {
    output = {
        {"symbol", position.symbol},
        {"quantity", position.quantity},
        {"entry_price", position.entry_price},
        {"mark_price", position.mark_price},
        {"stop_loss_price", position.stop_loss_price},
        {"take_profit_price", position.take_profit_price},
        {"entry_fee", position.entry_fee},
        {"opened_at_ms", position.opened_at_ms}
    };
}

void from_json(const json& input, PaperPosition& position) {
    position = {
        .symbol = input.at("symbol").get<std::string>(),
        .quantity = input.at("quantity").get<double>(),
        .entry_price = input.at("entry_price").get<double>(),
        .mark_price = input.at("mark_price").get<double>(),
        .stop_loss_price = input.at("stop_loss_price").get<double>(),
        .take_profit_price = input.at("take_profit_price").get<double>(),
        .entry_fee = input.at("entry_fee").get<double>(),
        .opened_at_ms = input.at("opened_at_ms").get<std::int64_t>()
    };
}

void to_json(json& output, const PaperExecution& execution) {
    output = {
        {"id", execution.id},
        {"symbol", execution.symbol},
        {"side", execution.side},
        {"reason", execution.reason},
        {"quantity", execution.quantity},
        {"price", execution.price},
        {"fee", execution.fee},
        {"realized_pnl", execution.realized_pnl},
        {"timestamp_ms", execution.timestamp_ms}
    };
}

void from_json(const json& input, PaperExecution& execution) {
    execution = {
        .id = input.at("id").get<std::uint64_t>(),
        .symbol = input.at("symbol").get<std::string>(),
        .side = input.at("side").get<std::string>(),
        .reason = input.at("reason").get<std::string>(),
        .quantity = input.at("quantity").get<double>(),
        .price = input.at("price").get<double>(),
        .fee = input.at("fee").get<double>(),
        .realized_pnl = input.at("realized_pnl").get<double>(),
        .timestamp_ms = input.at("timestamp_ms").get<std::int64_t>()
    };
}

PaperPortfolio::PaperPortfolio(std::filesystem::path state_path,
                               const double initial_balance,
                               common::RiskSettings risk_settings,
                               const double fallback_spread_fraction)
    : state_path_(std::move(state_path)),
      initial_balance_(initial_balance),
      risk_settings_(std::move(risk_settings)),
      fallback_spread_fraction_(fallback_spread_fraction) {
    if (state_path_.empty() || !std::isfinite(initial_balance_) ||
        initial_balance_ <= 0.0 ||
        !std::isfinite(fallback_spread_fraction_) ||
        fallback_spread_fraction_ < 0.0 || fallback_spread_fraction_ >= 1.0) {
        throw std::invalid_argument("Paper portfolio path, initial balance, or fallback spread is invalid");
    }
    risk::RiskManager validate_risk_settings(risk_settings_);

    if (std::filesystem::exists(state_path_)) {
        std::ifstream input(state_path_);
        if (!input) {
            throw std::runtime_error("Unable to open paper portfolio state: " + state_path_.string());
        }
        const auto persisted = json::parse(input);
        state_ = deserialize_state(persisted);
        initial_balance_ = state_.initial_balance;
    } else {
        const auto timestamp = current_time_ms();
        state_ = {
            .cash_balance = initial_balance_,
            .initial_balance = initial_balance_,
            .daily_starting_equity = initial_balance_,
            .peak_equity = initial_balance_,
            .utc_day = utc_day(timestamp),
            .trading_halted = false,
            .next_execution_id = 1,
            .positions = {},
            .executions = {}
        };
        save_state(state_);
    }
}

PaperPortfolioView PaperPortfolio::view_unlocked(const State& state) const {
    const double equity = equity_of(state.cash_balance, state.positions);
    double unrealized_pnl = 0.0;
    for (const auto& position : state.positions) {
        unrealized_pnl += position.quantity * (position.mark_price - position.entry_price) -
            position.entry_fee;
    }
    return {
        .cash_balance = state.cash_balance,
        .equity = equity,
        .gross_exposure = exposure_of(state.positions),
        .unrealized_pnl = unrealized_pnl,
        .daily_net_pnl = equity - state.daily_starting_equity,
        .peak_equity = state.peak_equity,
        .trading_halted = state.trading_halted,
        .positions = state.positions
    };
}

risk::PortfolioRiskSnapshot PaperPortfolio::risk_snapshot_unlocked(const State& state) const {
    const auto account = view_unlocked(state);
    return {
        .equity = account.equity,
        .available_balance = std::min(state.cash_balance, account.equity),
        .gross_exposure = account.gross_exposure,
        .daily_starting_equity = state.daily_starting_equity,
        .daily_net_pnl = account.daily_net_pnl,
        .peak_equity = std::max(state.peak_equity, account.equity),
        .open_positions = static_cast<int>(state.positions.size()),
        .open_symbols = [&] {
            std::vector<std::string> symbols;
            symbols.reserve(state.positions.size());
            for (const auto& position : state.positions) {
                symbols.push_back(position.symbol);
            }
            return symbols;
        }()
    };
}

void PaperPortfolio::validate_state(const State& state) const {
    if (!std::isfinite(state.cash_balance) || state.cash_balance < 0.0 ||
        !std::isfinite(state.initial_balance) || state.initial_balance <= 0.0 ||
        !std::isfinite(state.daily_starting_equity) || state.daily_starting_equity <= 0.0 ||
        !std::isfinite(state.peak_equity) || state.peak_equity <= 0.0 ||
        state.utc_day.size() != 10 || state.next_execution_id == 0) {
        throw std::invalid_argument("Persisted paper portfolio contains invalid account values");
    }

    std::set<std::string> symbols;
    for (const auto& position : state.positions) {
        if (!valid_position(position) || !symbols.insert(position.symbol).second) {
            throw std::invalid_argument("Persisted paper portfolio contains an invalid or duplicate position");
        }
    }
    std::set<std::uint64_t> execution_ids;
    std::uint64_t maximum_id = 0;
    for (const auto& execution : state.executions) {
        if (execution.id == 0 || execution.symbol.empty() ||
            (execution.side != "BUY" && execution.side != "SELL") ||
            execution.reason.empty() ||
            !std::isfinite(execution.quantity) || execution.quantity <= 0.0 ||
            !std::isfinite(execution.price) || execution.price <= 0.0 ||
            !std::isfinite(execution.fee) || execution.fee < 0.0 ||
            !std::isfinite(execution.realized_pnl) ||
            execution.timestamp_ms <= 0 ||
            !execution_ids.insert(execution.id).second) {
            throw std::invalid_argument("Persisted paper portfolio contains an invalid execution");
        }
        maximum_id = std::max(maximum_id, execution.id);
    }
    if (state.next_execution_id <= maximum_id) {
        throw std::invalid_argument("Persisted paper execution sequence is inconsistent");
    }
    if (state.peak_equity < equity_of(state.cash_balance, state.positions)) {
        throw std::invalid_argument("Persisted paper portfolio peak equity is below current equity");
    }
}

nlohmann::json PaperPortfolio::serialize_state(const State& state) const {
    return {
        {"version", 1},
        {"cash_balance", state.cash_balance},
        {"initial_balance", state.initial_balance},
        {"daily_starting_equity", state.daily_starting_equity},
        {"peak_equity", state.peak_equity},
        {"utc_day", state.utc_day},
        {"trading_halted", state.trading_halted},
        {"next_execution_id", state.next_execution_id},
        {"positions", state.positions},
        {"executions", state.executions}
    };
}

PaperPortfolio::State PaperPortfolio::deserialize_state(
    const nlohmann::json& input) const {
    if (input.at("version").get<int>() != 1) {
        throw std::invalid_argument("Unsupported paper portfolio state version");
    }
    State state{
        .cash_balance = input.at("cash_balance").get<double>(),
        .initial_balance = input.at("initial_balance").get<double>(),
        .daily_starting_equity = input.at("daily_starting_equity").get<double>(),
        .peak_equity = input.at("peak_equity").get<double>(),
        .utc_day = input.at("utc_day").get<std::string>(),
        .trading_halted = input.at("trading_halted").get<bool>(),
        .next_execution_id = input.at("next_execution_id").get<std::uint64_t>(),
        .positions = input.at("positions").get<std::vector<PaperPosition>>(),
        .executions = input.at("executions").get<std::vector<PaperExecution>>()
    };
    validate_state(state);
    return state;
}

std::vector<PaperExecution> PaperPortfolio::process_scan(
    const std::vector<market::ScanResult>& results,
    risk::RiskManager& risk_manager,
    std::int64_t timestamp_ms) {
    std::lock_guard lock(mutex_);
    if (timestamp_ms == 0) {
        timestamp_ms = current_time_ms();
    }
    if (timestamp_ms < 0) {
        throw std::invalid_argument("Paper scan timestamp must not be negative");
    }

    State next = state_;
    const auto price_for = [&](const std::string& symbol) -> const market::ScanResult* {
        const market::ScanResult* found = nullptr;
        for (const auto& result : results) {
            if (result.symbol != symbol) {
                continue;
            }
            if (found != nullptr) {
                throw std::invalid_argument("Paper scan contains duplicate market symbols");
            }
            found = &result;
        }
        return found;
    };
    for (auto& position : next.positions) {
        const auto* result = price_for(position.symbol);
        if (result != nullptr && result->technical &&
            std::isfinite(result->technical->close) && result->technical->close > 0.0) {
            position.mark_price = result->technical->close;
        }
    }

    const double marked_equity = equity_of(next.cash_balance, next.positions);
    const auto day = utc_day(timestamp_ms);
    if (day != next.utc_day) {
        next.utc_day = day;
        next.daily_starting_equity = marked_equity;
    }
    next.peak_equity = std::max(next.peak_equity, marked_equity);

    std::vector<PaperExecution> current_executions;
    for (std::size_t index = 0; index < next.positions.size();) {
        const auto* result = price_for(next.positions[index].symbol);
        if (result == nullptr || !result->technical ||
            !std::isfinite(result->technical->close) || result->technical->close <= 0.0) {
            ++index;
            continue;
        }

        auto& position = next.positions[index];
        const double market_price = result->technical->close;
        const bool stop_hit = market_price <= position.stop_loss_price;
        const bool target_hit = market_price >= position.take_profit_price;
        const bool sell_signal = result->signal &&
            result->signal->action == ai::Signal::sell;
        if (!stop_hit && !target_hit && !sell_signal) {
            ++index;
            continue;
        }

        const std::string reason = stop_hit ? "protective stop at candle close" :
            target_hit ? "take-profit at candle close" : "advisory sell signal";
        const double exit_trigger_price = target_hit && !stop_hit
            ? position.take_profit_price : market_price;
        const double spread = result->order_book
            ? result->order_book->spread_percent : fallback_spread_fraction_;
        const double exit_price = exit_trigger_price *
            (1.0 - risk_settings_.slippage_rate - spread / 2.0);
        if (!std::isfinite(exit_price) || exit_price <= 0.0) {
            throw std::invalid_argument("Simulated sell fill price is invalid");
        }
        const double notional = position.quantity * exit_price;
        const double fee = notional * risk_settings_.taker_fee_rate;
        const double realized_pnl = position.quantity *
            (exit_price - position.entry_price) - position.entry_fee - fee;
        if (!std::isfinite(notional) || !std::isfinite(fee) ||
            !std::isfinite(realized_pnl)) {
            throw std::invalid_argument("Simulated sell execution contains non-finite values");
        }

        next.cash_balance += notional - fee;
        PaperExecution execution{
            .id = next.next_execution_id++,
            .symbol = position.symbol,
            .side = "SELL",
            .reason = reason,
            .quantity = position.quantity,
            .price = exit_price,
            .fee = fee,
            .realized_pnl = realized_pnl,
            .timestamp_ms = timestamp_ms
        };
        next.executions.push_back(execution);
        current_executions.push_back(std::move(execution));
        next.positions.erase(next.positions.begin() + static_cast<std::ptrdiff_t>(index));
    }

    for (const auto& result : results) {
        if (!result.signal || result.signal->action != ai::Signal::buy ||
            !result.technical || !result.technical->atr ||
            !std::isfinite(result.technical->close) || result.technical->close <= 0.0 ||
            !std::isfinite(*result.technical->atr) || *result.technical->atr <= 0.0) {
            continue;
        }
        if (std::any_of(next.positions.begin(), next.positions.end(),
                        [&](const PaperPosition& position) {
                            return position.symbol == result.symbol;
                        })) {
            continue;
        }
        if (next.trading_halted) {
            continue;
        }

        const double spread = result.order_book
            ? result.order_book->spread_percent : fallback_spread_fraction_;
        const double entry_price = result.technical->close *
            (1.0 + risk_settings_.slippage_rate + spread / 2.0);
        if (!std::isfinite(entry_price) || entry_price <= 0.0) {
            throw std::invalid_argument("Simulated buy fill price is invalid");
        }

        const auto snapshot = risk_snapshot_unlocked(next);
        const auto decision = risk_manager.assess({
            .symbol = result.symbol,
            .side = risk::OrderSide::buy,
            .entry_price = entry_price,
            .atr = *result.technical->atr,
            .spread_fraction = spread
        }, snapshot);
        if (decision.emergency_shutdown) {
            next.trading_halted = true;
            continue;
        }
        if (!decision.approved || !decision.plan) {
            continue;
        }

        const auto& plan = *decision.plan;
        const double entry_notional = plan.position_quantity * entry_price;
        const double entry_fee = entry_notional * risk_settings_.taker_fee_rate;
        if (!std::isfinite(entry_notional) || !std::isfinite(entry_fee) ||
            entry_notional + entry_fee > next.cash_balance) {
            throw std::logic_error("Risk-approved simulated buy exceeds paper cash balance");
        }
        next.cash_balance -= entry_notional + entry_fee;
        PaperPosition position{
            .symbol = result.symbol,
            .quantity = plan.position_quantity,
            .entry_price = entry_price,
            .mark_price = result.technical->close,
            .stop_loss_price = plan.stop_loss_price,
            .take_profit_price = plan.take_profit_price,
            .entry_fee = entry_fee,
            .opened_at_ms = timestamp_ms
        };
        if (!valid_position(position)) {
            throw std::logic_error("Risk manager produced an invalid paper position");
        }
        next.positions.push_back(position);

        PaperExecution execution{
            .id = next.next_execution_id++,
            .symbol = result.symbol,
            .side = "BUY",
            .reason = "risk-approved advisory buy signal",
            .quantity = position.quantity,
            .price = position.entry_price,
            .fee = entry_fee,
            .realized_pnl = 0.0,
            .timestamp_ms = timestamp_ms
        };
        next.executions.push_back(execution);
        current_executions.push_back(std::move(execution));
    }

    const auto status = risk_manager.monitor(risk_snapshot_unlocked(next));
    if (status.emergency_shutdown) {
        next.trading_halted = true;
    }
    next.peak_equity = std::max(next.peak_equity, equity_of(next.cash_balance, next.positions));
    validate_state(next);
    save_state(next);
    state_ = std::move(next);
    return current_executions;
}

PaperPortfolioView PaperPortfolio::view() const {
    std::lock_guard lock(mutex_);
    return view_unlocked(state_);
}

std::vector<PaperExecution> PaperPortfolio::execution_history() const {
    std::lock_guard lock(mutex_);
    return state_.executions;
}

void PaperPortfolio::reset_risk_shutdown(risk::RiskManager& risk_manager) {
    std::lock_guard lock(mutex_);
    risk_manager.reset_emergency_shutdown(risk_snapshot_unlocked(state_));
    State next = state_;
    next.trading_halted = false;
    validate_state(next);
    save_state(next);
    state_ = std::move(next);
}

void PaperPortfolio::save_state(const State& state) const {
    const auto parent = state_path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    auto temporary_path = state_path_;
    temporary_path += ".tmp";
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Unable to create paper portfolio state: " +
                                     temporary_path.string());
        }
        output << serialize_state(state).dump(2) << '\n';
        output.flush();
        if (!output) {
            throw std::runtime_error("Unable to write paper portfolio state: " +
                                     temporary_path.string());
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary_path.c_str(), state_path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Unable to atomically replace paper portfolio state");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary_path, state_path_, error);
    if (error) {
        throw std::system_error(error, "Unable to atomically replace paper portfolio state");
    }
#endif
}

}  // namespace crypto_trader::paper
