<<<<<<< HEAD
# Crypto AI Trader

Phase 1 foundation for a modular C++20 cryptocurrency market-data application.
The current executable only consumes Binance public market data. It does not
authenticate, submit orders, or make trading recommendations.

## Architecture and implementation plan

```text
config/config.json
        |
        v
common::load_config() ----> common::initialize_logging()
        |
        +----> BinanceExchange ----> Binance REST /api/v3/klines
        |
        +----> BinanceMarketStream ----> Binance TLS WebSocket trade stream
```

The `Exchange` interface isolates exchange-specific historical data access.
`BinanceExchange` handles REST and returns typed candle records.
`BinanceMarketStream` owns TLS, WebSocket parsing, and reconnect behavior, and
delivers typed price updates through a callback. Configuration is validated
before either network client starts. All API access in this phase is public,
read-only market data.

Planned phases:

1. **Connectivity foundation (current):** CMake, configuration, logging,
   Binance public REST candles, live trade-price stream, and configuration tests.
2. **Market research:** scanner, indicators, trend, volume, volatility, and
   order-book analysis.
3. **Signals:** transparent rule-based research aggregation and BUY/SELL/HOLD
   decisions.
4. **Risk:** independent limits, sizing, stops, and take-profit calculations.
5. **Paper portfolio:** persistence, ledger, and simulated execution.
6. **Backtesting:** chronological simulation and performance metrics.
7. **ML:** feature pipeline, ONNX inference, and out-of-sample evaluation.
8. **Optional live execution:** only after independent safeguards and explicit
   user configuration are designed and tested. It is not present in this phase.

## Dependencies

- C++20 compiler and CMake 3.24+
- libcurl development files
- Boost 1.74+ (System component; Beast is header-only)
- OpenSSL development files
- CMake downloads nlohmann/json 3.11.3, spdlog 1.14.1, and GoogleTest 1.14.0
  with FetchContent

On Windows, install a Visual Studio C++ workload and CMake, plus Boost, OpenSSL,
and libcurl development packages. Ensure their CMake package/config files are
discoverable (for example via `CMAKE_PREFIX_PATH`). The first configure requires
network access to fetch the three pinned CMake dependencies.

The included `vcpkg.json` declares the native packages. With vcpkg installed,
configure CMake with its toolchain file so those dependencies are installed
automatically:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
```

If `VCPKG_ROOT` is not set, replace it with the path where you installed vcpkg.

## Build and test (PowerShell)

From the `crypto-ai-trader` directory:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Usage

The default config path is `config/config.json`; an alternate path can be
provided as the final argument.

```powershell
.\build\Release\crypto-ai-trader.exe --candles BTCUSDT
.\build\Release\crypto-ai-trader.exe --stream BTCUSDT
```

Symbols passed on the command line must also be listed in `trading_pairs`.
`--stream` reconnects after network failures using the configured delay. Press
Ctrl+C to request shutdown. REST and TLS certificate verification are enabled.

## Configuration and safety

`config/config.json` contains the exchange endpoints, configured symbols,
candle interval/limit, timeouts, reconnect delay, and log level. The Phase 1
mode is intentionally restricted to `paper`; configuration validation rejects
`live`. No API credentials are used or stored.

Binance rate limits, availability, and market-data accuracy remain external
dependencies. This project makes no claim of profitability.
=======
# ai_bot_crypto
crypto-ai
>>>>>>> ce93d6f92ea783ea80c6666d9c2cad5a9a7e7e96
