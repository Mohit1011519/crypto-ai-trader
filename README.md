# Crypto AI Trader

Automated C++20 trend-following bot for Binance spot. It buys breakouts in
established uptrends, protects every position with a stop-loss order resting on
the exchange, and trails that stop to lock in profit. It runs in **paper mode**
(simulated fills on live prices) or on **Binance Spot Testnet** (real orders,
no real money). Mainnet order submission is not enabled.

See [ARCHITECTURE.md](./ARCHITECTURE.md) for the design, capital-protection
layers, strategy rules, and backtest evidence.

## Results at a glance

Portfolio backtest of BTC, ETH, SOL, BNB, and XRP from one shared account, 4h
candles, Nov 2023 – Nov 2026, realistic fees and slippage, default 0.25% risk
per trade:

| 3-year return | Buy-and-hold | Max drawdown | Bear year (Nov 2025 – Nov 2026) |
|---|---|---|---|
| +17.7% | +98% | 8.1% | −4.3% (buy-and-hold −34.5%) |

The bot does not beat buy-and-hold in bull markets; it limits drawdowns and
stays mostly in cash during downtrends. Backtests are not forecasts.

## Build

Linux (Ubuntu packages: `build-essential cmake ninja-build libcurl4-openssl-dev
libssl-dev libboost-all-dev`):

```bash
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

Windows (Visual Studio Build Tools, CMake, vcpkg with `VCPKG_ROOT` set):

```powershell
cmake --preset windows-vcpkg
cmake --build --preset windows-release
ctest --preset windows-release
```

The first build downloads nlohmann/json 3.11.3, spdlog 1.14.1, and GoogleTest
1.14.0.

## Run

### Paper trading (no keys needed)

```bash
./build-linux/crypto-ai-trader --paper            # keeps running; Ctrl+C stops
./build-linux/crypto-ai-trader --paper-status
```

### Testnet trading

1. Create API keys at <https://testnet.binance.vision> (GitHub login).
2. Export them; they are never stored in files:

   ```bash
   export BINANCE_TESTNET_API_KEY=...
   export BINANCE_TESTNET_API_SECRET=...
   ```

3. Review `config/testnet.json`, especially `live.capital_allocation` (capital
   the bot may use, 100 USDT) and `live.maximum_order_notional` (largest single
   buy, 50 USDT).
4. Run:

   ```bash
   ./build-linux/crypto-ai-trader --live-once config/testnet.json      # one cycle
   ./build-linux/crypto-ai-trader --live config/testnet.json           # keep running
   ./build-linux/crypto-ai-trader --live-status config/testnet.json
   ./build-linux/crypto-ai-trader --live-flatten config/testnet.json   # emergency: sell all, halt
   ./build-linux/crypto-ai-trader --live-reset config/testnet.json     # resume after a halt
   ```

### What you will see

Each cycle prints one line per pair with the decision and the reason, then the
account and any open positions with their stop and unrealized P/L:

```text
BTCUSDT       77988.03  WAIT  Downtrend: not buying (open positions here are sold)
ETHUSDT        2266.97  WAIT  Uptrend: waiting for breakout above 2391.55

Testnet Account Allocation=100.00 Cash=100.00 Equity=100.00 Return=0.00% ... Trading=ENABLED
```

The bot wakes every 60 seconds and 3 seconds after each 4h candle closes.
New buy signals can only appear when a 4h candle closes, and stop-losses rest
on the exchange and trigger immediately, so faster polling would not change
results. Expect long quiet periods: in downtrends it deliberately stays in cash.

Everything is also written to `logs/paper.log` or `logs/testnet.log`:

```bash
tail -f logs/testnet.log
```

Testnet orders appear on the testnet website under open orders and trade
history.

### Phone alerts (optional)

Create a Telegram bot with @BotFather, then set:

```bash
export TELEGRAM_BOT_TOKEN=...
export TELEGRAM_CHAT_ID=...
```

The bot then messages you on start-up and for every buy, sell, halt, and error.

### Run 24/7

Use the systemd user service so the bot survives logouts, crashes, and reboots:

```bash
mkdir -p ~/.config/systemd/user ~/.config/crypto-ai-trader
cp deploy/crypto-ai-trader.service ~/.config/systemd/user/
cat > ~/.config/crypto-ai-trader/testnet.env <<'EOF'
BINANCE_TESTNET_API_KEY=...
BINANCE_TESTNET_API_SECRET=...
TELEGRAM_BOT_TOKEN=...
TELEGRAM_CHAT_ID=...
EOF
chmod 600 ~/.config/crypto-ai-trader/testnet.env
systemctl --user daemon-reload
systemctl --user enable --now crypto-ai-trader
loginctl enable-linger "$USER"
journalctl --user -u crypto-ai-trader -f     # live output
```

Edit `WorkingDirectory`/`ExecStart` in the service file if the repository is
not at `~/git/crypto-ai-trader-main`.

## Research commands

```bash
./build-linux/crypto-ai-trader --backtest-all          # shared-account portfolio backtest
./build-linux/crypto-ai-trader --backtest BTCUSDT      # one pair with its trade list
./build-linux/crypto-ai-trader --scan-once             # full indicator detail, no trading
./build-linux/crypto-ai-trader --ml-evaluate BTCUSDT   # offline ML holdout metrics
./build-linux/crypto-ai-trader --candles BTCUSDT
./build-linux/crypto-ai-trader --stream BTCUSDT
./build-linux/crypto-ai-trader --execution-status
./build-linux/crypto-ai-trader --execution-dry-run BTCUSDT buy 0.001
```

Backtests page `backtest.candle_count` closed candles (6500 ≈ 3 years of 4h),
compute indicators over the same `candle_limit` window a live scan sees, enter
at the next bar open, assume the stop fills first when a bar touches both stop
and target, and charge `backtest.spread_fraction` on every fill.

## Key settings

| Setting | Default | Meaning |
|---|---|---|
| `candle_interval` | `4h` | Signal timeframe. |
| `risk.risk_per_trade_fraction` | 0.0025 | Equity risked between entry and initial stop. 0.005 roughly doubles both profit and drawdown. |
| `risk.maximum_open_positions` | 3 | Concurrent positions. |
| `risk.daily_maximum_loss_fraction` | 0.02 | Daily loss that pauses new entries until the next UTC day. |
| `risk.maximum_drawdown_fraction` | 0.10 | Drawdown that halts trading and sells everything. |
| `risk.stop_atr_multiple` / `trailing_stop_atr_multiple` / `target_atr_multiple` | 2 / 4 / 10 | Initial stop, trailing distance, take-profit in ATRs. |
| `risk.breakeven_trigger_r` | 1.5 | Profit, in multiples of initial risk, that moves the stop to break-even. |
| `live.capital_allocation` | 100 | Capital the testnet bot manages. Applies when its state file is first created. |
| `live.maximum_order_notional` | 50 | Largest single entry. |
| `paper_initial_balance` | 100 | Paper capital. |

Invalid or inconsistent settings and corrupted state files are rejected instead
of silently reset. To change the allocation after trading has started, back up
and delete the state file first.

## Dependencies

- C++20 compiler, CMake 3.24+
- libcurl, OpenSSL, Boost.Asio/Beast/System (`vcpkg.json` on Windows)
- nlohmann/json, spdlog, GoogleTest (fetched by CMake)
