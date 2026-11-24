# Architecture Design

## Purpose and safety boundary

This C++20 application is an automated, long-only cryptocurrency trading bot
for Binance spot markets. It reads public market data, generates
trend-following signals, applies risk limits, and executes through one of three
venues:

- **Paper** — a simulated exchange priced from live public Binance data.
- **Testnet** — real orders on Binance Spot Testnet (no real money).
- **Mainnet** — real orders with real money on Binance Spot.

Mainnet is opt-in at several independent layers. It needs `mode: "mainnet"`,
`live_order_submission_enabled: true`, `live.order_base_url:
"https://api.binance.com"`, `live.real_money_acknowledged: true`, and a
`live.state_path` separate from the testnet ledger. Outside mainnet mode, only
`https://testnet.binance.vision` is accepted as the order endpoint. Testnet keys
come from `BINANCE_TESTNET_API_KEY` / `_SECRET` and mainnet keys from
`BINANCE_API_KEY` / `BINANCE_API_SECRET`, so a key can never be sent to the
wrong network. Keys are never written to configuration, state, or logs.

Paper and testnet run the **same** `LiveTrader` code path; only the venue
differs. The backtester uses the same signal engine, risk manager, and exit
rules on historical candles, trading every pair from one shared account.

## System context

```mermaid
flowchart LR
    Config[config/*.json]
    Public[Binance public REST<br/>candles, book, filters]
    Testnet[Binance Spot Testnet or mainnet<br/>signed REST orders]
    State[(Ledger + venue state<br/>atomic JSON)]
    Alerts[Telegram, optional]

    Config --> CLI[CLI]
    CLI --> Scanner[Market scanner]
    Public --> Scanner
    Scanner --> Signals[Signal engine]
    Signals --> Trader[LiveTrader]
    Risk[Risk manager] --> Trader
    Exits[Exit rules] --> Trader
    Trader --> Venue{{SpotVenue}}
    Venue -->|paper| PaperVenue[PaperVenue<br/>simulated fills]
    Venue -->|testnet / mainnet| BinanceVenue[BinanceSpotVenue]
    PaperVenue -->|prices, filters| Public
    BinanceVenue --> Testnet
    Trader <--> State
    Trader --> Alerts
    CLI --> Backtester[Portfolio backtester]
    Signals --> Backtester
    Risk --> Backtester
    Exits --> Backtester
```

## Capital protection

| Layer | Behaviour |
|---|---|
| Capital ring-fence | The bot trades only its own ledger, seeded with `live.capital_allocation` (paper: `paper_initial_balance`); other account funds are never used. Buys are limited to the smaller of that ledger and the exchange's free quote balance. |
| Order cap | No single entry exceeds `live.maximum_order_notional`, which must not exceed the allocation. |
| Per-trade risk | Each entry risks `risk.risk_per_trade_fraction` of the ledger (default 0.25%) between the fill price and the initial stop. |
| Exchange-resident stop | Every position gets a `STOP_LOSS` stop-market sell on the exchange immediately after the buy fills, so the loss stays bounded if the bot, PC, or network fails. Stop-market (not stop-limit) is used so a fast gap cannot skip the exit. |
| Never unprotected | If the stop cannot be placed or restored, the position is sold at market and trading halts. |
| Ratcheting | Break-even and trailing rules raise the stop; the exchange order is cancelled and replaced when the stop has risen by `live.stop_update_minimum_fraction`. |
| Bot-side backup exit | Each cycle the bot also sells at market if the best bid is at or below the ratcheted stop, at the take-profit, or on a sell/trend-break signal. |
| Daily pause | Losing `risk.daily_maximum_loss_fraction` (2%) in a UTC day blocks new entries until the next UTC day. Open positions keep their stops. |
| Drawdown kill switch | Falling `risk.maximum_drawdown_fraction` (10%) below peak equity halts trading permanently and, with `live.flatten_on_risk_shutdown`, sells every position. Only `--live-reset` / `--paper-reset` resumes, and only if limits are within bounds. `--live-flatten` triggers it manually. |
| Idempotent orders | Every order carries a persisted unique client order id. If a request times out or returns HTTP 5xx, the bot queries the order by that id instead of guessing. A failed lookup is treated as "outcome unknown", never as "rejected". An unresolved buy halts new entries. |
| Entry journal | Each entry is persisted as `pending_entry` before the buy is sent. Every cycle (and `--live-flatten`) looks a pending entry up by client order id. If it filled, the position is booked and gets its exchange stop. If it never reached the exchange, it is cleared. No new entries are made while one is pending. This closes the gap where a crash or lost response right after a fill would leave an untracked, unprotected position. |
| Fee-safe sizing | A filled order is never discarded because the commission lookup failed. The booked quantity is clamped to the account's free base balance, so the stop never exceeds what is held. Fees paid in BNB are valued in the quote asset and charged to the ledger. |
| Crash safety | State is written atomically after every order. Each cycle reconciles stops against the exchange (filled, cancelled, or missing stops are detected), and the last candle each pair was bought on is persisted so a restart never re-buys the same signal. |
| Mainnet pre-flight | Before every mainnet trading session the bot reads `/sapi/v1/account/apiRestrictions`. It refuses keys that can withdraw or that lack spot-trading or read permission (missing fields fail closed), and warns about keys without an IP restriction or with transfer, margin, futures, or options rights. It also requires `canTrade`, warns if the free quote balance is below the allocation, and logs the real-money limits. Status, flatten, and reset skip this check so an emergency exit is never blocked. |
| Unattended operation | `deploy/crypto-ai-trader.service` runs the bot as a systemd user service with automatic restart; optional Telegram alerts report every trade, halt, and error. |

No safeguard can guarantee against loss: stop-market orders can fill below the
stop in a gap, and the strategy loses money in choppy or bearish markets. These
layers bound the size of losses; they do not prevent them.

## Runtime components

| Component | Responsibility | Main location |
|---|---|---|
| Configuration, logging, alerts | Parse and validate all settings; console plus rotating log file; optional Telegram notifier. | `include/common/`, `src/common/` |
| Public market data | Closed-candle and order-book fetches over TLS; pages backwards for long histories; drops the still-forming candle so live research matches backtests. | `include/exchange/binance_exchange.hpp`, `src/exchange/binance_exchange.cpp` |
| Signed REST client | HMAC-SHA256 signing, server-clock sync, and classification of failures as definite rejections (`BinanceApiError`) or unknown outcomes (`BinanceUnknownOutcome`). Works public-only without keys. | `include/exchange/binance_signed_client.hpp`, `src/exchange/binance_signed_client.cpp` |
| Market stream | Public trade-price updates over TLS WebSocket (`--stream`). | `include/exchange/binance_market_stream.hpp`, `src/exchange/binance_market_stream.cpp` |
| Indicators and analysis | EMA, RSI, MACD, Bollinger, ATR, Stochastic RSI; trend, support/resistance and breakout, volume flow, volatility, order-book metrics. | `include/indicators/`, `include/market/`, `src/indicators/`, `src/market/` |
| Scanner | Fetches each pair, assembles `ScanResult`, evaluates signals, and in loop mode wakes at `scan_interval_seconds` or 3 s after each candle closes, whichever is sooner. | `include/market/market_scanner.hpp`, `src/market/market_scanner.cpp` |
| Signal engine | Weighted research with quality, coverage, confirmation, volatility, spread, and cost gates, plus the trend and breakout gates and the `exit_long` trend-break flag. Emits once per closed candle. | `include/ai/signal_engine.hpp`, `src/ai/signal_engine.cpp` |
| Risk manager | Sizes buy-only spot entries from an account snapshot; enforces position, exposure, daily-pause, and drawdown limits. | `include/risk/risk_manager.hpp`, `src/risk/risk_manager.cpp` |
| Exit rules | Ratchets the protective stop (break-even, chandelier trail). Shared by backtester and trader. | `include/risk/exit_rules.hpp`, `src/risk/exit_rules.cpp` |
| Spot venue | `SpotVenue` interface: symbol filters, book top, balances, market orders, exchange stop-market orders, query, cancel. | `include/execution/spot_venue.hpp` |
| Binance venue | `SpotVenue` over the signed client with client-order-id reconciliation and commission lookup. | `src/execution/binance_spot_venue.cpp` |
| Paper venue | Simulated `SpotVenue`: prices and filters from public Binance data; persisted balances and resting stops; stops trigger when the bid crosses them. | `include/execution/paper_venue.hpp`, `src/execution/paper_venue.cpp` |
| Live trader | The trading engine: manages positions (stop reconciliation, ratcheting, exits), applies risk limits, opens protected positions, and persists its ledger after each order. | `include/live/live_trader.hpp`, `src/live/live_trader.cpp` |
| Portfolio backtester | Replays all pairs on a common timeline from one shared account with the same signal, risk, and exit rules and conservative fills. | `include/backtest/backtester.hpp`, `src/backtest/backtester.cpp` |
| ML evaluator | Offline regularized logistic classifier with chronological holdout metrics; not used for trading. | `include/ml/ml_evaluator.hpp`, `src/ml/ml_evaluator.cpp` |
| Dry-run executor | Validated order preview (`--execution-dry-run`); sends nothing. | `include/execution/order_executor.hpp`, `src/execution/order_executor.cpp` |
| CLI | Command dispatch, plain-language market summary, account display. | `src/main.cpp` |

## Trading strategy

The default configuration is a long-only, 4h **trend-following breakout**
("trade with the flow") strategy.

| Stage | Rule | Setting |
|---|---|---|
| Regime | EMA 20 > EMA 50 > EMA 200 and close above EMA 20. | `signals.require_bullish_trend_for_buy` |
| Trigger | Close breaks above the prior 50-bar high (Donchian breakout). | `signals.require_breakout_for_buy`, `research.support_resistance_period` |
| Confirmation | Weighted momentum, candle-flow, and order-book score with quality, liquidity, spread, and cost gates. | `signals.*` |
| Sizing | Risk 0.25% of equity between entry and the initial stop, capped by cash, exposure, order cap, and 3 open positions. | `risk.risk_per_trade_fraction`, `risk.maximum_open_positions` |
| Initial stop | Entry − 2 ATR, resting on the exchange. | `risk.stop_atr_multiple` |
| Break-even | Stop moves to entry + round-trip costs once the highest close is 1.5R above entry. | `risk.breakeven_trigger_r` |
| Trailing stop | Highest close since entry − 4 ATR; never moves down. | `risk.trailing_stop_atr_multiple` |
| Take-profit | Entry + 10 ATR, so winners are rarely capped early. | `risk.target_atr_multiple` |
| Trend break | EMA 20 below EMA 50 with close below EMA 50 sells, even when entry gates (e.g. high volatility) block new signals. | `TradingSignal::exit_long` |

### Backtest evidence

Portfolio backtest (`--backtest-all`): BTC, ETH, SOL, BNB, XRP on 4h candles,
Nov 2023 – Nov 2026, one shared account, 0.1% taker fee, 0.05% slippage, 0.02%
spread on every fill, stop-first when a bar touches both stop and target.

| Risk per trade | 3-year return | Equal-weight buy-and-hold | Max drawdown | Worst day | Held-out bear year (Nov 2025 – Nov 2026) | Kill switch |
|---|---|---|---|---|---|---|
| **0.25% (default)** | **+17.7%** | +98% | **8.1%** | −1.5% | **−4.3%** (buy-and-hold −34.5%) | not triggered |
| 0.5% | +38.6% | +98% | 10.5% | −3.0% | −9.5% | triggered in 2026 |
| 1% | +36.5% | +98% | 10.1% | −3.1% | −8.9% | triggered early |

Yearly at 0.25%: 2023 (Nov–Dec) +2.2%, 2024 +11.6%, 2025 +7.8%, 2026 −4.3%.

**Out-of-sample check (Nov 2026).** The parameters above were chosen on
Nov 2023 – Nov 2026. Rerunning the unchanged default strategy on 15,000 4h
candles (Jan 2020 – Nov 2026) adds roughly four years it was never tuned on:

| Variant | Return | Max drawdown | Profit factor | Kill switch |
|---|---|---|---|---|
| **Default (0.25% risk)** | **+47.0%** | **8.1%** | **1.56** | not triggered |
| + pullback-in-uptrend entries | +2.4% | 10.3% | 1.04 | triggered in 2023 |
| 0.5% risk | +37.8% | 10.2% | 1.61 | triggered in 2022, idle after |

Default strategy in the untouched years: 2020 +7.8% (profit factor 1.70),
2021 +10.7% (1.83), 2022 −3.9% (0.31) while the five coins fell 52–94%, 2023 +11.4%
(2.12). Equal-weight buy-and-hold returned +1457% over the same window, with
far deeper drawdowns. `--backtest-all` now also prints trades and profit factor
by year.

**Robustness checks (Nov 2026, same window, default 5 coins unless noted):**

| Test | Return | Max drawdown | Profit factor | Kill switch |
|---|---|---|---|---|
| Stop 1.5 / 2.5 ATR | +54.6% / +36.2% | 9.4% / 7.4% | 1.54 / 1.52 | no / no |
| Trailing stop 3 / 5 ATR | +40.0% / +48.4% | 7.4% / 8.4% | 1.49 / 1.57 | no / no |
| Breakout window 35 / 70 bars | +46.2% / +45.4% | 8.1% / 7.6% | 1.52 / 1.60 | no / no |
| Double fees, slippage and spread | +27.5% | 8.0% | 1.35 | no |
| 10 coins never used in development (ADA, DOGE, LTC, LINK, DOT, AVAX, ATOM, TRX, ETC, FIL) | +7.4% | 10.0% | 1.15 | triggered in 2023 |

Across 526 trades from single-coin runs of the five default coins, the mean
trade is positive (t = 3.6; bootstrap P(mean ≤ 0) ≈ 0.0001), and the total
stays positive without the five best trades. The result does not hinge on
exact settings and survives doubled costs. Trades in correlated coins are not
independent, so the t-statistic overstates certainty.

The edge is much weaker on coins outside the development set: several of them
fell for years after 2021, and the kill switch stopped that run. Part of the
default result therefore comes from trading the strongest large coins, so
adding more pairs is not a free improvement.
At 0.5% with doubled fees and 5× spread the strategy still returned +28.7%
(profit factor 1.49).

Findings that shaped the design:

- The original 1m weighted-score strategy lost −5.5% in the development period
  with realistic costs (profit factor 0.58); 1m moves are smaller than fees.
- Single-symbol backtests understated risk: trading five correlated pairs from
  one account roughly doubled the worst drawdown. Portfolio backtesting is now
  the default.
- A daily-loss limit that latched permanently stopped the bot for over a year
  after one bad day; it now pauses until the next UTC day.
- Faster timeframes lose to fees. The same rules on 1h, 15m and 5m candles
  gave profit factors of 0.87, 0.37 and 0.24, and 15m/5m tripped the kill
  switch. Measured Nov 2026: the median 1-second BTC move is 0.005% against a
  0.20% round-trip taker fee, and a REST order takes about 220 ms from a home
  connection. High-frequency trading is therefore not viable on this setup.
- A pullback-in-uptrend entry (EMA 20 > 50 > 200, close between EMA 20 and 50,
  RSI ≤ 50) was defined once and tested once on Jan 2020 – Nov 2026. It cut the
  profit factor from 1.56 to 1.04 and tripped the kill switch, so it was not
  adopted.
- A Bitcoin regime filter (buy only while BTC's EMA 50 is above its EMA 200),
  tested once at 0.5% risk, did not help: +113.0% vs +111.4% on the default
  coins (better in 2022, worse in 2026), and worse on a broader 11-coin set
  (+15.9% vs +22.4%, kill switch in both). Not adopted.
- Short selling (1x, as on perpetual futures) with rules mirroring the long
  side was tested once at 0.5% risk with a conservative funding cost. It
  lost money (129 shorts, profit factor 0.91), lost even in the 2022 crash
  year, and tripped the 20% kill switch in 2022, so the bot missed 2023–2025:
  +28.6% vs +111.4% for long-only. Tripling the funding cost changed little.
  Not adopted.
- Doubling risk to 0.5% with the 10% kill switch tripped it in 2022, ending
  with less than the default. Scaling the daily pause and kill switch with the
  risk keeps the same 460 trades and scales returns almost linearly
  (Jan 2020 – Nov 2026):

  | Risk per trade | Daily pause / kill switch | Per year | Max drawdown | Worst year |
  |---|---|---|---|---|
  | 0.25% | 2% / 10% | ~5.8% | 8.1% | −4.3% |
  | 0.5% | 4% / 20% | ~11.6% | 15.6% | −8.4% |
  | 0.75% | 6% / 30% | ~17.3% | 22.6% | −12.5% |
  | 1.0% | 8% / 40% | ~22.0% | 27.3% | −15.5% |

  The testnet and mainnet configs use 0.5% / 4% / 20%. Future drawdowns
  should be expected to exceed the backtest maximum.
- The strategy does not beat buy-and-hold in bull markets. Its value is bounded
  drawdowns and staying mostly in cash during downtrends. The Nov 2025 – Nov
  2026 period has been inspected, so further tuning needs new data or
  walk-forward validation. Historical results do not forecast future returns.

## Trading cycle

```mermaid
sequenceDiagram
    participant Scanner
    participant Trader as LiveTrader
    participant Risk as RiskManager
    participant Venue as SpotVenue
    Scanner->>Trader: process_scan(closed-candle research)
    loop each open position
        Trader->>Venue: query exchange stop
        alt stop filled
            Trader->>Trader: record SELL, persist
        else
            Trader->>Trader: ratchet stop (exit rules)
            Trader->>Venue: book top
            alt bid ≤ stop, target, sell, or trend break
                Trader->>Venue: cancel stop, market SELL
            else stop raised enough
                Trader->>Venue: cancel and replace stop
            end
        end
    end
    Trader->>Risk: monitor(account snapshot)
    alt drawdown limit
        Trader->>Venue: flatten all, halt
    end
    loop each BUY signal on a new candle
        Trader->>Risk: assess(entry at ask, ATR, spread)
        Trader->>Venue: market BUY (client order id)
        Trader->>Venue: STOP_LOSS sell
        alt stop rejected
            Trader->>Venue: market SELL, halt
        end
    end
    Trader->>Trader: persist ledger
```

Signals come from mainnet public data in both paper and testnet mode; testnet
prices closely track mainnet. A cycle that throws (for example a network
failure) is logged, alerted, and retried on the next wake-up while exchange
stops keep positions protected.

## Configuration and persistence

| File | Purpose |
|---|---|
| `config/config.json` | Paper mode (default). |
| `config/testnet.json` | Testnet mode: `mode: "testnet"`, `live_order_submission_enabled: true`, and the `live` section. |
| `config/mainnet.json` | Real-money mode: `mode: "mainnet"`, mainnet order endpoint, its own ledger and log. Ships with `real_money_acknowledged: false`, so it will not start until you change that. |

Optional sections and keys default to backward-compatible values:
`signals.require_bullish_trend_for_buy`, `require_breakout_for_buy`,
`suppress_repeated_direction`; `risk.trailing_stop_atr_multiple`,
`breakeven_trigger_r`; `backtest.candle_count`, `spread_fraction`; `live.*`;
`log_file`.

| State | Contents |
|---|---|
| `paper_state_path` / `live.state_path` | Trader ledger: cash, allocation, daily baseline, peak equity, halt flag, positions with initial/ratcheted/exchange stop and stop order id, execution history, last entry candle per pair. |
| `<paper_state_path>.venue.json` (paper only) | Simulated exchange balances and orders. |
| `log_file` | Rotating log (5 MB × 3) of market decisions, trades, and errors. |

All state is written to a temporary sibling and atomically renamed. Malformed or
unsupported state fails closed instead of resetting the account.

## CLI workflows

Run from the repository root; the config path defaults to `config/config.json`.

| Command | Behaviour |
|---|---|
| `--paper [config]` / `--paper-once [config]` | Run the trader continuously / once against the simulated venue. |
| `--paper-status`, `--paper-flatten`, `--paper-reset [config]` | Show the ledger; sell everything and halt; resume after a halt if limits allow. |
| `--live [config]` / `--live-once [config]` | Same, against Binance Spot Testnet or, with `config/mainnet.json`, mainnet (matching API keys required). |
| `--live-status`, `--live-flatten`, `--live-reset [config]` | Testnet/mainnet equivalents. |
| `--backtest-all [config]` | Shared-account portfolio backtest of every configured pair. |
| `--backtest SYMBOL [config]` | Single-pair backtest with the trade list. |
| `--scan-once` / `--scan [config]` | Detailed research output without trading. |
| `--candles SYMBOL`, `--stream SYMBOL` | Print public candles / stream trade prices. |
| `--ml-evaluate SYMBOL` | Offline ML holdout evaluation. |
| `--execution-status`, `--execution-dry-run SYMBOL SIDE QTY` | Show the execution boundary; preview an order without sending it. |

## Build and verification

```bash
cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release
```

```powershell
cmake --preset windows-vcpkg
cmake --build --preset windows-release
ctest --preset windows-release
```

The GoogleTest suite covers configuration validation, market research, signals,
risk (including the daily pause), exit rules, the portfolio backtester, the
live trader against a fake venue (protection, ratcheting, rejected stops,
flatten, restart safety, capital caps), the paper venue, HMAC signing against
Binance's documented vector, ML evaluation, and the dry-run executor.

## Known limitations

- The mainnet path shares all of its order logic with testnet, but it has not
  yet been run against mainnet. Rehearse on testnet with the same sizing
  first.
- Binance's 5 USDT minimum order size is not modeled by the backtester. Small
  allocations (below about 150 USDT at 0.25% risk, about 80 USDT at 0.5%) skip many signals that the
  backtest would have taken.
- After an unknown buy outcome is reconciled, new entries stay halted until
  `--live-reset`, so that a person checks the account first.
- Paper market orders are not retained by the simulated venue, so a paper
  entry lost to a crash is treated as never placed.
- Paper stops trigger only when a cycle observes the bid below the stop, so
  paper fills are checked at the scan cadence rather than tick by tick.
- Backtests use OHLCV bars: trailing stops ratchet on closed-bar closes and
  trigger on bar lows; historical order books are unavailable, so a fixed
  spread is assumed.
- Strategy parameters were selected on Nov 2023 – Nov 2026 data, which is no
  longer an untouched holdout.
