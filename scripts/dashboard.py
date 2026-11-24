#!/usr/bin/env python3
"""Read-only web dashboard for the trading bot (standard library only).

Shows what the bot is worth, profit and loss, risk limits, open positions,
trades, the latest market check, and recent problems. It never places or
changes orders: it only reads the bot's ledger, its log file, and systemd.

Usage, from the repository root:
    python3 scripts/dashboard.py                        # testnet, http://127.0.0.1:8765
    python3 scripts/dashboard.py config/mainnet.json    # mainnet ledger
    python3 scripts/dashboard.py --host 0.0.0.0         # also reachable on your Wi-Fi

When reachable from other devices it requires a secret token, stored in
~/.config/crypto-ai-trader/dashboard.token and printed at start-up.
"""
from __future__ import annotations

import argparse
import hmac
import http.server
import json
import math
import os
import re
import secrets
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LOG_LINE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})(?:\.\d+)?\] \[(\w+)\] (.*)$")
DECISION = re.compile(r"^([A-Z0-9]{2,20}) (BUY|WAIT) (.*)$")
# 2020-2026 portfolio backtest at each tested risk level: (per year %, profit factor).
EXPECTED = {0.0025: (5.8, 1.56), 0.005: (11.6, 1.52), 0.0075: (17.3, 1.48), 0.01: (22.0, 1.45)}
SAMPLE_EVERY_MS = 15 * 60 * 1000
MAX_CHART_POINTS = 800


def number(value, default=0.0):
    return float(value) if isinstance(value, (int, float)) and math.isfinite(value) else default


def read_tail(path: Path, max_bytes: int = 512 * 1024) -> list[str]:
    try:
        with open(path, "rb") as handle:
            handle.seek(0, os.SEEK_END)
            size = handle.tell()
            handle.seek(max(0, size - max_bytes))
            text = handle.read().decode("utf-8", "replace")
    except OSError:
        return []
    lines = text.splitlines()
    return lines[1:] if size > max_bytes else lines  # the first line may be cut


def local_ms(stamp: str) -> int:
    return int(time.mktime(time.strptime(stamp, "%Y-%m-%d %H:%M:%S")) * 1000)


class Dashboard:
    def __init__(self, config_path: str, service: str):
        self.config_path = Path(config_path)
        self.service = service
        self.lock = threading.Lock()
        self.history: list[list[float]] = []
        self.history_trades = -1
        self.service_cache = (0.0, "unknown")
        config = self.config()
        if config.get("mode") == "paper":
            ledger = config.get("paper_state_path", "data/paper_trader.json")
        else:
            ledger = config.get("live", {}).get("state_path", "data/testnet_portfolio.json")
        self.ledger_path = Path(ledger)
        self.history_path = self.ledger_path.with_name(self.ledger_path.stem + ".equity.csv")
        self.log_path = Path(config["log_file"]) if config.get("log_file") else None
        self.load_history()

    def config(self) -> dict:
        return json.loads(self.config_path.read_text())

    def load_history(self) -> None:
        try:
            for line in self.history_path.read_text().splitlines():
                stamp, value = line.split(",")
                self.history.append([int(stamp), float(value)])
        except (OSError, ValueError):
            pass

    def ledger(self) -> dict | None:
        try:
            return json.loads(self.ledger_path.read_text())
        except (OSError, ValueError):
            return None

    @staticmethod
    def account(ledger: dict) -> dict:
        positions = ledger.get("positions", [])
        open_value = sum(number(p["quantity"]) * number(p["mark_price"]) for p in positions)
        equity = number(ledger["cash_balance"]) + open_value
        start = number(ledger["capital_allocation"], 1.0)
        peak = max(number(ledger["peak_equity"]), equity)
        daily_start = number(ledger.get("daily_starting_equity"), start)
        sells = [e for e in ledger.get("executions", []) if e["side"] == "SELL"]
        return {
            "start": start,
            "equity": equity,
            "cash": number(ledger["cash_balance"]),
            "profit": equity - start,
            "profit_fraction": (equity - start) / start,
            "realized": sum(number(e["realized_pnl"]) for e in sells),
            "unrealized": sum(number(p["quantity"]) * number(p["mark_price"]) - number(p["entry_cost"])
                              for p in positions),
            "fees": sum(number(e["fee"]) for e in ledger.get("executions", [])),
            "peak": peak,
            "drawdown_fraction": (peak - equity) / peak if peak > 0 else 0.0,
            "daily_loss_fraction": max(0.0, daily_start - equity) / daily_start if daily_start > 0 else 0.0,
            "halted": bool(ledger.get("trading_halted")),
            "pending_entry": (ledger.get("pending_entry") or {}).get("symbol"),
            "open_positions": len(positions),
            "sells": sells,
        }

    def sample(self) -> None:
        """Records the account value every 15 minutes and after each trade."""
        ledger = self.ledger()
        if ledger is None:
            return
        account = self.account(ledger)
        trades = len(ledger.get("executions", []))
        now = int(time.time() * 1000)
        with self.lock:
            last = self.history[-1] if self.history else None
            if last is not None and now - last[0] < SAMPLE_EVERY_MS and trades == self.history_trades:
                return
            self.history.append([now, round(account["equity"], 6)])
            self.history_trades = trades
            try:
                with open(self.history_path, "a") as handle:
                    handle.write(f"{now},{account['equity']:.6f}\n")
            except OSError:
                pass

    def service_state(self) -> str:
        checked, state = self.service_cache
        if time.time() - checked < 10:
            return state
        try:
            result = subprocess.run(["systemctl", "--user", "is-active", self.service],
                                    capture_output=True, text=True, timeout=3)
            state = result.stdout.strip() or "unknown"
        except (OSError, subprocess.SubprocessError):
            state = "unknown"
        self.service_cache = (time.time(), state)
        return state

    def log_summary(self) -> dict:
        markets: dict[str, dict] = {}
        problems: list[dict] = []
        last_cycle = None
        for line in read_tail(self.log_path) if self.log_path else []:
            match = LOG_LINE.match(line)
            if not match:
                continue
            stamp, level, message = match.groups()
            try:
                when = local_ms(stamp)
            except ValueError:
                continue
            decision = DECISION.match(message)
            if level == "info" and decision:
                symbol, action, reason = decision.groups()
                markets[symbol] = {"symbol": symbol, "action": action, "reason": reason, "t": when}
                last_cycle = when
            elif level in ("warning", "error", "critical"):
                problems.append({"t": when, "level": level, "message": message[:400]})
        return {"markets": markets, "problems": problems[-15:], "last_cycle_ms": last_cycle}

    def status(self) -> dict:
        config = self.config()
        risk = config.get("risk", {})
        risk_per_trade = number(risk.get("risk_per_trade_fraction"), 0.0025)
        yearly, profit_factor = EXPECTED.get(round(risk_per_trade, 4), (None, None))
        logs = self.log_summary()
        pairs = config.get("trading_pairs", [])
        result = {
            "mode": config.get("mode", "paper"),
            "now_ms": int(time.time() * 1000),
            "service": self.service_state(),
            "last_cycle_ms": logs["last_cycle_ms"],
            "markets": [logs["markets"].get(p, {"symbol": p, "action": None}) for p in pairs],
            "problems": logs["problems"],
            "limits": {
                "risk_per_trade": risk_per_trade,
                "daily": number(risk.get("daily_maximum_loss_fraction"), 0.02),
                "drawdown": number(risk.get("maximum_drawdown_fraction"), 0.10),
            },
            "expected": {"yearly_percent": yearly, "profit_factor": profit_factor},
        }
        ledger = self.ledger()
        if ledger is None:
            result["error"] = f"No ledger yet at {self.ledger_path}: start the bot first."
            return result
        account = self.account(ledger)
        sells = account.pop("sells")
        wins = [number(e["realized_pnl"]) for e in sells if number(e["realized_pnl"]) > 0]
        losses = [number(e["realized_pnl"]) for e in sells if number(e["realized_pnl"]) <= 0]
        result["account"] = account
        result["stats"] = {
            "closed": len(sells),
            "wins": len(wins),
            "losses": len(losses),
            "win_rate": len(wins) / len(sells) if sells else None,
            "profit_factor": sum(wins) / -sum(losses) if losses and sum(losses) < 0 else None,
        }
        result["positions"] = [{
            "symbol": p["symbol"],
            "quantity": number(p["quantity"]),
            "entry": number(p["entry_price"]),
            "mark": number(p["mark_price"]),
            "stop": number(p["stop_loss_price"]),
            "target": number(p["take_profit_price"]),
            "pnl": number(p["quantity"]) * number(p["mark_price"]) - number(p["entry_cost"]),
            "t": int(p.get("opened_at_ms", 0)),
        } for p in ledger.get("positions", [])]
        result["trades"] = [{
            "t": int(e["timestamp_ms"]),
            "side": e["side"],
            "symbol": e["symbol"],
            "price": number(e["price"]),
            "quantity": number(e["quantity"]),
            "pnl": number(e["realized_pnl"]) if e["side"] == "SELL" else None,
            "reason": e["reason"],
        } for e in reversed(ledger.get("executions", [])[-15:])]
        result["trade_pnl"] = [{
            "t": int(e["timestamp_ms"]), "symbol": e["symbol"],
            "pnl": number(e["realized_pnl"]), "reason": e["reason"],
        } for e in sells]
        with self.lock:
            history = list(self.history)
        if len(history) > MAX_CHART_POINTS:
            stride = math.ceil(len(history) / MAX_CHART_POINTS)
            history = history[::stride] + [history[-1]]
        history.append([result["now_ms"], account["equity"]])
        result["equity_history"] = history
        return result


def make_handler(dashboard: Dashboard, token: str | None):
    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "trader-dashboard"
        sys_version = ""

        def log_message(self, *_):
            pass  # request lines can carry the token; never log them

        def send(self, code: int, body: bytes, content_type: str, csp: str | None = None):
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("X-Frame-Options", "DENY")
            if csp:
                self.send_header("Content-Security-Policy", csp)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            if token is not None:
                given = urllib.parse.parse_qs(url.query).get("token", [""])[0]
                if not hmac.compare_digest(given.encode(), token.encode()):
                    self.send(403, b"Forbidden: open the URL with ?token=... printed at start-up\n",
                              "text/plain; charset=utf-8")
                    return
            if url.path == "/":
                nonce = secrets.token_urlsafe(16)
                csp = (f"default-src 'none'; script-src 'nonce-{nonce}'; style-src 'nonce-{nonce}'; "
                       "connect-src 'self'; img-src 'self' data:; base-uri 'none'; form-action 'none'; "
                       "frame-ancestors 'none'")
                self.send(200, PAGE.replace("__NONCE__", nonce).encode(), "text/html; charset=utf-8", csp)
            elif url.path == "/api/status":
                try:
                    body = json.dumps(dashboard.status(), allow_nan=False).encode()
                except Exception as error:  # report, never crash the server
                    body = json.dumps({"error": f"Dashboard could not read the bot data: {error}"}).encode()
                self.send(200, body, "application/json")
            else:
                self.send(404, b"Not found\n", "text/plain; charset=utf-8")

    return Handler


def load_token() -> str:
    path = Path.home() / ".config" / "crypto-ai-trader" / "dashboard.token"
    if path.exists() and path.read_text().strip():
        return path.read_text().strip()
    path.parent.mkdir(parents=True, exist_ok=True)
    token = secrets.token_urlsafe(18)
    old_umask = os.umask(0o077)
    try:
        path.write_text(token + "\n")
    finally:
        os.umask(old_umask)
    return token


def lan_address() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(("192.0.2.1", 9))  # UDP connect sends nothing; it picks the route
            return probe.getsockname()[0]
    except OSError:
        return "<this-computer-ip>"


def main() -> int:
    parser = argparse.ArgumentParser(description="Read-only dashboard for the trading bot")
    parser.add_argument("config", nargs="?", default="config/testnet.json")
    parser.add_argument("--host", default="127.0.0.1",
                        help="127.0.0.1 = this computer only (default); 0.0.0.0 = also your Wi-Fi")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--service", default="crypto-ai-trader", help="systemd user service of the bot")
    args = parser.parse_args()
    os.chdir(REPO)
    loopback = args.host in ("127.0.0.1", "localhost", "::1")
    token = None if loopback else load_token()
    dashboard = Dashboard(args.config, args.service)

    def sampler():
        while True:
            try:
                dashboard.sample()
            except Exception as error:
                print(f"equity sample failed: {error}", file=sys.stderr, flush=True)
            time.sleep(60)

    threading.Thread(target=sampler, daemon=True).start()
    server = http.server.ThreadingHTTPServer((args.host, args.port), make_handler(dashboard, token))
    if loopback:
        print(f"Dashboard: http://127.0.0.1:{args.port}/  (this computer only)", flush=True)
    else:
        shown_host = lan_address() if args.host in ("0.0.0.0", "::") else args.host
        print(f"Dashboard: http://{shown_host}:{args.port}/?token={token}", flush=True)
        print("Anyone with this link on your network can see your balances; keep it private.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Trader Dashboard</title>
<style nonce="__NONCE__">
:root {
  color-scheme: light;
  --page: #f9f9f7; --surface: #fcfcfb; --ink: #0b0b0b; --ink-2: #52514e; --muted: #898781;
  --grid: #e1e0d9; --axis: #c3c2b7; --border: rgba(11,11,11,0.10);
  --series: #2a78d6; --gain: #2a78d6; --loss: #e34948;
  --good: #0ca30c; --warning: #fab219; --critical: #d03b3b; --up-text: #006300; --down-text: #d03b3b;
}
@media (prefers-color-scheme: dark) {
  :root:where(:not([data-theme="light"])) {
    color-scheme: dark;
    --page: #0d0d0d; --surface: #1a1a19; --ink: #f0efec; --ink-2: #c3c2b7; --muted: #898781;
    --grid: #2c2c2a; --axis: #383835; --border: rgba(255,255,255,0.10);
    --series: #3987e5; --gain: #3987e5; --loss: #e66767; --up-text: #0ca30c; --down-text: #e66767;
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --page: #0d0d0d; --surface: #1a1a19; --ink: #f0efec; --ink-2: #c3c2b7; --muted: #898781;
  --grid: #2c2c2a; --axis: #383835; --border: rgba(255,255,255,0.10);
  --series: #3987e5; --gain: #3987e5; --loss: #e66767; --up-text: #0ca30c; --down-text: #e66767;
}
* { box-sizing: border-box; }
figure { margin: 0; }
body { margin: 0; background: var(--page); color: var(--ink);
  font: 14px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
main { max-width: 1200px; margin: 0 auto; padding: 16px; transition: opacity .2s; }
body.loading main { opacity: .6; }
header { display: flex; flex-wrap: wrap; align-items: baseline; gap: 8px 16px; margin-bottom: 12px; }
h1 { font-size: 20px; margin: 0; font-weight: 650; }
h2 { font-size: 14px; margin: 0 0 10px; font-weight: 600; color: var(--ink); }
.badge { font-size: 12px; font-weight: 600; padding: 2px 8px; border-radius: 999px;
  border: 1px solid var(--border); color: var(--ink-2); }
.updated { color: var(--muted); font-size: 12px; margin-left: auto; }
.banner { border: 1px solid var(--border); border-radius: 12px; padding: 10px 14px; margin-bottom: 12px;
  background: var(--surface); }
.pills { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 12px; }
.pill { display: flex; align-items: center; gap: 8px; background: var(--surface); border: 1px solid var(--border);
  border-radius: 999px; padding: 5px 12px 5px 6px; color: var(--ink); }
.icon { width: 18px; height: 18px; border-radius: 50%; display: inline-grid; place-items: center;
  font-size: 11px; font-weight: 800; color: #fff; flex: none; }
.icon.good { background: var(--good); } .icon.critical { background: var(--critical); }
.icon.warning { background: var(--warning); color: #0b0b0b; }
.grid-top { display: grid; grid-template-columns: minmax(260px, 1.2fr) 2fr; gap: 12px; margin-bottom: 12px; }
.card { background: var(--surface); border: 1px solid var(--border); border-radius: 12px; padding: 16px; min-width: 0; }
.hero-label, .tile-label { color: var(--ink-2); font-size: 13px; }
.hero-value { font-size: 52px; font-weight: 650; line-height: 1.1; margin: 4px 0; letter-spacing: -0.5px; }
.hero-unit { font-size: 18px; font-weight: 500; color: var(--ink-2); margin-left: 6px; }
.delta { font-weight: 600; font-size: 15px; }
.delta.up { color: var(--up-text); } .delta.down { color: var(--down-text); } .delta.flat { color: var(--ink-2); }
.sub { color: var(--muted); font-size: 12px; }
.tiles { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 12px; }
.tile { border: 1px solid var(--border); border-radius: 10px; padding: 10px 12px; }
.tile-value { font-size: 22px; font-weight: 600; margin-top: 2px; }
.meters { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 12px; margin-bottom: 12px; }
.meter-head { display: flex; justify-content: space-between; gap: 8px; align-items: baseline; }
.meter-track { height: 8px; border-radius: 999px; margin: 10px 0 6px; overflow: hidden; }
.meter-fill { height: 100%; border-radius: 999px; min-width: 2px; }
.charts { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 12px; margin-bottom: 12px; }
.chart-head { display: flex; justify-content: space-between; align-items: baseline; gap: 8px; flex-wrap: wrap; }
.chart-host { position: relative; width: 100%; min-height: 60px; }
.legend { display: flex; gap: 14px; color: var(--ink-2); font-size: 12px; margin: -4px 0 6px; }
.legend i { display: inline-block; width: 10px; height: 10px; border-radius: 2px; margin-right: 6px; vertical-align: -1px; }
.legend .gain { background: var(--gain); } .legend .loss { background: var(--loss); }
button.link { background: none; border: 1px solid var(--border); color: var(--ink-2); border-radius: 8px;
  padding: 3px 10px; font: inherit; font-size: 12px; cursor: pointer; }
button.link:hover { color: var(--ink); }
.empty { color: var(--muted); margin: 18px 0; }
svg { display: block; overflow: visible; }
svg .grid { stroke: var(--grid); stroke-width: 1; shape-rendering: crispEdges; }
svg .axis, svg .ref { stroke: var(--axis); stroke-width: 1; shape-rendering: crispEdges; }
svg .tick, svg .ref-label { fill: var(--muted); font-size: 11px; font-variant-numeric: tabular-nums; }
svg .line { fill: none; stroke: var(--series); stroke-width: 2; stroke-linejoin: round; stroke-linecap: round; }
svg .area { fill: var(--series); opacity: .10; }
svg .dot { fill: var(--series); stroke: var(--surface); stroke-width: 2; }
svg .end-label { fill: var(--ink); font-size: 12px; font-weight: 600; }
svg .value-label { fill: var(--ink-2); font-size: 11px; font-variant-numeric: tabular-nums; }
svg .crosshair { stroke: var(--muted); stroke-width: 1; }
svg .bar.gain { fill: var(--gain); } svg .bar.loss { fill: var(--loss); } svg .bar.hover { opacity: .7; }
svg .hit { fill: transparent; outline: none; } svg .hit:focus-visible { stroke: var(--ink-2); stroke-width: 1; }
.tip { position: absolute; pointer-events: none; background: var(--surface); border: 1px solid var(--border);
  border-radius: 8px; padding: 6px 9px; font-size: 12px; box-shadow: 0 4px 14px rgba(0,0,0,.12);
  max-width: 260px; z-index: 2; }
.tip strong { display: block; font-size: 14px; color: var(--ink); }
.tip span { display: block; color: var(--ink-2); }
.table-wrap { overflow-x: auto; }
table { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
th, td { text-align: left; padding: 7px 10px 7px 0; border-bottom: 1px solid var(--grid); white-space: nowrap; }
th { color: var(--ink-2); font-weight: 600; font-size: 12px; }
td.num, th.num { text-align: right; }
td.wrap { white-space: normal; min-width: 220px; }
.sections { display: grid; gap: 12px; }
footer { color: var(--muted); font-size: 12px; margin: 16px 0 8px; }
@media (max-width: 760px) {
  .grid-top { grid-template-columns: 1fr; }
  .hero-value { font-size: 44px; }
  .updated { margin-left: 0; width: 100%; }
}
</style>
</head>
<body>
<main>
  <header>
    <h1>Crypto AI Trader</h1>
    <span class="badge" id="mode">…</span>
    <span class="updated" id="updated"></span>
  </header>
  <div class="banner" id="offline" hidden></div>
  <div class="banner" id="error" hidden></div>
  <div class="pills" id="pills"></div>
  <section class="grid-top">
    <div class="card" id="hero"></div>
    <div class="card"><div class="tiles" id="tiles"></div></div>
  </section>
  <section class="meters" id="meters"></section>
  <section class="charts">
    <figure class="card" id="equity-card">
      <div class="chart-head"><h2>Account value over time (USDT)</h2><button class="link" data-table="equity">Show table</button></div>
      <div class="chart-host" id="equity-chart"></div>
      <div class="table-wrap" id="equity-table" hidden></div>
    </figure>
    <figure class="card" id="trades-card">
      <div class="chart-head"><h2>Profit or loss per closed trade (USDT)</h2><button class="link" data-table="trades">Show table</button></div>
      <div class="legend"><span><i class="gain"></i>Gain</span><span><i class="loss"></i>Loss</span></div>
      <div class="chart-host" id="trades-chart"></div>
      <div class="table-wrap" id="trades-table" hidden></div>
    </figure>
  </section>
  <div class="sections">
    <section class="card"><h2>Latest market check</h2><div class="table-wrap" id="markets"></div></section>
    <section class="card"><h2>Open positions</h2><div class="table-wrap" id="positions"></div></section>
    <section class="card"><h2>Recent orders (last 15)</h2><div class="table-wrap" id="recent"></div></section>
    <section class="card"><h2>Recent problems (warnings and errors in the log)</h2><div class="table-wrap" id="problems"></div></section>
  </div>
  <footer id="footer"></footer>
</main>
<script nonce="__NONCE__">
"use strict";
const SVG_NS = "http://www.w3.org/2000/svg";
const $ = (id) => document.getElementById(id);
function el(tag, attrs, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs || {})) {
    if (key === "class") node.className = value;
    else if (key === "text") node.textContent = value;
    else node.setAttribute(key, value);
  }
  for (const child of children) if (child != null) node.append(child);
  return node;
}
function svg(tag, attrs) {
  const node = document.createElementNS(SVG_NS, tag);
  for (const [key, value] of Object.entries(attrs || {})) node.setAttribute(key, value);
  return node;
}
function svgText(attrs, text) { const node = svg("text", attrs); node.textContent = text; return node; }

const money = (v) => Math.abs(v) >= 10000
  ? v.toLocaleString(undefined, { maximumFractionDigits: 0 })
  : v.toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
const signed = (v) => (v > 0.0049 ? "+" : v < -0.0049 ? "−" : "") + money(Math.abs(v));
const pct = (f, digits = 2) => (f * 100).toFixed(digits) + "%";
const qty = (v) => Number(v.toPrecision(6)).toString();
const price = (v) => v >= 100 ? money(v) : Number(v.toPrecision(6)).toString();
const when = (ms) => new Date(ms).toLocaleString(undefined, { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });
function ago(seconds) {
  if (seconds < 90) return Math.max(0, Math.round(seconds)) + " s ago";
  if (seconds < 5400) return Math.round(seconds / 60) + " min ago";
  if (seconds < 172800) return Math.round(seconds / 3600) + " h ago";
  return Math.round(seconds / 86400) + " days ago";
}

function pill(state, label) {
  const glyph = state === "good" ? "✓" : state === "warning" ? "!" : "✕";
  return el("span", { class: "pill" }, el("span", { class: "icon " + state, "aria-hidden": "true", text: glyph }),
            el("span", { text: label }));
}

function table(headers, rows, numeric = []) {
  if (!rows.length) return null;
  const head = el("tr", {}, ...headers.map((h, i) => el("th", { class: numeric.includes(i) ? "num" : "", text: h })));
  const body = rows.map((row) => el("tr", {}, ...row.map((cell, i) =>
    el("td", { class: numeric.includes(i) ? "num" : (i === row.length - 1 && headers.wrapLast ? "wrap" : ""), text: cell }))));
  return el("table", {}, el("thead", {}, head), el("tbody", {}, ...body));
}
function fill(id, node, emptyText) {
  $(id).replaceChildren(node || el("p", { class: "empty", text: emptyText }));
}

function niceTicks(lo, hi, count) {
  const raw = (hi - lo) / count;
  const magnitude = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((f) => f * magnitude).find((s) => s >= raw);
  const ticks = [];
  for (let v = Math.floor(lo / step) * step; v < hi + step * 0.999; v += step) ticks.push(+v.toFixed(10));
  return ticks;
}
function tickLabel(v, step, withSign) {
  const digits = Math.min(2, Math.max(0, -Math.floor(Math.log10(step) + 1e-9)));
  const text = Math.abs(v) >= 10000 ? (v / 1000).toFixed(0) + "K"
    : Math.abs(v).toLocaleString(undefined, { minimumFractionDigits: digits, maximumFractionDigits: digits });
  return (v < 0 ? "−" : withSign && v > 0 ? "+" : "") + text;
}

function tooltip(host) {
  let tip = host.querySelector(".tip");
  if (!tip) { tip = el("div", { class: "tip", hidden: "" }); host.append(tip); }
  return {
    show(x, y, strong, ...lines) {
      tip.replaceChildren(el("strong", { text: strong }), ...lines.map((l) => el("span", { text: l })));
      tip.hidden = false;
      const w = tip.offsetWidth, h = tip.offsetHeight;
      tip.style.left = Math.min(Math.max(0, x + 12), host.clientWidth - w) + "px";
      tip.style.top = Math.max(0, y - h - 10) + "px";
    },
    hide() { tip.hidden = true; },
  };
}

function drawEquity(host, points, start) {
  host.replaceChildren();
  if (points.length < 3) {
    host.append(el("p", { class: "empty", text:
      "The line fills in while the dashboard runs: one point every 15 minutes and after every trade." }));
    return;
  }
  const W = Math.max(280, host.clientWidth), H = 240, m = { l: 52, r: 78, t: 14, b: 26 };
  const pw = W - m.l - m.r, ph = H - m.t - m.b;
  const t0 = points[0][0], t1 = points[points.length - 1][0];
  const values = points.map((p) => p[1]).concat([start]);
  let lo = Math.min(...values), hi = Math.max(...values);
  if (hi - lo < 0.02) { lo -= 0.5; hi += 0.5; }
  const ticks = niceTicks(lo, hi, 4);
  lo = ticks[0]; hi = ticks[ticks.length - 1];
  const step = ticks[1] - ticks[0];
  const x = (t) => m.l + (t1 === t0 ? pw : ((t - t0) / (t1 - t0)) * pw);
  const y = (v) => m.t + ((hi - v) / (hi - lo)) * ph;
  const last = points[points.length - 1];
  const chart = svg("svg", { width: W, height: H, viewBox: `0 0 ${W} ${H}`, role: "img",
    "aria-label": `Account value from ${money(points[0][1])} to ${money(last[1])} USDT` });
  for (const v of ticks) {
    chart.append(svg("line", { class: "grid", x1: m.l, x2: m.l + pw, y1: y(v), y2: y(v) }));
    chart.append(svgText({ class: "tick", x: m.l - 8, y: y(v) + 4, "text-anchor": "end" }, tickLabel(v, step)));
  }
  const span = t1 - t0;
  const format = span < 36 * 3600e3 ? { hour: "2-digit", minute: "2-digit" }
    : span < 200 * 86400e3 ? { month: "short", day: "numeric" } : { month: "short", year: "numeric" };
  const labels = Math.max(2, Math.min(5, Math.floor(pw / 110) + 1));
  for (let i = 0; i < labels; i++) {
    const t = t0 + (span * i) / (labels - 1);
    chart.append(svgText({ class: "tick", x: x(t), y: H - 6,
      "text-anchor": i === 0 ? "start" : i === labels - 1 ? "end" : "middle" },
      new Date(t).toLocaleString(undefined, format)));
  }
  chart.append(svg("line", { class: "ref", x1: m.l, x2: m.l + pw, y1: y(start), y2: y(start) }));
  // The start label sits in the right margin, clear of the line's beginning,
  // unless the end-value label is already there.
  const startLabel = Math.abs(y(start) - y(points[points.length - 1][1])) >= 16
    ? { class: "ref-label", x: m.l + pw + 8, y: y(start) + 4 }
    : { class: "ref-label", x: m.l + pw - 4, y: y(start) + 14, "text-anchor": "end" };
  chart.append(svgText(startLabel, "Start " + money(start)));
  const d = points.map((p, i) => `${i ? "L" : "M"}${x(p[0]).toFixed(1)},${y(p[1]).toFixed(1)}`).join("");
  chart.append(svg("path", { class: "area", d: `${d}L${x(t1).toFixed(1)},${m.t + ph}L${x(t0).toFixed(1)},${m.t + ph}Z` }));
  chart.append(svg("path", { class: "line", d }));
  chart.append(svg("circle", { class: "dot", cx: x(last[0]), cy: y(last[1]), r: 4 }));
  chart.append(svgText({ class: "end-label", x: x(last[0]) + 8, y: y(last[1]) + 4 }, money(last[1])));
  const cross = svg("line", { class: "crosshair", y1: m.t, y2: m.t + ph, visibility: "hidden" });
  const marker = svg("circle", { class: "dot", r: 4, visibility: "hidden" });
  const hit = svg("rect", { class: "hit", x: m.l, y: m.t, width: pw, height: ph, tabindex: 0,
    "aria-label": "Account value chart. Use the left and right arrow keys to read values." });
  chart.append(cross, marker, hit);
  host.append(chart);
  const tip = tooltip(host);
  let index = points.length - 1;
  const show = (i) => {
    index = i;
    const [t, v] = points[i];
    for (const [k, val] of [["x1", x(t)], ["x2", x(t)]]) cross.setAttribute(k, val);
    marker.setAttribute("cx", x(t)); marker.setAttribute("cy", y(v));
    cross.setAttribute("visibility", "visible"); marker.setAttribute("visibility", "visible");
    tip.show(x(t), y(v), money(v) + " USDT", `${signed(v - start)} since start`, when(t));
  };
  const hide = () => { cross.setAttribute("visibility", "hidden"); marker.setAttribute("visibility", "hidden"); tip.hide(); };
  hit.addEventListener("pointermove", (event) => {
    const px = event.clientX - chart.getBoundingClientRect().left;
    let lo2 = 0, hi2 = points.length - 1;
    while (hi2 - lo2 > 1) { const mid = (lo2 + hi2) >> 1; if (x(points[mid][0]) < px) lo2 = mid; else hi2 = mid; }
    show(Math.abs(x(points[lo2][0]) - px) <= Math.abs(x(points[hi2][0]) - px) ? lo2 : hi2);
  });
  hit.addEventListener("pointerleave", hide);
  hit.addEventListener("focus", () => show(index));
  hit.addEventListener("blur", hide);
  hit.addEventListener("keydown", (event) => {
    if (event.key === "ArrowLeft") { show(Math.max(0, index - 1)); event.preventDefault(); }
    if (event.key === "ArrowRight") { show(Math.min(points.length - 1, index + 1)); event.preventDefault(); }
  });
}

function barPath(x0, w, base, tip) {
  const h = Math.abs(base - tip), r = Math.min(4, w / 2, h);
  if (h < 1) return `M${x0},${base - 0.5}h${w}v1h${-w}Z`;
  const dir = tip < base ? 1 : -1;  // 1 = bar grows upward
  return `M${x0},${base}V${tip + dir * r}Q${x0},${tip} ${x0 + r},${tip}H${x0 + w - r}` +
         `Q${x0 + w},${tip} ${x0 + w},${tip + dir * r}V${base}Z`;
}

function drawTrades(host, trades) {
  host.replaceChildren();
  if (!trades.length) {
    host.append(el("p", { class: "empty", text:
      "No closed trades yet. Each closed trade appears as a bar: blue above the line for a gain, red below it for a loss." }));
    return;
  }
  const shown = trades.slice(-60);
  const W = Math.max(280, host.clientWidth), H = 220, m = { l: 52, r: 8, t: 18, b: 18 };
  const pw = W - m.l - m.r, ph = H - m.t - m.b;
  const pnls = shown.map((t) => t.pnl);
  let lo = Math.min(0, ...pnls), hi = Math.max(0, ...pnls);
  if (hi - lo < 0.02) hi = lo + 1;
  const ticks = niceTicks(lo, hi, 4);
  lo = ticks[0]; hi = ticks[ticks.length - 1];
  const step = ticks[1] - ticks[0];
  const y = (v) => m.t + ((hi - v) / (hi - lo)) * ph;
  const chart = svg("svg", { width: W, height: H, viewBox: `0 0 ${W} ${H}`, role: "img",
    "aria-label": `${shown.length} closed trades, net ${signed(pnls.reduce((a, b) => a + b, 0))} USDT` });
  for (const v of ticks) {
    if (Math.abs(v) > 1e-9) chart.append(svg("line", { class: "grid", x1: m.l, x2: m.l + pw, y1: y(v), y2: y(v) }));
    chart.append(svgText({ class: "tick", x: m.l - 8, y: y(v) + 4, "text-anchor": "end" }, tickLabel(v, step, true)));
  }
  const zero = y(0);
  chart.append(svg("line", { class: "axis", x1: m.l, x2: m.l + pw, y1: zero, y2: zero }));
  const band = pw / shown.length, width = Math.max(2, Math.min(24, band - 2));
  const tip = tooltip(host);
  const best = pnls.indexOf(Math.max(...pnls)), worst = pnls.indexOf(Math.min(...pnls));
  shown.forEach((trade, i) => {
    const center = m.l + band * (i + 0.5), top = y(trade.pnl);
    const bar = svg("path", { class: "bar " + (trade.pnl >= 0 ? "gain" : "loss"), d: barPath(center - width / 2, width, zero, top) });
    const hit = svg("rect", { class: "hit", x: m.l + band * i, y: m.t, width: band, height: ph, tabindex: 0,
      "aria-label": `${trade.symbol}, ${when(trade.t)}: ${signed(trade.pnl)} USDT, ${trade.reason}` });
    const on = () => { bar.classList.add("hover"); tip.show(center, Math.min(top, zero), signed(trade.pnl) + " USDT",
      `${trade.symbol} · ${when(trade.t)}`, trade.reason); };
    const off = () => { bar.classList.remove("hover"); tip.hide(); };
    hit.addEventListener("pointerenter", on); hit.addEventListener("pointerleave", off);
    hit.addEventListener("focus", on); hit.addEventListener("blur", off);
    chart.append(bar);
    if ((i === best && trade.pnl > 0) || (i === worst && trade.pnl < 0)) {
      const above = trade.pnl > 0;
      chart.append(svgText({ class: "value-label", x: center, y: above ? top - 5 : top + 13, "text-anchor": "middle" },
        signed(trade.pnl)));
    }
    chart.append(hit);
  });
  host.append(chart);
}

function renderTables(data) {
  const statusText = (market) => market.action ? market.action : "—";
  fill("markets", table(["Coin", "Decision", "Checked", "Why"],
    data.markets.map((mk) => [mk.symbol, statusText(mk), mk.t ? when(mk.t) : "—", mk.reason || "No check logged yet"])),
    "No market checks in the log yet.");
  const pos = data.positions || [];
  fill("positions", table(["Coin", "Quantity", "Bought at", "Now", "Stop", "Target", "P/L (USDT)", "Since"],
    pos.map((p) => [p.symbol, qty(p.quantity), price(p.entry), price(p.mark), price(p.stop), price(p.target), signed(p.pnl), when(p.t)]),
    [1, 2, 3, 4, 5, 6]), "No open positions: the bot is in cash.");
  const recent = data.trades || [];
  const recentHeaders = ["Time", "Side", "Coin", "Price", "Quantity", "P/L (USDT)", "Reason"];
  recentHeaders.wrapLast = true;
  fill("recent", table(recentHeaders, recent.map((t) => [when(t.t), t.side, t.symbol, price(t.price), qty(t.quantity),
    t.pnl == null ? "" : signed(t.pnl), t.reason]), [3, 4, 5]),
    "No trades yet. The bot only buys breakouts in uptrends and waits in cash otherwise.");
  const problemHeaders = ["Time", "Level", "Message"];
  problemHeaders.wrapLast = true;
  fill("problems", table(problemHeaders, (data.problems || []).slice().reverse().map((p) => [when(p.t), p.level, p.message])),
    "No warnings or errors in the recent log.");
}

function renderChartTables(data) {
  const history = data.equity_history || [];
  const daily = new Map();
  for (const [t, v] of history) daily.set(new Date(t).toDateString(), [t, v]);
  fill("equity-table", table(["Time", "Account value (USDT)"], [...daily.values()].reverse().slice(0, 60)
    .map(([t, v]) => [when(t), money(v)]), [1]), "No history yet.");
  fill("trades-table", table(["Closed", "Coin", "P/L (USDT)", "Reason"], (data.trade_pnl || []).slice().reverse()
    .map((t) => [when(t.t), t.symbol, signed(t.pnl), t.reason]), [2]), "No closed trades yet.");
}

function render(data) {
  const real = data.mode === "mainnet";
  $("mode").textContent = real ? "MAINNET · real money" : data.mode === "testnet" ? "TESTNET · fake money" : "PAPER · simulated";
  $("updated").textContent = "Updated " + new Date(data.now_ms).toLocaleTimeString();
  const age = data.last_cycle_ms ? (data.now_ms - data.last_cycle_ms) / 1000 : null;
  const account = data.account;
  const pills = [];
  pills.push(data.service === "active" ? pill("good", "Bot running")
    : data.service === "unknown" ? pill("warning", "Bot status unknown") : pill("critical", "Bot stopped (" + data.service + ")"));
  pills.push(age == null ? pill("warning", "No market check logged yet")
    : age <= 180 ? pill("good", "Market checked " + ago(age))
    : age <= 900 ? pill("warning", "Last market check " + ago(age)) : pill("critical", "No market check for " + ago(age).replace(" ago", "")));
  if (account) {
    pills.push(account.halted ? pill("critical", "Trading halted — check before --live-reset")
      : account.pending_entry ? pill("warning", "Unconfirmed entry " + account.pending_entry + " (being checked)")
      : pill("good", "Trading enabled"));
  }
  $("pills").replaceChildren(...pills);
  $("error").hidden = !data.error;
  $("error").textContent = data.error || "";
  if (!account) return;

  const direction = account.profit > 0.0049 ? "up" : account.profit < -0.0049 ? "down" : "flat";
  const arrow = direction === "up" ? "▲ " : direction === "down" ? "▼ " : "";
  $("hero").replaceChildren(
    el("div", { class: "hero-label", text: "Worth now" }),
    el("div", { class: "hero-value" }, money(account.equity), el("span", { class: "hero-unit", text: "USDT" })),
    el("div", { class: "delta " + direction, text: `${arrow}${signed(account.profit)} USDT (${account.profit_fraction >= 0 ? "+" : "−"}${pct(Math.abs(account.profit_fraction))}) since start` }),
    el("div", { class: "sub", text: `Started with ${money(account.start)} USDT · cash ${money(account.cash)}` }));

  const s = data.stats;
  const expected = data.expected || {};
  const tile = (label, value, sub) => el("div", { class: "tile" }, el("div", { class: "tile-label", text: label }),
    el("div", { class: "tile-value", text: value }), sub ? el("div", { class: "sub", text: sub }) : null);
  $("tiles").replaceChildren(
    tile("Realized P/L", signed(account.realized), "closed trades, after fees"),
    tile("Unrealized P/L", signed(account.unrealized), `${account.open_positions} open position${account.open_positions === 1 ? "" : "s"}`),
    tile("Closed trades", String(s.closed), `${s.wins} wins · ${s.losses} losses`),
    tile("Win rate", s.win_rate == null ? "—" : pct(s.win_rate, 0), "about 40% is normal"),
    tile("Profit factor", s.profit_factor == null ? "—" : s.profit_factor.toFixed(2),
      expected.profit_factor ? `backtest ${expected.profit_factor.toFixed(2)} · above 1 = winning` : "above 1 = winning"),
    tile("Fees paid", money(account.fees), "USDT"));

  const meter = (label, used, limit, note) => {
    const ratio = limit > 0 ? Math.min(1, used / limit) : 0;
    const color = ratio >= 0.8 ? "var(--critical)" : ratio >= 0.5 ? "var(--warning)" : "var(--series)";
    const track = el("div", { class: "meter-track" });
    track.style.background = `color-mix(in srgb, ${color} 18%, var(--surface))`;
    const bar = el("div", { class: "meter-fill", role: "meter", "aria-valuemin": "0", "aria-valuemax": String(limit),
      "aria-valuenow": String(used), "aria-label": label });
    bar.style.width = (ratio * 100).toFixed(1) + "%";
    bar.style.background = color;
    track.append(bar);
    const head = el("div", { class: "meter-head" }, el("h2", { text: label }),
      el("span", {}, ratio >= 0.8 ? pill("critical", `${pct(used)} of ${pct(limit, 0)} limit`) : `${pct(used)} of ${pct(limit, 0)} limit`));
    return el("div", { class: "card" }, head, track, el("div", { class: "sub", text: note }));
  };
  $("meters").replaceChildren(
    meter("Today's loss", account.daily_loss_fraction, data.limits.daily,
      `At ${pct(data.limits.daily, 0)} the bot stops opening new trades until tomorrow (UTC).`),
    meter("Drop from best value", account.drawdown_fraction, data.limits.drawdown,
      `At ${pct(data.limits.drawdown, 0)} the bot sells everything and stops trading until you reset it. Best: ${money(account.peak)} USDT.`));

  drawEquity($("equity-chart"), data.equity_history || [], account.start);
  drawTrades($("trades-chart"), data.trade_pnl || []);
  renderChartTables(data);
  renderTables(data);
  const judge = s.closed < 30 ? ` Too early to judge: ${s.closed} closed trade${s.closed === 1 ? "" : "s"}; wait for about 30.` : "";
  $("footer").textContent = `Read-only: this page never places or changes orders. Risk ${pct(data.limits.risk_per_trade, 2)} per trade` +
    (expected.yearly_percent ? `; backtest expectation about +${expected.yearly_percent}% a year.` : ".") + judge + " Refreshes every 15 seconds.";
}

for (const button of document.querySelectorAll("button[data-table]")) {
  button.addEventListener("click", () => {
    const target = $(button.dataset.table + "-table");
    target.hidden = !target.hidden;
    button.textContent = target.hidden ? "Show table" : "Hide table";
  });
}

let latest = null;
async function refresh() {
  document.body.classList.add("loading");
  try {
    const response = await fetch("api/status" + location.search, { cache: "no-store" });
    if (!response.ok) throw new Error("HTTP " + response.status);
    latest = await response.json();
    render(latest);
    $("offline").hidden = true;
  } catch (error) {
    $("offline").hidden = false;
    $("offline").textContent = "Can't reach the dashboard (" + error.message + "). Showing the last data received.";
  } finally {
    document.body.classList.remove("loading");
  }
}
let resizeTimer = null;
new ResizeObserver(() => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => { if (latest && latest.account) {
    drawEquity($("equity-chart"), latest.equity_history || [], latest.account.start);
    drawTrades($("trades-chart"), latest.trade_pnl || []);
  } }, 150);
}).observe(document.querySelector("main"));
refresh();
setInterval(refresh, 15000);
</script>
</body>
</html>
"""

if __name__ == "__main__":
    sys.exit(main())
