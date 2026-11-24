#!/usr/bin/env python3
"""Is the bot making money? Reads the bot's own ledger (read-only, no network).

Usage, from the repository root:
    python3 scripts/profit_report.py                       # testnet
    python3 scripts/profit_report.py config/mainnet.json   # mainnet
"""
import datetime
import json
import sys
from collections import defaultdict
from pathlib import Path


def main() -> int:
    config_path = Path(sys.argv[1] if len(sys.argv) > 1 else "config/testnet.json")
    config = json.loads(config_path.read_text())
    live = config.get("live", {})
    ledger_path = Path(live.get("state_path", "data/testnet_portfolio.json"))
    if not ledger_path.exists():
        print(f"No ledger yet at {ledger_path}: the bot has not run with {config_path}.")
        return 1
    ledger = json.loads(ledger_path.read_text())
    kill_switch = config.get("risk", {}).get("maximum_drawdown_fraction", 0.10)
    risk_per_trade = config.get("risk", {}).get("risk_per_trade_fraction", 0.0025)
    # 2020-2026 portfolio backtest at each tested risk level: (per year %, profit factor).
    expected = {0.0025: (5.8, 1.56), 0.005: (11.6, 1.52), 0.0075: (17.3, 1.48), 0.01: (22.0, 1.45)}
    yearly, backtest_pf = expected.get(round(risk_per_trade, 4), (None, None))
    quote = live.get("quote_asset", "USDT")

    start = ledger["capital_allocation"]
    positions = ledger["positions"]
    open_value = sum(p["quantity"] * p["mark_price"] for p in positions)
    equity = ledger["cash_balance"] + open_value
    peak = max(ledger["peak_equity"], equity)
    profit = equity - start
    drawdown = (peak - equity) / peak if peak > 0 else 0.0

    sells = [e for e in ledger["executions"] if e["side"] == "SELL"]
    wins = [e["realized_pnl"] for e in sells if e["realized_pnl"] > 0]
    losses = [e["realized_pnl"] for e in sells if e["realized_pnl"] <= 0]
    realized = sum(wins) + sum(losses)
    fees = sum(e["fee"] for e in ledger["executions"])
    unrealized = sum(p["quantity"] * p["mark_price"] - p["entry_cost"] for p in positions)

    mode = config.get("mode", "?").upper()
    verdict = "MAKING MONEY" if profit > 0 else "LOSING MONEY" if profit < 0 else "FLAT (no change)"
    print(f"=== {mode} bot: {verdict} ===")
    print(f"Started with      {start:12.2f} {quote}")
    print(f"Worth now         {equity:12.2f} {quote}   ({profit:+.2f} {quote}, {profit / start * 100:+.2f}%)")
    print(f"  realized        {realized:+12.2f} {quote}   from closed trades (after fees)")
    print(f"  unrealized      {unrealized:+12.2f} {quote}   on {len(positions)} open position(s)")
    print(f"Fees paid         {fees:12.4f} {quote}")
    print(f"Drop from peak    {drawdown * 100:11.2f}%    (trading halts at {kill_switch * 100:.0f}%)")
    print(f"Trading           {'HALTED - check before --live-reset' if ledger['trading_halted'] else 'enabled'}")
    if ledger.get("pending_entry"):
        print(f"Unconfirmed entry {ledger['pending_entry']['symbol']} (being checked every cycle)")

    print(f"\nClosed trades     {len(sells)}  (wins {len(wins)}, losses {len(losses)})")
    if sells:
        print(f"Win rate          {len(wins) / len(sells) * 100:.0f}%")
        if wins:
            print(f"Average win       {sum(wins) / len(wins):+.2f} {quote}   best {max(wins):+.2f}")
        if losses:
            print(f"Average loss      {sum(losses) / len(losses):+.2f} {quote}   worst {min(losses):+.2f}")
        if losses and sum(losses) < 0:
            reference = f"; backtest {backtest_pf:.2f}" if backtest_pf else ""
            print(f"Profit factor     {sum(wins) / -sum(losses):.2f}   (above 1.0 = winners outweigh losers"
                  f"{reference})")
        by_month = defaultdict(float)
        for e in sells:
            month = datetime.datetime.fromtimestamp(e["timestamp_ms"] / 1000, datetime.timezone.utc)
            by_month[month.strftime("%Y-%m")] += e["realized_pnl"]
        print("Realized by month " + "  ".join(f"{m}: {v:+.2f}" for m, v in sorted(by_month.items())))

    if positions:
        print("\nOpen positions")
        for p in positions:
            pnl = p["quantity"] * p["mark_price"] - p["entry_cost"]
            print(f"  {p['symbol']:9} qty {p['quantity']:.6g}  bought {p['entry_price']:.6g}"
                  f"  now {p['mark_price']:.6g}  stop {p['stop_loss_price']:.6g}  P/L {pnl:+.2f}")

    print("\nRecent trades")
    for e in ledger["executions"][-10:] or []:
        when = datetime.datetime.fromtimestamp(e["timestamp_ms"] / 1000, datetime.timezone.utc)
        pnl = f"  P/L {e['realized_pnl']:+.2f}" if e["side"] == "SELL" else ""
        print(f"  {when:%Y-%m-%d %H:%M} UTC  {e['side']:4} {e['symbol']:9} @ {e['price']:.6g}"
              f"  ({e['reason']}){pnl}")
    if not ledger["executions"]:
        print("  none yet: the bot only buys breakouts in uptrends and waits in cash otherwise")

    if len(sells) < 30:
        expectation = (f" Backtest expectation at {risk_per_trade * 100:g}% risk per trade:"
                       f" about +{yearly:g}% a year." if yearly else "")
        print(f"\nToo early to judge: {len(sells)} closed trade(s). About 4 in 10 trades win, so"
              f" results swing a lot until roughly 30 trades.{expectation}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
