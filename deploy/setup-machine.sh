#!/usr/bin/env bash
# Sets this machine up to run the bot 24/7 on Binance Spot TESTNET (fake money):
# installs build tools, builds and tests, stores the testnet API keys privately,
# runs a one-order self-test, and installs the systemd user service.
#
# Usage, from the repository root:   bash deploy/setup-machine.sh
#
# Safe to re-run. Only one machine may trade a Binance account at a time.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"
env_dir="$HOME/.config/crypto-ai-trader"
env_file="$env_dir/testnet.env"
unit_dir="$HOME/.config/systemd/user"
service=crypto-ai-trader

step() { printf '\n==> %s\n' "$*"; }
# Discards keys pressed while earlier steps ran (for example Enter during the
# build), so a prompt never takes them as an empty answer.
drain_input() {
    while read -r -s -t 0.2 -n 10000 _ 2>/dev/null; do :; done
}
ask() { local reply; drain_input; read -r -p "$1 [y/N] " reply; [[ "$reply" =~ ^[Yy]$ ]]; }

if [[ "$repo" == *" "* || "$repo" == *"#"* ]]; then
    echo "Move the repository to a path without spaces or '#': $repo" >&2
    exit 1
fi

step "1/7 Build tools"
packages=(build-essential cmake ninja-build libcurl4-openssl-dev libssl-dev libboost-all-dev curl)
missing=()
for package in "${packages[@]}"; do
    dpkg -s "$package" >/dev/null 2>&1 || missing+=("$package")
done
if ((${#missing[@]})); then
    echo "Installing: ${missing[*]}"
    sudo apt-get update
    sudo apt-get install -y "${missing[@]}"
else
    echo "All build tools are installed."
fi
# CMakePresets.json version 6 needs CMake 3.25+ (Ubuntu 22.04 ships 3.22).
cmake_version="$(cmake --version | awk 'NR==1 {print $3}')"
if ! printf '3.25.0\n%s\n' "$cmake_version" | sort -V -C; then
    echo "CMake $cmake_version is too old; installing a current CMake from snap."
    sudo snap install cmake --classic
    hash -r
fi
echo "CMake $(cmake --version | awk 'NR==1 {print $3}'), $(g++ --version | head -1)"

step "2/7 Build and run the tests"
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release

step "3/7 Testnet API keys"
if [[ -s "$env_file" ]]; then
    echo "Using the keys already saved in $env_file (to re-enter them: rm $env_file)"
else
    echo "Paste the keys from https://testnet.binance.vision (the secret is not shown)."
    key=""
    secret=""
    for attempt in 1 2 3; do
        drain_input
        read -r -p "Testnet API key: " key
        read -r -s -p "Testnet secret key: " secret
        echo
        key="${key//[[:space:]]/}"
        secret="${secret//[[:space:]]/}"
        if [[ "$key" =~ ^[A-Za-z0-9]{20,128}$ && "$secret" =~ ^[A-Za-z0-9]{20,128}$ ]]; then
            break
        fi
        echo "Each key is one line of letters and digits (usually 64). Try again ($attempt/3)." >&2
        key=""
        secret=""
    done
    if [[ -z "$key" ]]; then
        echo "No valid keys entered." >&2
        exit 1
    fi
    mkdir -p "$env_dir"
    chmod 700 "$env_dir"
    (umask 077; printf 'BINANCE_TESTNET_API_KEY=%s\nBINANCE_TESTNET_API_SECRET=%s\n' \
        "$key" "$secret" > "$env_file")
    unset key secret
    echo "Saved to $env_file (readable only by you)."
fi

step "4/7 Order self-test on testnet (fake money, outside the bot ledger)"
if ! (set -a; . "$env_file"; set +a; ./build-linux/crypto-ai-trader --live-selftest config/testnet.json); then
    echo "Self-test failed. If it says the API key is invalid, run: rm $env_file" >&2
    echo "and start this script again to re-enter the keys." >&2
    exit 1
fi

step "5/7 One bot per Binance account"
echo "Two machines trading the same account would double every trade."
echo "Stop the bot on every other machine first. On the server (10.10.50.2):"
echo "    systemctl --user disable --now crypto-ai-trader"
if ! ask "Is the bot stopped on every other machine?"; then
    echo "Not installing the service. Re-run this script when the other copy is stopped."
    exit 1
fi

step "6/7 Install the 24/7 service"
mkdir -p "$unit_dir"
sed -e "s#^WorkingDirectory=.*#WorkingDirectory=$repo#" \
    -e "s#^ExecStart=[^ ]*#ExecStart=$repo/build-linux/crypto-ai-trader#" \
    deploy/crypto-ai-trader.service > "$unit_dir/$service.service"
grep -E '^(WorkingDirectory|ExecStart|EnvironmentFile)=' "$unit_dir/$service.service"
systemctl --user daemon-reload
systemctl --user enable "$service"
systemctl --user restart "$service"
if ! loginctl enable-linger "$USER" 2>/dev/null; then
    echo "Could not keep the bot running after logout; run: sudo loginctl enable-linger $USER"
fi
sleep 3
if systemctl --user is-active --quiet "$service"; then
    echo "Service is running."
else
    echo "Service is NOT running; see: journalctl --user -u $service -n 50" >&2
    exit 1
fi

step "7/7 Sleep settings"
if compgen -G "/sys/class/power_supply/BAT*" >/dev/null; then
    echo "This is a laptop. The bot pauses whenever it sleeps (lid closed or idle)."
    echo "Exchange stop-losses keep protecting positions while it sleeps, but trailing"
    echo "stops, exits, and new entries wait until it wakes."
    lid_conf=/etc/systemd/logind.conf.d/crypto-ai-trader.conf
    lid_settings=""
    if ask "Never sleep, on battery AND plugged in (lid closed or idle)? (sudo)"; then
        lid_settings='HandleLidSwitch=ignore\nHandleLidSwitchExternalPower=ignore\nHandleLidSwitchDocked=ignore\n'
        sleep_types=(sleep-inactive-ac-type sleep-inactive-battery-type)
        echo "On battery the bot runs until the battery is empty (a few hours), then the"
        echo "laptop shuts down and the bot stops; it starts again by itself after power-on."
        echo "Do NOT put the laptop in a bag while it is running with the lid closed: it"
        echo "stays on and can overheat."
    elif ask "Never sleep only while PLUGGED IN? (sudo; on battery it still sleeps)"; then
        lid_settings='HandleLidSwitchExternalPower=ignore\nHandleLidSwitchDocked=ignore\n'
        sleep_types=(sleep-inactive-ac-type)
    fi
    if [[ -n "$lid_settings" ]]; then
        sudo mkdir -p /etc/systemd/logind.conf.d
        printf "[Login]\n$lid_settings" | sudo tee "$lid_conf" >/dev/null
        for sleep_type in "${sleep_types[@]}"; do
            gsettings set org.gnome.settings-daemon.plugins.power "$sleep_type" 'nothing' \
                2>/dev/null || true
        done
        echo "Applied. The lid setting takes effect after the next reboot."
        echo "To undo: sudo rm $lid_conf && gsettings reset-recursively org.gnome.settings-daemon.plugins.power"
    fi
else
    echo "Not a laptop; nothing to change."
fi

public_ip="$(curl -fsS --max-time 10 https://checkip.amazonaws.com 2>/dev/null || echo unknown)"
cat <<EOF

Done: the testnet bot runs 24/7 on this machine.
  Watch it:   journalctl --user -u $service -f
  Status:     systemctl --user status $service
  Account:    ./build-linux/crypto-ai-trader --live-status config/testnet.json
  Stop it:    systemctl --user stop $service   (stop it before --live-flatten/--live-reset)

Public IP that Binance sees from this network: $public_ip
For a future mainnet key, restrict it to this IP. On a laptop this changes when you
change networks (office, home, hotspot), and the key then stops working until updated.
EOF
