# Crypto AI Trader

Modular C++20 cryptocurrency market-data, research, signal, risk-analysis, and
paper-portfolio foundation. Phases 1 through 5 provide public Binance market
data, advisory BUY/SELL/HOLD signals, risk sizing, and locally simulated
positions with a persistent execution ledger. The application does not
authenticate or place exchange orders.

## Architecture

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
                                                    +--> RiskManager (explicit account snapshot)
                                                               |
                                                               +--> PaperPortfolio --> local state/ledger
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
6. **Backtesting — not implemented.**
7. **Machine learning — not implemented.**
8. **Live trading — not implemented.**

Research values and signals are descriptive/advisory, not recommendations or
claims of profitability. No position state is tracked; a suppressed signal
does not imply an existing open position. The ATR-multiple reward/risk figure
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
