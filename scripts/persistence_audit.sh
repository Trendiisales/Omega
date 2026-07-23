#!/usr/bin/env bash
# persistence_audit.sh -- ENFORCEMENT: every engine that registers a GUI display source (shows open
# positions in the dashboard) MUST also have a persist source in PositionPersistence.hpp, or its
# positions VANISH with no ledger close on every restart/deploy (the recurring "trades disappeared" bug).
# FAILS the build on any gap not in the documented allowlist. Re-enabling a dormant engine without
# wiring persistence -> this gate fails. (S-2026-06-26, after Luke/IndexBearShort silent-loss incidents.)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EI="$ROOT/include/engine_init.hpp"; PP="$ROOT/include/PositionPersistence.hpp"
fail=0
disp=$(grep -oE 'register_source\("[^"]+"' "$EI" | sed -E 's/register_source\("//;s/"//' | sort -u)

# ---- DYNAMIC register_source shapes (S-2026-07-14, latent-class sweep item 5b) ----
# register_source(<expr>) with a NON-LITERAL first arg is invisible to the literal grep
# above -- that is exactly how MondayRiskOn (enabled shadow, runtime-built name, no
# persist wire) hid from this audit. Every dynamic line must match a KNOWN shape below,
# and each shape resolver must yield >0 concrete names (a format drift that parses to
# zero is a FAIL, not silence). An UNKNOWN dynamic shape FAILS naming the line, so a
# future runtime-built registration cannot be invisible.
dyn_lines=$(grep -nE 'register_source\(' "$EI" | grep -vE 'register_source\([[:space:]]*"' \
            | grep -vE '^[0-9]+:[[:space:]]*//' || true)   # drop comment-only mentions
dyn=""
while IFS= read -r ln; do
  [ -z "$ln" ] && continue
  case "$ln" in
    *"register_source(eng->engine_name"*)
      # MondayRiskOn block: name = "<prefix>" + m.sym, one per mons[] roster entry.
      # Derive BOTH the prefix and the symbols from the code (derive-don't-copy).
      pfx=$(grep -oE 'engine_name *= *std::string\("[^"]+"\) *\+ *m\.sym' "$EI" \
            | sed -E 's/.*std::string\("([^"]+)"\).*/\1/' | head -1 || true)
      syms=$(grep -oE '\{ *&g_monday_[a-z0-9_]+, *"[^"]+"' "$EI" \
            | sed -E 's/.*"([^"]+)"/\1/' || true)
      if [ -z "$pfx" ] || [ -z "$syms" ]; then
        echo "  *** DYNAMIC register_source resolver parsed ZERO names (MondayRiskOn roster/prefix format drifted -- fix the resolver): $ln"
        fail=1
      else
        names=$(printf '%s\n' "$syms" | sed "s/^/${pfx}/")
        dyn="$dyn
$names"
      fi;;
    *"register_source(label,"*)
      # reg_straddle wrapper: names are the literal first args at its call sites.
      names=$(grep -oE 'reg_straddle\("[^"]+"' "$EI" | sed -E 's/reg_straddle\("//;s/"//' || true)
      if [ -z "$names" ]; then
        echo "  *** DYNAMIC register_source resolver parsed ZERO names (reg_straddle call sites not found): $ln"
        fail=1
      else
        dyn="$dyn
$names"
      fi;;
    *"register_source(tag,"*)
      # _ps_src wrapper: names are the literal first args at its call sites.
      names=$(grep -oE '_ps_src\("[^"]+"' "$EI" | sed -E 's/_ps_src\("//;s/"//' || true)
      if [ -z "$names" ]; then
        echo "  *** DYNAMIC register_source resolver parsed ZERO names (_ps_src call sites not found): $ln"
        fail=1
      else
        dyn="$dyn
$names"
      fi;;
    *)
      echo "  *** UNKNOWN DYNAMIC register_source shape -- the audit cannot resolve its display name."
      echo "      Use a string literal, or add a resolver case + persist wire for: $ln"
      fail=1;;
  esac
done <<< "$dyn_lines"
disp=$(printf '%s\n%s\n' "$disp" "$dyn" | grep -v '^$' | sort -u)

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
# S-2026-07-14: FxCrossRevEURGBP REMOVED -- dead entry; its source was un-registered
# S-2026-06-29 (FX cull), so the allowlist row guarded nothing.
allow="FvgContinuation FvgCont10m FvgCont30m GoldOrbRetrace GoldOrbRetraceLDN \
NqFutMomo DonchianPortfolio EmaPullbackPortfolio Us30Ensemble XauSessNYpm"
# QndxSqf (S-2026-07-11): SELF-RESTORING by construction, not dormant -- its legs
# re-derive target position from the daily-CSV replay + on-connect adopt
# (qndx_sqf_ibkr.cpp warm_from_csv/ingest_daily_bar). A persist wire would fight
# the signal-state reconcile. Allowed as designed-out, not as a gap.
allow="$allow QndxSqf"
# StockDipTurtle (S-2026-07-22) + DualMom (S-2026-07-23a): SELF-PERSISTING by
# construction, same class as QndxSqf -- each owns an atomic tmp+rename state file
# restored at boot (StockDipTurtleEngine cfg_.live_path save_live_/load_live_;
# DualMomentumEngine cfg.state_path save_/load_state incl. day counter + tokens).
# A PositionPersistence wire would double-restore. Allowed as designed-out.
allow="$allow StockDipTurtle DualMom"
# DayMover7 + Bigcap3G4 (S-2026-07-23): same SELF-PERSISTING class -- own atomic
# state files (daymover7_live.txt / bigcap3g4_live.txt, tmp+rename, load_state at
# boot incl. RETRY rows), DualMom pattern verbatim.
allow="$allow DayMover7 Bigcap3G4"
while read -r tag; do
  [ -z "$tag" ] && continue
  printf '%s\n' "$pers" | grep -qx "$tag" && continue
  printf '%s ' $allow | grep -qw "$tag" && continue
  echo "  *** PERSISTENCE GAP: '$tag' shows positions but has NO persist source -> add a wire in PositionPersistence.hpp"
  fail=1
done <<< "$disp"
if [ "$fail" -ne 0 ]; then
  echo "[persistence-audit] FAIL -- unpersisted display engine(s) / unresolvable dynamic registration(s) above."
  exit 1
fi
echo "[persistence-audit] OK -- every display engine (incl. dynamic-name registrations) persists (or is an allowlisted dormant engine)."
