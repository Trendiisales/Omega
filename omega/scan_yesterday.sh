#!/usr/bin/env bash
# scan_yesterday.sh — pull fresh daily history from your IBKR (TWS / IB Gateway)
# and run the Omega scanner on the latest close.
#
# PREREQUISITES (on your Mac):
#   1. TWS or IB Gateway running and LOGGED IN.
#   2. In TWS: Configuration > API > Settings:
#        - "Enable ActiveX and Socket Clients" = ON
#        - Socket port = 7497 (paper) or 7496 (live); add 127.0.0.1 to trusted IPs
#   3. The TWS API C++ source installed (https://interactivebrokers.github.io/),
#      e.g. unzipped to ~/twsapi.
#   4. Your IBKR market-data subscriptions active for these symbols.
#
# USAGE:
#   ./scan_yesterday.sh                 # uses defaults below
#   TWSAPI_DIR=~/twsapi PORT=7497 ./scan_yesterday.sh
#
set -euo pipefail
cd "$(dirname "$0")"

TWSAPI_DIR="${TWSAPI_DIR:-$HOME/twsapi}"
PORT="${PORT:-7497}"
DURATION="${DURATION:-2 Y}"     # enough history to warm the 200-EMA regime filter
BAR="${BAR:-1 day}"
TOP="${TOP:-40}"
UNIVERSE_FILE="${UNIVERSE_FILE:-universe.txt}"
LIVE_DIR="data/live"

echo "==> Building Omega with IBKR support (TWSAPI_DIR=$TWSAPI_DIR)"
cmake -B build -S . -DOMEGA_WITH_IBKR=ON -DTWSAPI_DIR="$TWSAPI_DIR" >/dev/null
cmake --build build -j >/dev/null
echo "    build OK"

# Collect tickers (strip comments/blanks).
mapfile -t SYMS < <(grep -vE '^\s*(#|$)' "$UNIVERSE_FILE" | awk '{print $1}')
echo "==> ${#SYMS[@]} symbols to fetch from IBKR on port $PORT"

mkdir -p "$LIVE_DIR"
rm -f "$LIVE_DIR"/*.csv 2>/dev/null || true

# Fetch in small batches to respect IBKR pacing (~50 historical reqs / 10 min).
BATCH=10
for ((i=0; i<${#SYMS[@]}; i+=BATCH)); do
  CHUNK=("${SYMS[@]:i:BATCH}")
  echo "    fetching: ${CHUNK[*]}"
  ./build/omega_ibkr_fetch "$LIVE_DIR" "${CHUNK[@]}" \
      --port "$PORT" --duration "$DURATION" --bar "$BAR" || true
  sleep 12   # pacing guard between batches
done

echo "==> Scanning latest close"
./build/omega_scan "$LIVE_DIR" --benchmark SPY --top "$TOP" | tee scan_results.txt
echo
echo "Saved full table to: $(pwd)/scan_results.txt"
echo "Rows marked ENTER passed all gates (trend + strength + ignition + flow + regime)."
