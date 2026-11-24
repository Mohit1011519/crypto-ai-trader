#!/usr/bin/env bash
# Installs the read-only dashboard as a systemd user service next to the bot.
#
# Usage, from the repository root:
#   bash deploy/install-dashboard.sh                 # this computer only: http://127.0.0.1:8765
#   bash deploy/install-dashboard.sh --lan           # also other devices on your Wi-Fi (token link)
#   bash deploy/install-dashboard.sh config/mainnet.json
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"
host=127.0.0.1
config=config/testnet.json
for argument in "$@"; do
    case "$argument" in
        --lan) host=0.0.0.0 ;;
        *) config="$argument" ;;
    esac
done
if [[ ! -f "$config" ]]; then
    echo "Config not found: $config" >&2
    exit 1
fi
unit_dir="$HOME/.config/systemd/user"
unit="$unit_dir/crypto-ai-trader-dashboard.service"
mkdir -p "$unit_dir"
sed -e "s#^WorkingDirectory=.*#WorkingDirectory=$repo#" \
    -e "s#^ExecStart=.*#ExecStart=/usr/bin/python3 -I $repo/scripts/dashboard.py $config --host $host#" \
    deploy/crypto-ai-trader-dashboard.service > "$unit"
systemctl --user daemon-reload
systemctl --user enable crypto-ai-trader-dashboard >/dev/null
since="$(date '+%Y-%m-%d %H:%M:%S')"
systemctl --user restart crypto-ai-trader-dashboard
for _ in $(seq 1 20); do
    line="$(journalctl --user -u crypto-ai-trader-dashboard --since "$since" --no-pager -o cat 2>/dev/null |
            grep -m1 '^Dashboard:' || true)"
    [[ -n "$line" ]] && break
    sleep 0.5
done
if ! systemctl --user is-active --quiet crypto-ai-trader-dashboard; then
    echo "The dashboard did not start; see: journalctl --user -u crypto-ai-trader-dashboard -n 30" >&2
    exit 1
fi
echo "${line:-Dashboard is running (port 8765).}"
if [[ "$host" == 0.0.0.0 ]]; then
    echo "Open that link on your phone (same Wi-Fi). Anyone with the link can see your balances."
    echo "If it does not load, allow the port: sudo ufw allow 8765/tcp"
fi
echo "Stop it: systemctl --user disable --now crypto-ai-trader-dashboard"
