#!/bin/zsh
# =============================================================================
# crypto_book_watchdog.sh -- self-healing staleness guard for the crypto book
# (S-2026-07-02, operator: "zero instances of any staleness in both books,
#  fixed permanently").
#
# WHY: launchd StartInterval will NOT start a new refresh_shadow.py run while a
# previous one is hung (e.g. IBKR gateway wait without timeout), and a wrong
# path (the 2026-07-01 IBKRCrypto->Crypto move) makes every run a silent no-op.
# Both failure modes stale the book with nothing to restart it. This watchdog
# closes the loop: if the book heartbeat is older than MAX_AGE, it
# `launchctl kickstart -k`s the refresh job -- which KILLS a hung instance and
# starts a fresh one. Runs every 10 min via com.chimera.book-watchdog.plist.
#
# Heartbeat = max(updated, live_mark_ts) from state.json, NOT file mtime
# (mtime false-alarms; see protection_selftest.py check [6] for the reasoning).
# =============================================================================
STATE="$HOME/Crypto/backtest/data/ibkrcrypto/state.json"
[ -f "$STATE" ] || STATE="$HOME/IBKRCrypto/backtest/data/ibkrcrypto/state.json"  # pre-cutover fallback
LABEL="com.chimera.book-refresh"
MAX_AGE=1800   # seconds; refresh cadence is 300s, so >30min = 6 missed cycles

age=$(/opt/homebrew/bin/python3 - "$STATE" <<'PY'
import json, sys, datetime as dt
try:
    d = json.load(open(sys.argv[1]))
    hb = 0.0
    u = (d.get("updated", "") or "").replace(" UTC", "")
    try:
        hb = max(hb, dt.datetime.strptime(u, "%Y-%m-%d %H:%M")
                       .replace(tzinfo=dt.timezone.utc).timestamp())
    except Exception:
        pass
    lm = (d.get("live_mark_ts", "") or "").replace(" UTC", "")
    try:  # live_mark_ts is a "YYYY-MM-DD HH:MM UTC" string, not an epoch float
        hb = max(hb, dt.datetime.strptime(lm, "%Y-%m-%d %H:%M")
                       .replace(tzinfo=dt.timezone.utc).timestamp())
    except Exception:
        pass
    now = dt.datetime.now(dt.timezone.utc).timestamp()
    print(int(now - hb) if hb > 0 else 10**9)
except Exception:
    print(10**9)   # unreadable/missing state == maximally stale -> kickstart
PY
)

if [ "$age" -gt "$MAX_AGE" ]; then
  echo "$(date -u '+%Y-%m-%d %H:%M:%S') [BOOK-WATCHDOG] heartbeat ${age}s stale (> ${MAX_AGE}s) -- kickstart -k ${LABEL}"
  /bin/launchctl kickstart -k "gui/$(id -u)/${LABEL}"
else
  : # fresh -- stay silent
fi
