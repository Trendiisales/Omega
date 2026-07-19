#!/bin/bash
# bigcap_rth_verify.sh — one-shot RTH live-fire proof for the bigcap mimic ladder
# =============================================================================
# WHY: HEAD 6d2b4df2 tee'd the :7784 bigcap L1 feed into the mimic ladder
#   LIVE-CONFIRM gate + the daily-close writer (DCWRITE). The tee can only be
#   PROVEN during a live US RTH session (13:30-20:00 UTC). This job samples the
#   VPS just after RTH close and looks for deterministic tee evidence:
#     (a) C:\Omega\data\rdagent\sp500_long_close.csv last row == the RTH day that
#         just closed, written BEFORE the 22:35 UTC yfinance batch job
#         (mtime hour < 22 UTC == the tee wrote it, not the fallback pull), AND
#     (b) [AULAD][DCWRITE] <date> APPEND ... (>= min_cov 10 names fresh), and/or
#     (c) [AULAD][LIVE-CONFIRM] gate lines during the session.
#
# Cloud routines / ScheduleWakeup CANNOT do this (no ssh omega-new from the
# cloud sandbox; 1h wake cap can't reach the window). Hence a local Mac launchd
# job (com.omega.bigcap-rth-verify) firing daily at NZ 08:25 == 20:25 UTC, just
# after the 20:00 UTC RTH close and well before the 22:35 UTC yfinance job.
#
# ONE-SHOT: on the first clean PASS this script drops a DONE marker and unloads
# its own launchd job, so it stops firing. On weekends / holidays / thin days it
# logs INCONCLUSIVE and stays loaded to retry the next RTH.
#
# READ-ONLY: only reads VPS files/logs over ssh (no POST / no mutation) — see
# memory feedback-audit-read-only-never-mutate. ssh form starts literally
# `ssh omega-new` per memory feedback-vps-ssh-command-form.
#
# Install:  cp tools/com.omega.bigcap-rth-verify.plist ~/Library/LaunchAgents/ \
#             && launchctl load ~/Library/LaunchAgents/com.omega.bigcap-rth-verify.plist
# Remove:   launchctl unload ~/Library/LaunchAgents/com.omega.bigcap-rth-verify.plist \
#             && rm ~/Library/LaunchAgents/com.omega.bigcap-rth-verify.plist
# =============================================================================
set -uo pipefail

LABEL="com.omega.bigcap-rth-verify"
PLIST="$HOME/Library/LaunchAgents/${LABEL}.plist"
DONE="/tmp/bigcap_rth_verify.DONE"
LOG="/tmp/bigcap_rth_verify.log"
CSV='C:\Omega\data\rdagent\sp500_long_close.csv'

ts() { TZ=UTC date '+%Y-%m-%d %H:%M:%SZ'; }
say() { echo "[$(ts)] $*" | tee -a "$LOG"; }
notify() { osascript -e "display notification \"$1\" with title \"Bigcap RTH verify\"" 2>/dev/null || true; }

# Idempotent: already proven -> no-op (and make sure the job is gone).
if [[ -f "$DONE" ]]; then
  say "DONE marker present ($DONE) -> already proven; unloading job."
  launchctl bootout "gui/$(id -u)/${LABEL}" 2>/dev/null || launchctl unload "$PLIST" 2>/dev/null || true
  exit 0
fi

# The RTH day that just closed == current UTC date at 20:25 UTC fire time.
EXPECT="$(TZ=UTC date '+%Y-%m-%d')"
say "=== bigcap RTH verify: expecting daily-close row for UTC day $EXPECT ==="

# One consolidated read-only ssh pull.
SNAP="$(ssh omega-new "powershell -NoProfile -Command \"\
\$c='${CSV}'; \
if(Test-Path \$c){ \$f=Get-Item \$c; \
  Write-Output ('MTIME_UTC='+\$f.LastWriteTimeUtc.ToString('yyyy-MM-dd HH:mm')); \
  \$last=Get-Content \$c -Tail 1; Write-Output ('LASTROW='+\$last) } \
else { Write-Output 'MTIME_UTC=MISSING'; Write-Output 'LASTROW=MISSING' }; \
Write-Output '---AULAD---'; \
Get-Content C:\Omega\logs\omega_service_stdout.log -Tail 6000 | Select-String 'AULAD.*DCWRITE|AULAD.*LIVE-CONFIRM|AULAD.*ENTRY' | Select-Object -Last 20 | ForEach-Object { \$_.Line }; \
Write-Output '---HASH---'; \
Get-Content C:\Omega\logs\omega_service_stderr.log -Tail 40 | Select-String 'Git hash' | Select-Object -Last 1 | ForEach-Object { \$_.Line }\"" 2>&1)"

say "VPS snapshot:"
echo "$SNAP" | tee -a "$LOG"

MTIME="$(echo "$SNAP" | grep '^MTIME_UTC=' | head -1 | cut -d= -f2-)"
LASTROW="$(echo "$SNAP" | grep '^LASTROW=' | head -1 | cut -d= -f2-)"
ROWDATE="$(echo "$LASTROW" | cut -d, -f1)"
MTIME_HOUR="$(echo "$MTIME" | awk '{print $2}' | cut -d: -f1)"

# DCWRITE APPEND for the expected day (not a SKIP)?
DCW_APPEND="$(echo "$SNAP" | grep -E "DCWRITE.*${EXPECT}.*(APPEND|appended|wrote)" | grep -vi 'SKIP' | head -1)"
LIVECONF="$(echo "$SNAP" | grep -E "LIVE-CONFIRM" | head -1)"

VERDICT="INCONCLUSIVE"; REASON=""
if [[ "$ROWDATE" == "$EXPECT" ]]; then
  if [[ -n "$MTIME_HOUR" && "$MTIME_HOUR" =~ ^[0-9]+$ && "$MTIME_HOUR" -lt 22 ]]; then
    VERDICT="PASS"
    REASON="CSV row $EXPECT written ${MTIME} UTC (<22:00 => tee, not 22:35 yfinance)."
  else
    VERDICT="FAIL"
    REASON="CSV row $EXPECT present but mtime ${MTIME} UTC >=22:00 => likely yfinance fallback, tee did NOT write it."
  fi
else
  REASON="No $EXPECT row yet (last row date=$ROWDATE). Weekend/holiday/thin, or tee not writing. Retry next RTH."
fi
[[ -n "$DCW_APPEND" ]] && REASON="$REASON | DCWRITE-APPEND: $DCW_APPEND"
[[ -n "$LIVECONF"   ]] && REASON="$REASON | LIVE-CONFIRM seen."

say "VERDICT=$VERDICT : $REASON"

# Corroborating selftests (best-effort; do not gate the verdict on them).
PY="/opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python"
[[ -x "$PY" ]] || PY="python3"
say "--- feedpath_selftest.py ---"; "$PY" /Users/jo/Omega/tools/feedpath_selftest.py >>"$LOG" 2>&1; say "feedpath rc=$?"
say "--- feeds_selftest.py ---";    "$PY" /Users/jo/Omega/tools/feeds_selftest.py    >>"$LOG" 2>&1; say "feeds rc=$?"

if [[ "$VERDICT" == "PASS" ]]; then
  notify "PASS — bigcap tee proven ($EXPECT). Job unloading."
  say "PASS -> writing DONE marker + unloading self."
  echo "PASS $EXPECT $REASON" > "$DONE"
  launchctl bootout "gui/$(id -u)/${LABEL}" 2>/dev/null || launchctl unload "$PLIST" 2>/dev/null || true
elif [[ "$VERDICT" == "FAIL" ]]; then
  notify "FAIL — bigcap tee did NOT write $EXPECT (yfinance fallback). See $LOG."
  say "FAIL -> staying loaded; operator to inspect. NOT writing DONE."
else
  notify "Inconclusive ($EXPECT) — retry next RTH. See $LOG."
  say "INCONCLUSIVE -> staying loaded to retry next RTH."
fi
exit 0
