# Crypto AI Trader

Automated C++20 trend-following bot for Binance spot. It buys breakouts in
established uptrends, protects every position with a stop-loss order resting on
the exchange, and trails that stop to lock in profit. It runs in **paper mode**
(simulated fills on live prices), on **Binance Spot Testnet** (real orders, no
real money), or on **Binance mainnet with real money** once a separate config
explicitly acknowledges it.

See [ARCHITECTURE.md](./ARCHITECTURE.md) for the design, capital-protection
layers, strategy rules, and backtest evidence.

## Results at a glance

Portfolio backtest of BTC, ETH, SOL, BNB, and XRP from one shared account, 4h
candles, Nov 2023 – Nov 2026, realistic fees and slippage, default 0.25% risk
per trade:

| 3-year return | Buy-and-hold | Max drawdown | Bear year (Nov 2025 – Nov 2026) |
|---|---|---|---|
| +17.7% | +98% | 8.1% | −4.3% (buy-and-hold −34.5%) |

On Jan 2020 – Nov 2026, which includes about four years the settings were
never tuned on, the unchanged strategy returned +47% with an 8.1% maximum
drawdown (2022 crash year: −3.9%). Faster timeframes, a pullback entry, and
doubled risk all did worse; see ARCHITECTURE.md.

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
   the bot may use, 250 USDT) and `live.maximum_order_notional` (largest single
   buy, 100 USDT).
4. Run:

   ```bash
   ./build-linux/crypto-ai-trader --live-once config/testnet.json      # one cycle
   ./build-linux/crypto-ai-trader --live config/testnet.json           # keep running
   ./build-linux/crypto-ai-trader --live-status config/testnet.json
   ./build-linux/crypto-ai-trader --live-flatten config/testnet.json   # emergency: sell all, halt
   ./build-linux/crypto-ai-trader --live-reset config/testnet.json     # resume after a halt
   ./build-linux/crypto-ai-trader --live-selftest config/testnet.json  # order round-trip check
   ```

### Mainnet (real money)

Real-money trading uses `config/mainnet.json`, its own ledger
(`data/mainnet_portfolio.json`), its own log (`logs/mainnet.log`), and its own
keys. The shipped file refuses to start until you set
`live.real_money_acknowledged` to `true`.

Go-live checklist:

1. **Rehearse on testnet first** with the same sizing (`config/testnet.json`
   now uses the same 250 USDT allocation). Start with
   `--live-selftest config/testnet.json`: it runs one minimum-size round trip
   (buy, lookup by client order id, resting stop, stop replacement, cancel,
   sell) outside the bot ledger and cleans up after itself. Testnet charges no
   fees, so fee handling is only exercised on mainnet. Run it 24/7 for at least a week and
   confirm that buys get a resting stop, stops ratchet, exits fill, and
   `--live-flatten` works.
2. **Fund a dedicated account.** Ideally use a Binance sub-account that holds
   only the money the bot may lose. The bot trades only its ledger, but a
   separate account also caps the damage from a bug.
3. **Create a restricted API key.** Enable *Reading* and *Spot & Margin
   Trading* only. Never enable withdrawals, and restrict the key to this
   machine's IP. Before every mainnet session the bot checks the key and
   refuses to trade if it can withdraw or cannot trade spot. It warns if the
   key has no IP restriction or has permissions it does not need.
4. **Size it deliberately.** `config/testnet.json` and `config/mainnet.json`
   risk 0.5% per trade, with a 4% daily pause and a 20% drawdown kill switch.
   In the 2020–2026 backtest that level made about 11.6% a year, with a 15.6%
   worst drop and a −8.4% worst year. Expect future drops to exceed the
   backtest's. At the current 4h volatility (ATR 2–3%) each entry is about 9%
   of the allocation. Binance rejects orders under 5 USDT, so allocations
   below about 80 USDT skip most signals. At 250 USDT each trade risks about
   1.25 USDT, entries are about 24 USDT, and the kill switch trips at about
   50 USDT lost.
5. **Export keys and acknowledge.** Keys are read from the environment only:

   ```bash
   export BINANCE_API_KEY=...
   export BINANCE_API_SECRET=...
   # edit config/mainnet.json: "real_money_acknowledged": true
   ./build-linux/crypto-ai-trader --live-selftest config/mainnet.json  # ~8 USDT round trip, ~0.02 USDT fees
   ./build-linux/crypto-ai-trader --live-once config/mainnet.json   # one cycle
   ./build-linux/crypto-ai-trader --live config/mainnet.json        # keep running
   ./build-linux/crypto-ai-trader --live-status config/mainnet.json
   ./build-linux/crypto-ai-trader --live-flatten config/mainnet.json  # emergency
   ```

6. **Run it unattended** with `deploy/crypto-ai-trader-mainnet.service` and
   `~/.config/crypto-ai-trader/mainnet.env` (same steps as "Run 24/7" below),
   with Telegram alerts set up.

If the bot reports an *unconfirmed entry* or halts after an unknown order
outcome, it keeps looking the order up every cycle and protects it with a stop
if it filled. Check the account on Binance before running `--live-reset`.

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

### Dashboard

A read-only web page shows what the bot is worth, profit and loss, the daily
and drawdown limits, an equity chart, profit per trade, open positions, the
latest market check, and recent warnings or errors. It never places or
changes orders, and it needs only Python 3 (no packages):

```bash
python3 scripts/dashboard.py                    # http://127.0.0.1:8765 (this computer only)
bash deploy/install-dashboard.sh                # run it 24/7 as a user service
bash deploy/install-dashboard.sh --lan          # also reachable from your phone on the same Wi-Fi
python3 scripts/profit_report.py                # the same numbers in the terminal
```

With `--lan`, the page requires a secret token (saved in
`~/.config/crypto-ai-trader/dashboard.token`); open the printed link. Anyone
with the link can see your balances. The equity chart fills in while the
dashboard runs: one point every 15 minutes and after every trade, stored next
to the ledger as `*.equity.csv`.

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
| `risk.risk_per_trade_fraction` | 0.0025 | Equity risked between entry and initial stop. The testnet and mainnet configs use 0.005 (with a 4% daily pause and a 20% kill switch), which roughly doubles both profit and drawdown. |
| `risk.maximum_open_positions` | 3 | Concurrent positions. |
| `risk.daily_maximum_loss_fraction` | 0.02 | Daily loss that pauses new entries until the next UTC day. |
| `risk.maximum_drawdown_fraction` | 0.10 | Drawdown that halts trading and sells everything. |
| `risk.stop_atr_multiple` / `trailing_stop_atr_multiple` / `target_atr_multiple` | 2 / 4 / 10 | Initial stop, trailing distance, take-profit in ATRs. |
| `risk.breakeven_trigger_r` | 1.5 | Profit, in multiples of initial risk, that moves the stop to break-even. |
| `live.capital_allocation` | 250 | Capital the testnet/mainnet bot manages. Applies when its state file is first created. |
| `live.maximum_order_notional` | 100 | Largest single entry. |
| `live.real_money_acknowledged` | false | Must be `true` for mode `mainnet`; ignored otherwise. |
| `paper_initial_balance` | 100 | Paper capital. |

Invalid or inconsistent settings and corrupted state files are rejected instead
of silently reset. To change the allocation after trading has started, back up
and delete the state file first.

## Dependencies

- C++20 compiler, CMake 3.24+
- libcurl, OpenSSL, Boost.Asio/Beast/System (`vcpkg.json` on Windows)
- nlohmann/json, spdlog, GoogleTest (fetched by CMake)
