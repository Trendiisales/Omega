#!/bin/bash
# scripts/mac_canary_engines.sh
# ---------------------------------------------------------------------
# Mac canary syntax check for engine headers that the main OmegaBacktest
# target doesn't compile. Catches MSVC-class C++ type/init errors that
# clang+OmegaBacktest pass silently (e.g. AtomicL2 undeclared, mixed
# designated/positional aggregate init, ODR-violating header definitions).
#
# Each header is compiled in isolation with `clang++ -fsyntax-only -std=c++20
# -Iinclude` plus a tiny one-line driver that #includes it and defines a
# stub main(). Headers that need globals.hpp expansions get
# `-include L2Globals.hpp` injected.
#
# Run before any commit that touches an engine header:
#   bash scripts/mac_canary_engines.sh
#
# Exits 0 if all headers compile, nonzero on first failure.
# ---------------------------------------------------------------------
set -u
INCLUDE="-Iinclude"
STD="-std=c++20"
# -Werror=unused-*: MSVC builds with /WX where C4189 (unused local) is a hard error;
# clang defaulting these to off let unused locals pass canary then break the VPS build
# (memory: project-msvc-c4189-canary-gap). Keep as errors to match MSVC.
WARN="-Wall -Wextra -Werror=unused-variable -Werror=unused-but-set-variable -Wno-c++20-extensions"

# Headers to canary-check (target = file containing the issue class we missed)
# Self-contained or only depend on L2Globals/CellPrimitives/TradeLedger:
HEADERS=(
  include/StockDayMoverLadderCompanion.hpp
  include/BigCap2pctImpulseCompanion.hpp
  include/GoldBeFloorCompanion.hpp
  include/FxBeFloorCompanion.hpp
  include/IndexBeFloorCompanion.hpp
  include/StallCompanion.hpp
  include/L2Globals.hpp
  include/L2LeverageState.hpp
  include/SurvivorPortfolio.hpp
  include/PumpScalpManager.hpp
  include/NqMomentumEngine.hpp
  include/DonchianEngine.hpp
  include/EmaPullbackEngine.hpp
  include/IndexFlowEngine.hpp
  include/XauTrendFollow4hEngine.hpp
  include/XauTrendFollow1hEngine.hpp
  include/Ger40KeltnerH1Engine.hpp
  include/BreakBounceEngine.hpp
  include/TrendLineBreakEngine.hpp
  include/IndexSessionEngine.hpp
  include/SessionMomentumEngine.hpp
  include/IndexSeasonalEngine.hpp
  include/CalendarTomEngine.hpp
  include/MgcSlowDonchian30mEngine.hpp
  include/GoldTsmomD1V2Engine.hpp
  include/CrossSectionalIndexEngine.hpp
  include/GoldSeasonalEngine.hpp
  include/GoldOversoldBounceEngine.hpp
  include/IndexFomcEngine.hpp
  include/IndexRiskGate.hpp
  include/Us30EnsembleEngine.hpp
  include/IndexIntradayDriftEngine.hpp
  include/Ger40TurtleH4Engine.hpp
  include/NasTurtleD1Engine.hpp
  include/PeachyOrbEngine.hpp
  include/QndxStrat.hpp
  include/QndxSqfIbkr.hpp
)

fails=0
for h in "${HEADERS[@]}"; do
  if [ ! -f "$h" ]; then
    echo "[skip] $h: not found"
    continue
  fi
  tmp=$(mktemp -t mac_canary_XXXXXX.cpp)
  base=$(basename "$h")
  cat > "$tmp" <<EOF
#include "$base"
int main() { return 0; }
EOF
  out=$(clang++ -fsyntax-only $STD $INCLUDE $WARN -x c++ "$tmp" 2>&1)
  rc=$?
  rm -f "$tmp"
  if [ $rc -ne 0 ]; then
    echo "===================="
    echo "[FAIL] $h"
    echo "$out" | head -40
    echo "===================="
    fails=$((fails + 1))
  else
    echo "[ok]   $h"
  fi
done

if [ $fails -gt 0 ]; then
  echo ""
  echo "[mac-canary-engines] $fails header(s) failed -- fix before commit."
  exit 1
fi
echo "[mac-canary-engines] all $((${#HEADERS[@]})) headers ok"

# ---------------------------------------------------------------------
# STANDING GATE — adverse-protection mandate (added 2026-06-19 after the
# NqMomentum no-protection regression). Every NEW position-opening engine must
# carry a backtested ADVERSE-PROTECTION verdict. Makes the rule a build gate,
# not an advisory memory note.
# ---------------------------------------------------------------------
echo ""
echo "[mac-canary-engines] running adverse-protection audit..."
bash "$(dirname "$0")/adverse_protection_audit.sh" || {
  echo "[mac-canary-engines] adverse-protection audit FAILED -- fix before commit."
  exit 1
}

# STANDING REGISTRY (added 2026-06-24): regenerate ENGINE_REGISTRY.md = every
# enabled engine -> verdict + in-book + price-bear status. Informational (does
# NOT block on the known UNAUDITED set yet) -- surfaces enabled-but-unaudited.
echo ""
echo "[mac-canary-engines] regenerating engine registry..."
python3 "$(dirname "$0")/../tools/engine_registry.py" --repo "$(dirname "$0")/.." >/dev/null 2>&1
echo "  -> ENGINE_REGISTRY.md ($(grep -c '^| g_' "$(dirname "$0")/../ENGINE_REGISTRY.md" 2>/dev/null) engines, $(grep -cE '\| UNAUDITED \|' "$(dirname "$0")/../ENGINE_REGISTRY.md" 2>/dev/null) unaudited)"

# TOMBSTONE GUARD (added 2026-06-24L): HARD-BLOCK resurrection of a tombstoned
# engine. Overrides AUDITED_CONFIGS -- a fresh EDGE row cannot un-tombstone a
# FORBIDDEN engine. This is the gate that makes a tombstone STICK (GoldOrb was
# un-tombstoned 3x before this existed). FAILS the canary if any blocklisted
# global in backtest/TOMBSTONES.tsv is enabled=true.
echo ""
echo "[mac-canary-engines] tombstone guard (anti-resurrection)..."
python3 "$(dirname "$0")/../tools/tombstone_guard.py" --repo "$(dirname "$0")/.." || {
  echo "[mac-canary-engines] FAIL: a tombstoned engine is enabled -- see backtest/TOMBSTONES.tsv"
  exit 1
}

# FAIL-VERDICT GUARD (added S-2026-07-12, never-again audit): an engine with a recorded
# true-cost FAIL backtest may NOT be enabled. The tombstone guard only covers TOMBSTONED
# engines; on 2026-07-12 three true-cost-FAIL engines (XauTrendFollow2h, GoldPanicBounce,
# XauTrendRiderD1 -- GOLD_PHASE1B) were live until the operator ordered a cull. FAILS the
# canary if any global in backtest/FAIL_VERDICTS.tsv is enabled=true.
echo ""
echo "[mac-canary-engines] fail-verdict guard (true-cost-FAIL engines stay disabled)..."
python3 "$(dirname "$0")/../tools/fail_verdict_guard.py" --repo "$(dirname "$0")/.." || {
  echo "[mac-canary-engines] FAIL: an engine with a recorded true-cost FAIL is enabled -- see backtest/FAIL_VERDICTS.tsv"
  exit 1
}

# STANDING GATE — live-dump staleness monitor (added S-2026-07-02). Every CSV the
# VPS binary writes via set_live_dump() must be listed in tools/live_dump_manifest.tsv
# so tools/feeds_selftest.py can poll its VPS freshness. Closes the blind spot where a
# VPS writer dies but every Mac banner stays GREEN. WARN-only (mirrors informational
# gates); set STRICT=1 to hard-fail on an unmonitored dump.
echo ""
echo "[mac-canary-engines] live-dump freshness audit..."
bash "$(dirname "$0")/../tools/live_dump_freshness_audit.sh" || {
  echo "[mac-canary-engines] live-dump freshness audit FAILED -- a live dump is unmonitored."
  exit 1
}

# MIMIC DRAWDOWN-CANCEL GATE (added S-2026-07-11): every ACTIVE mimic/companion book
# must carry a backtested DRAWDOWN-CANCEL verdict. A mimic never touches the real trade,
# so a cold cut is FREE protection -> the lever is mandatory on ALL of them (operator
# "enforce it"). Retired BE-floors grandfathered via scripts/mimic_drawdown_legacy.txt.
echo ""
echo "[mac-canary-engines] mimic drawdown-cancel gate (every active mimic carries an lc verdict)..."
bash "$(dirname "$0")/mimic_drawdown_audit.sh" || {
  echo "[mac-canary-engines] FAIL: an active mimic ships without a backtested drawdown-cancel verdict."
  exit 1
}

# PNL COMPLETENESS GATE (added S-2026-07-11): every money book must reach the ALL-TIME
# headline. Blocks an orphaned endpoint (BigCap2pct: existed, never folded) or an assigned
# total-global that updDayPnl never references. Operator "i have been looking at half the pnl".
echo ""
echo "[mac-canary-engines] pnl completeness gate (every money book folds into ALL-TIME)..."
bash "$(dirname "$0")/mimic_pnl_completeness_gate.sh" || {
  echo "[mac-canary-engines] FAIL: a money book does not reach the ALL-TIME headline."
  exit 1
}

# TRADE-VISIBILITY GATE (added S-2026-07-12, never-again audit class A): every trade-
# producing book across ALL systems (omega-new engines/companions/ladders, Mac crypto
# books, josgp1 chimera) must reach the desk or be explicitly research/retired/in-flight
# in tools/trade_visibility_manifest.tsv. The pnl gate above covers endpoint<->fold; this
# covers producer<->endpoint — the class where an entire system (chimera) traded invisibly
# for weeks. STRANDED row or an unmanifested new endpoint/push = FAIL.
echo ""
echo "[mac-canary-engines] trade-visibility gate (every producer reaches the desk)..."
bash "$(dirname "$0")/trade_visibility_gate.sh" || {
  echo "[mac-canary-engines] FAIL: a trade-producing book is invisible to the desk (or manifest drift)."
  exit 1
}

# GUI DRIFT GATE (added S-2026-07-06): HARD-BLOCK a hand-edited OmegaIndexHtml.hpp.
# include/OmegaIndexHtml.hpp MUST equal regen(tools/gui/omega_desk.html). Three GUI
# commits drifted the .hpp from its .html source with only a prose "do not hand-edit"
# comment guarding it. This is the gate that makes the generator contract STICK.
echo ""
echo "[mac-canary-engines] GUI drift gate (OmegaIndexHtml.hpp == regen(omega_desk.html))..."
bash "$(dirname "$0")/../tools/gui/gui_drift_gate.sh" || {
  echo "[mac-canary-engines] FAIL: GUI header drifted from its .html source -- see tools/gui/gui_drift_gate.sh"
  exit 1
}

# S-2026-06-26 PERSISTENCE ENFORCEMENT: fail if any display engine lacks a persist source.
# S-2026-07-11 PHASE 1b: this call had been left BELOW an `exit 0` (unreachable) when a
# later gate was appended above it -- the audit never ran in the canary and 4 unpersisted
# display engines accumulated (ConnorsRSI2, GoldPanicBounce, SpxTurtleD1, QndxSqf).
# Moved back into the live path; the gaps were wired/justified the same commit.
echo ""
echo "[mac-canary-engines] persistence audit (every display engine persists)..."
bash "$(dirname "$0")/persistence_audit.sh" || exit 1

# GOLD LOT-SIZE HARD GATE (added S-2026-07-13, SESSION_HANDOFF_2026-07-13b §5):
# the −$3,953 up-jump incident shipped XAUUSD engines at lot=1.0 — 100x the 0.01
# desk convention — and NO gate existed to catch it. Any gold-symbol engine
# (line mentions xau/gold/xag/mgc, case-insensitive) assigning .lot > 0.05 in
# engine_init.hpp is a hard FAIL. Index engines ($-normalized lot 0.3-3.0
# convention) are exempt by symbol. Deliberate exceptions: annotate the line
# with LOT-GATE-OK and the evidence.
echo ""
echo "[mac-canary-engines] gold lot-size gate (XAU/gold .lot <= 0.05)..."
LOT_VIOLATIONS=$(grep -nE "\.lot *= *[0-9.]+" include/engine_init.hpp \
  | grep -iE "xau|gold|xag|mgc" \
  | grep -v "LOT-GATE-OK" \
  | awk '{ if (match($0, /\.lot *= *[0-9.]+/)) { v=substr($0,RSTART,RLENGTH); sub(/.*= */,"",v); if (v+0 > 0.05) print } }')
if [ -n "$LOT_VIOLATIONS" ]; then
  echo "[mac-canary-engines] FAIL: gold-symbol engine with lot > 0.05 (the 100x-bug class):"
  echo "$LOT_VIOLATIONS"
  exit 1
fi
echo "[mac-canary-engines] gold lot-size gate PASS"

exit 0
