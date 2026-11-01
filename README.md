# Crypto AI Trader

Modular C++20 cryptocurrency market-data and research foundation. Completed
Phases 1 through 3 provide public Binance market-data access, transparent
research, and advisory BUY/SELL/HOLD signals. The application does not
authenticate, track positions, or place orders.

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
4. **Risk — not implemented:** independent limits, position sizing, and
   protective exits.
5. **Paper portfolio — not implemented:** persistence, ledger, and simulated
   execution.
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
```

`--scan-once` researches every configured pair once. `--scan` repeats at the
configured interval; press Ctrl+C to request shutdown. Individual market-data
failures are logged and reported without preventing the remaining pairs from
being scanned. Scan output includes the signal action, signed research score,
confidence, available-weight coverage, confirmations, estimated net reward/risk,
estimated round-trip costs, component scores, and the decision's explanations.

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
Binance depth limit. Invalid or inconsistent settings are rejected. No API
credentials are used or stored, and this phase cannot place orders.
