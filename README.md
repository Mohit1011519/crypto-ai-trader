# Crypto AI Trader

Modular C++20 cryptocurrency market-data, research, signal, risk-analysis,
paper-portfolio, backtesting, and machine-learning research foundation. The
project includes a dry-run execution interface, but live order submission is
disabled and no exchange orders are placed.

## Architecture

See [ARCHITECTURE.md](./ARCHITECTURE.md) for component responsibilities, data
flows, persistence boundaries, execution safeguards, and known limitations.

```text
config/config.json
        |
        +--> common::load_config() --> validated settings
        |
        +--> BinanceExchange --> public REST candles and order book
        |                              |
        +--> BinanceMarketStream       +--> MarketScanner
                    |                  |      +--> technical indicators
                    +--> trade prices  |      +--> trend / support / resistance
                                       |      +--> volume / volatility
                                       |      +--> order-book research
                                       +--> SignalEngine --> research report
                                                    |
                                                    +--> RiskManager (account snapshot)
                                                               |
                                                               +--> PaperPortfolio --> local state/ledger
                                                               +--> Backtester / ML evaluator
                                                               +--> DryRunOrderExecutor
                                                                    DisabledLiveOrderExecutor
```

`Exchange` is an abstraction for public market-data calls. The scanner uses it
to fetch each configured market's OHLCV candles and order book, then sends those
inputs to deterministic, independently testable research functions. Research
values carry input sample counts, timestamps, and a sample-sufficiency quality
score. Indicator warm-up values are reported as unavailable instead of
inventing data.

## Phase status

1. **Connectivity foundation — implemented:** CMake, validated configuration,
   logging, Binance REST candles, TLS WebSocket trade prices, and tests.
2. **Market research — implemented:** configurable scanner, EMA, RSI, MACD,
   Bollinger Bands, ATR, Stochastic RSI, trend direction/strength, rolling
   support/resistance and breakout checks, volume spike/average and candle-based
   pressure proxies, volume-price divergence, ATR and historical volatility,
   volatility classification, and order-book spread/imbalance/depth/liquidity
   measurements.
3. **Signals — implemented:** configurable weighted research aggregation,
   independent confirmation count, quality/coverage/regime gates, and
   fee/slippage/spread-adjusted reward/risk checks. Repeated same-direction
   signals are suppressed for the lifetime of the scanner.
4. **Risk — implemented:** independent snapshot monitoring, latched daily-loss
   and account-drawdown shutdowns, buy-only spot sizing, maximum position and
   gross-exposure limits, ATR protective exits, and fee/slippage/spread-adjusted
   risk estimates. The planner is not connected to execution.
5. **Paper portfolio — implemented:** local JSON persistence, cash/equity and
   position ledger, risk-approved simulated spot buys, simulated sell/stop/
   target exits, and fee/slippage-aware paper fills. It is not connected to
   exchange order endpoints.
6. **Backtesting — implemented:** chronological single-symbol historical
   simulation using completed-bar research, next-bar-open entries, the shared
   signal and risk rules, transaction costs, stop-first same-bar ambiguity,
   equity/drawdown and return metrics, and comparison to buy-and-hold.
7. **Machine learning — implemented:** a deterministic, regularized logistic
   classifier with past-only candle features, cost-adjusted forward labels,
   chronological train/holdout split with label-overlap purging, and holdout
   classification metrics. It is an offline research evaluation and does not
   alter signals, paper trading, or live execution.
8. **Execution boundary — dry-run only:** a validated order-executor interface,
   simulated preview adapter, and always-rejecting live adapter. Authenticated
   exchange execution, API credentials, and real order placement are not
   implemented; configuration rejects attempts to enable them.

Research values and signals are descriptive/advisory, not recommendations or
claims of profitability. Live scan signals do not use the paper portfolio's
position state; a suppressed signal does not imply an existing open position.
The ATR-multiple reward/risk figure
is a configured scenario estimate, not a statistically validated expected
return. The displayed confidence is the absolute weighted research score, not a
calibrated probability of profit. Volatility acts as a score dampener and
high/sudden volatility blocks signals; it is not itself a directional
bullish/bearish input. Candle-based buy/sell pressure is a close-location proxy,
not exchange-reported aggressor-side trade flow. Historical volatility is the
unannualized standard deviation of log returns for the configured candle
window. Order-book depth is a snapshot and can change immediately. If order-book
data is unavailable, the configured maximum spread is used conservatively in
the cost estimate and the missing component reduces research coverage.

## Dependencies

- C++20 compiler and CMake 3.24+
- libcurl, Boost.Asio/Beast, Boost.System, and OpenSSL (declared by `vcpkg.json`)
- nlohmann/json 3.11.3, spdlog 1.14.1, and GoogleTest 1.14.0, fetched by CMake

On Windows, install Visual Studio C++ Build Tools, CMake, and vcpkg. Set
`VCPKG_ROOT` to the vcpkg installation directory. The first build requires
network access to fetch the pinned CMake dependencies and vcpkg packages.

## Build and test (Windows PowerShell)

Run from the project directory:

```powershell
cmake --preset windows-vcpkg
cmake --build --preset windows-release
ctest --preset windows-release
```

VS Code CMake Tools is configured to use the included `windows-vcpkg` configure
preset and `windows-release` build/test presets.

## Commands

The default config path is `config/config.json`; commands accept an alternate
config path as shown:

```powershell
.\build\Release\crypto-ai-trader.exe --candles BTCUSDT [config-path]
.\build\Release\crypto-ai-trader.exe --stream BTCUSDT [config-path]
.\build\Release\crypto-ai-trader.exe --scan-once [config-path]
.\build\Release\crypto-ai-trader.exe --scan [config-path]
.\build\Release\crypto-ai-trader.exe --paper-once [config-path]
.\build\Release\crypto-ai-trader.exe --paper [config-path]
.\build\Release\crypto-ai-trader.exe --paper-status [config-path]
.\build\Release\crypto-ai-trader.exe --paper-reset [config-path]
.\build\Release\crypto-ai-trader.exe --backtest BTCUSDT [config-path]
.\build\Release\crypto-ai-trader.exe --ml-evaluate BTCUSDT [config-path]
.\build\Release\crypto-ai-trader.exe --execution-status [config-path]
.\build\Release\crypto-ai-trader.exe --execution-dry-run BTCUSDT buy 0.001 [config-path]
```

`--scan-once` researches every configured pair once. `--scan` repeats at the
configured interval; press Ctrl+C to request shutdown. Individual market-data
failures are logged and reported without preventing the remaining pairs from
being scanned. Scan output includes the signal action, signed research score,
confidence, available-weight coverage, confirmations, estimated net reward/risk,
estimated round-trip costs, component scores, and the decision's explanations.
`--paper-once` processes one public-data scan through the risk manager and
paper ledger; `--paper` repeats at the configured interval. BUY signals can
open simulated spot positions when risk checks approve. SELL signals close
existing paper positions, while stop/target exits are evaluated using candle
close values. These are simplified paper fills—not realistic queue, liquidity,
or intrabar execution models. Positions, cash, risk shutdown state, and
simulated execution records persist at `paper_state_path`; an existing state
file takes precedence over a later change to `paper_initial_balance`.
`--paper-status` displays the last persisted account marks. `--paper-reset`
fetches fresh market data, updates marks and applicable exits, and then resets
the latched risk shutdown only if configured limits are safe. The path is local
application state and should be backed up before manual edits.

`--execution-status` reports the configured safety boundary.
`--execution-dry-run` fetches public candles and previews a requested quantity
against the last candle close; it is not a live quote, risk approval, paper fill,
or exchange order. The executor returns a simulation-only result and does not
persist it. The disabled live adapter rejects every request.
`live_order_submission_enabled` must remain `false`; configuration loading
fails if it is set to `true`.

`--backtest SYMBOL` fetches the configured number of recent public candles and
simulates one symbol without modifying the paper account or state file. Each
signal is calculated using only bars already closed; entries occur at the
following bar's open. Protective stops and targets are checked against each
bar's high/low; if both trigger in one bar, the stop is assumed first. Gaps
through a stop fill at the open, while targets are assumed to fill at their
target price. Since historical order-book data is unavailable, the configured
maximum spread is applied conservatively. Backtest metrics include final equity,
strategy and buy-and-hold returns, maximum drawdown, an unannualized per-bar
Sharpe ratio, completed trade counts, and profit factor when there are losses.
The configured candle limit bounds the available history; this is a research
simulation, not a forecast or execution-quality guarantee.

`--ml-evaluate SYMBOL` fits a small L2-regularized logistic classifier on the
first 70% of chronological samples and reports metrics on a later holdout.
Six candle-only features use data available at the sample close; the binary
label asks whether the following five-candle log return exceeded estimated
round-trip fees, slippage, and configured maximum spread. Training labels that
would extend into the holdout are purged, and feature normalization is fit only
on the training subset. Reported accuracy, precision, recall, log loss,
Brier score, and ROC-AUC (when both classes occur) describe that limited sample;
they are not calibrated profitability estimates. The model is retrained for
each command, is not persisted, and is not wired into BUY/SELL signals or paper
execution.

REST and WebSocket TLS certificate verification are enabled. REST requests use
the configured timeout; WebSocket DNS, TCP, TLS, handshake, and read operations
are bounded by `request_timeout_seconds`. Live WebSocket access requires
outbound connectivity to `stream.binance.com:9443`.

## Configuration and safety

`config/config.json` defines the paper-only mode, Binance endpoints, selected
symbols, candle settings, connection timeouts, scan interval, and research
periods and thresholds. Signal settings define weights, thresholds, required
confirmations, ATR-based expected reward/risk multiples, taker fees, slippage,
and maximum acceptable spread. Sentiment currently has zero weight because no
sentiment provider is implemented. Order-book depth must use a supported
Binance depth limit. `paper_initial_balance` seeds a new paper account only;
`paper_state_path` selects its persistent state and execution ledger. Risk
settings define per-trade equity risk (default 0.5%),
maximum open positions (3), daily loss (2%), peak-equity drawdown (10%), gross
exposure (100% of equity), ATR exit multiples, fees, and slippage. The standalone
risk manager requires a caller-supplied snapshot; the paper portfolio constructs
that snapshot from its local ledger. It latches shutdown when daily loss or
drawdown reaches its limit and requires an explicit safe reset. New short orders
are rejected; the paper simulator only sells to close positions it already
holds. The ledger does not reconcile exchange balances or external transactions.
Invalid or inconsistent settings and corrupted ledger files are rejected
instead of silently reset. No API credentials are used or stored, and the
application cannot place exchange orders.
