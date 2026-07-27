#!/usr/bin/env bash
#
# Agentmeter — one-run setup + flash.
#
#   ./setup.sh                 # pick your board, auto-detect the USB port, flash
#   ./setup.sh <board> [port]  # non-interactive
#
# Installs PlatformIO for you if it's missing. Fonts and everything else are
# pre-built and committed — there is nothing else to set up.
#
set -euo pipefail
cd "$(dirname "$0")"

say()  { printf '\033[1;36m%s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m%s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m%s\033[0m\n' "$*" >&2; exit 1; }

OS="$(uname -s)"

# ---------------------------------------------------------------------------
# 1. PlatformIO — find it, or install it.
# ---------------------------------------------------------------------------
find_pio() {
    if command -v pio >/dev/null 2>&1; then echo "pio"; return; fi
    if [ -x "$HOME/.platformio/penv/bin/pio" ]; then echo "$HOME/.platformio/penv/bin/pio"; return; fi
    echo ""
}

PIO="$(find_pio)"
if [ -z "$PIO" ]; then
    say "PlatformIO not found — installing it (one-time, ~1–2 min)…"
    command -v python3 >/dev/null 2>&1 || die \
"Python 3 is required to install PlatformIO. Install it first:
  • Debian/Ubuntu:  sudo apt install python3 python3-venv curl
  • macOS:          brew install python
then re-run ./setup.sh"
    command -v curl >/dev/null 2>&1 || die "curl is required. Install curl and re-run."
    python3 -c "$(curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py)"
    PIO="$(find_pio)"
    [ -n "$PIO" ] || die "PlatformIO install did not complete. See the output above."
fi
say "Using PlatformIO: $PIO"

# ---------------------------------------------------------------------------
# 2. Board.
# ---------------------------------------------------------------------------
BOARDS=(waveshare_amoled_216 waveshare_amoled_18 waveshare_amoled_216_c6 waveshare_amoled_18_c6)
BOARD="${1:-}"
if [ -z "$BOARD" ]; then
    echo
    say "Which board do you have?"
    echo "  1) 2.16\" square  (ESP32-S3)   — the common one   [default]"
    echo "  2) 1.8\"  portrait (ESP32-S3)"
    echo "  3) 2.16\" square  (ESP32-C6)"
    echo "  4) 1.8\"  portrait (ESP32-C6)"
    read -r -p "Choice [1-4]: " choice
    case "${choice:-1}" in
        1|"") BOARD="${BOARDS[0]}" ;;
        2)    BOARD="${BOARDS[1]}" ;;
        3)    BOARD="${BOARDS[2]}" ;;
        4)    BOARD="${BOARDS[3]}" ;;
        *)    die "Invalid choice: $choice" ;;
    esac
fi
say "Board: $BOARD"

# ---------------------------------------------------------------------------
# 3. Serial port — auto-detect (override with the 2nd argument).
# ---------------------------------------------------------------------------
detect_port() {
    case "$OS" in
        Darwin) ls /dev/cu.usbmodem* 2>/dev/null | head -1 ;;
        *)      ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1 ;;
    esac
}
PORT="${2:-$(detect_port)}"
if [ -z "$PORT" ]; then
    die "No USB serial device found. Plug the board in via USB and re-run,
or pass the port explicitly:  ./setup.sh $BOARD /dev/ttyACM0"
fi
say "Port: $PORT"

# ---------------------------------------------------------------------------
# 4. Port permission (Linux) — fix it if we can't write to the port.
# ---------------------------------------------------------------------------
if [ "$OS" = "Linux" ] && [ ! -w "$PORT" ]; then
    warn "No write access to $PORT — granting it (needs sudo, one-time)…"
    # Persistent rule so this survives re-plugging; plus an immediate chmod.
    echo 'KERNEL=="ttyACM[0-9]*|ttyUSB[0-9]*", MODE="0666"' | \
        sudo tee /etc/udev/rules.d/99-clawdmeter.rules >/dev/null || true
    sudo udevadm control --reload-rules 2>/dev/null || true
    sudo udevadm trigger 2>/dev/null || true
    sudo chmod a+rw "$PORT" || die "Could not get access to $PORT."
fi

# ---------------------------------------------------------------------------
# 5. Build + flash.
# ---------------------------------------------------------------------------
echo
say "Building and flashing… (first build downloads toolchains — a few minutes)"
"$PIO" run -d firmware -e "$BOARD" -t upload --upload-port "$PORT"

cat <<EOF

$(say "Done — firmware flashed.")

Next: set up Wi-Fi + your Anthropic token (one time)
  1. On your phone, join the Wi-Fi network "ClawdMeter" (no password).
  2. A "sign in to network" page opens — or browse to http://192.168.4.1
  3. Enter your Wi-Fi name, password, and token (sk-ant-…), then Save.

Your token is the "oauthToken" field in ~/.claude/.credentials.json
EOF
