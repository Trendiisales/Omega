#!/usr/bin/env bash
# persistence_audit.sh -- ENFORCEMENT: every engine that registers a GUI display source (shows open
# positions in the dashboard) MUST also have a persist source in PositionPersistence.hpp, or its
# positions VANISH with no ledger close on every restart/deploy (the recurring "trades disappeared" bug).
# FAILS the build on any gap not in the documented allowlist. Re-enabling a dormant engine without
# wiring persistence -> this gate fails. (S-2026-06-26, after Luke/IndexBearShort silent-loss incidents.)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EI="$ROOT/include/engine_init.hpp"; PP="$ROOT/include/PositionPersistence.hpp"
disp=$(grep -oE 'register_source\("[^"]+"' "$EI" | sed -E 's/register_source\("//;s/"//' | sort -u)
# persist tags: wire_* literals + inline/dynamic sources added by hand below
pers=$(grep -oE 'wire_(livepos|cross|multicell)\(g_[a-z0-9_]+, *"[^"]+"' "$PP" | sed -E 's/.*, *"//;s/"//' | sort -u)
pers="$pers
Survivor
BigCapMomoIbkr"
# DORMANT/tombstoned engines (enabled=false; cannot hold a live position). RE-ENABLING ONE WITHOUT
# WIRING PERSISTENCE WILL FAIL THIS GATE -- that is the point. Keep this list short + justified.
# S-2026-07-11 PHASE 1b: GoldVolBreakoutM30 REMOVED -- it had been LIVE since the
# S-2026-07-01 cutover while still allowlisted as "dormant"; both instances (spot +
# MGC) are now properly persisted via wire_cross.
allow="FvgContinuation FvgCont10m FvgCont30m GoldOrbRetrace GoldOrbRetraceLDN \
NqFutMomo DonchianPortfolio EmaPullbackPortfolio Us30Ensemble XauSessNYpm FxCrossRevEURGBP"
# QndxSqf (S-2026-07-11): SELF-RESTORING by construction, not dormant -- its legs
# re-derive target position from the daily-CSV replay + on-connect adopt
# (qndx_sqf_ibkr.cpp warm_from_csv/ingest_daily_bar). A persist wire would fight
# the signal-state reconcile. Allowed as designed-out, not as a gap.
allow="$allow QndxSqf"
fail=0
while read -r tag; do
  [ -z "$tag" ] && continue
  printf '%s\n' "$pers" | grep -qx "$tag" && continue
  printf '%s ' $allow | grep -qw "$tag" && continue
  echo "  *** PERSISTENCE GAP: '$tag' shows positions but has NO persist source -> add a wire in PositionPersistence.hpp"
  fail=1
done <<< "$disp"
if [ "$fail" -ne 0 ]; then
  echo "[persistence-audit] FAIL -- unpersisted display engine(s) above. Positions would vanish on restart."
  exit 1
fi
echo "[persistence-audit] OK -- every display engine persists (or is an allowlisted dormant engine)."
