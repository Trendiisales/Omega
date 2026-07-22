#!/bin/bash
# tools/monitor_crashsafe_wrap.sh — crash-safe notify wrapper for Mac cron monitors
# (S-2026-07-12, never-again audit, class B).
#
# WHY: on 2026-07-12 feedpath_selftest.py CRASHED under cron's /usr/bin/python3 3.9
# (datetime.UTC is 3.11-only). An uncaught Python traceback exits 1 — the same exit
# class as a real RED — so the `|| osascript "FEED-PATH RED / path broken"` wrapper
# reported a SYSTEM failure when the truth was "the MONITOR is broken; system state
# UNKNOWN". The inverse also existed: staleness_scan's grep-for-RED wrapper made a
# monitor crash SILENT (no RESULT line -> no notify -> dead monitor, nobody knows).
#
# WHAT: run the monitor, classify the outcome, notify honestly:
#   MONITOR-CRASH  -> "🩺 MONITOR BROKEN (crash)" — system state UNKNOWN, fix the monitor.
#   SYSTEM-RED     -> the monitor's own RED notification (title/msg passed in).
#   GREEN          -> silent.
# Crash classification:
#   * output contains a Python traceback / bash "command not found" / "syntax error", OR
#   * exit code is NOT in the monitor's declared known-code set (-k), OR
#   * a required output marker (-q regex) is missing (e.g. staleness_scan's ^RESULT:).
#   A traceback ALWAYS wins over a known exit code (that's the 1-vs-1 conflation).
# Crash notifications are throttled to one per 6h per monitor (marker in /tmp) so a
# broken monitor doesn't nag every cycle; the log records every occurrence.
#
# usage:
#   monitor_crashsafe_wrap.sh -n NAME -l LOGFILE [-k KNOWN_CODES] [-r RED_CODES]
#       [-t RED_TITLE -m RED_MSG] [-q REQUIRED_OUT_REGEX] [-R RED_OUT_REGEX] -- cmd args...
#   -k  comma list of NON-CRASH exit codes besides 0 (designed failures), e.g. "1,2"
#   -r  comma list of exit codes that mean SYSTEM RED (notify), subset of -k, e.g. "1"
#   -R  alternative: output regex that means SYSTEM RED (e.g. 'RESULT: RED')
#   -q  output regex that MUST be present or the monitor is BROKEN (e.g. '^RESULT:')
# Exit code: passes the monitor's exit code through (99 if crash-by-marker with rc=0).
set -u
# MAINTENANCE SUPPRESS (2026-07-21): when the operator has DELIBERATELY parked the desk
# (Omega service Stopped+Disabled / Gateway on paper / crypto box stopped), EVERY monitor
# routed through this wrapper (protection, feeds, feedpath, staleness, health, update-guard,
# crypto-staleness) reports RED on the intended-down state and spams notifications. This flag
# silences ALL of them at once. Re-arm when going live again: rm ~/.omega_desk_maintenance
[ -f "$HOME/.omega_desk_maintenance" ] && exit 0
NAME="" LOG="" KNOWN="" REDC="" TITLE="" MSG="" REQRE="" REDRE=""
while [ $# -gt 0 ]; do
  case "$1" in
    -n) NAME="$2"; shift 2;;
    -l) LOG="$2"; shift 2;;
    -k) KNOWN="$2"; shift 2;;
    -r) REDC="$2"; shift 2;;
    -t) TITLE="$2"; shift 2;;
    -m) MSG="$2"; shift 2;;
    -q) REQRE="$2"; shift 2;;
    -R) REDRE="$2"; shift 2;;
    --) shift; break;;
    *) echo "monitor_crashsafe_wrap: unknown arg $1" >&2; exit 64;;
  esac
done
[ -z "$NAME" ] || [ -z "$LOG" ] || [ $# -eq 0 ] && { echo "usage: -n NAME -l LOG [-k codes] [-r codes] [-t title -m msg] [-q re] [-R re] -- cmd..." >&2; exit 64; }

OUT=$("$@" 2>&1); RC=$?
{ echo "== $(date '+%Y-%m-%d %H:%M:%S') $NAME rc=$RC =="; printf '%s\n' "$OUT"; } >> "$LOG" 2>/dev/null

in_list() { # $1=code $2=comma-list
  local IFS=','; for c in $2; do [ "$1" = "$c" ] && return 0; done; return 1
}

notify() { # $1=msg $2=title
  /usr/bin/osascript -e "display notification \"$1\" with title \"$2\" sound name \"Basso\"" 2>/dev/null
}

# --- classify ---
CRASH=0; WHY=""
if printf '%s' "$OUT" | grep -q "Traceback (most recent call last)"; then
  CRASH=1; WHY="python traceback"
elif printf '%s' "$OUT" | grep -qE "command not found|: syntax error|ModuleNotFoundError|ImportError:"; then
  CRASH=1; WHY="interpreter/env error"
elif [ -n "$REQRE" ] && ! printf '%s\n' "$OUT" | grep -qE "$REQRE"; then
  CRASH=1; WHY="required output marker '$REQRE' missing (monitor died mid-run)"
elif [ "$RC" -ne 0 ] && ! in_list "$RC" "$KNOWN"; then
  CRASH=1; WHY="unexpected exit code $RC (known: 0,$KNOWN)"
fi

if [ "$CRASH" = "1" ]; then
  MARK="/tmp/monitor_broken_${NAME}.notified"
  # throttle: at most one crash notification per 6h per monitor
  if [ ! -f "$MARK" ] || [ -n "$(find "$MARK" -mmin +360 2>/dev/null)" ]; then
    notify "MONITOR BROKEN (crash): $NAME — the monitor itself failed ($WHY). System state UNKNOWN, not necessarily RED. See $LOG" "🩺 MONITOR BROKEN"
    touch "$MARK"
  fi
  echo "MONITOR-CRASH [$NAME] $WHY (rc=$RC)" >> "$LOG" 2>/dev/null
  [ "$RC" -ne 0 ] && exit "$RC" || exit 99
fi
rm -f "/tmp/monitor_broken_${NAME}.notified" 2>/dev/null

# --- system RED? ---
RED=0
if [ -n "$REDRE" ]; then
  printf '%s\n' "$OUT" | grep -qE "$REDRE" && RED=1
elif [ -n "$REDC" ] && in_list "$RC" "$REDC"; then
  RED=1
fi
if [ "$RED" = "1" ] && [ -n "$TITLE" ]; then
  notify "$MSG" "$TITLE"
fi
exit "$RC"
