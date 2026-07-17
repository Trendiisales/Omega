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
# engine_init.hpp OR omega_main.hpp is a hard FAIL. Index engines ($-normalized
# lot 0.3-3.0 convention) are exempt by symbol. Deliberate exceptions: annotate
# the line with LOT-GATE-OK and the evidence.
# S-2026-07-14 (latent-class sweep P1-2): omega_main.hpp added — the 5 MGC
# engines configured there (lot = CONTRACTS, 1.0 = 1 micro, intentional +
# annotated) were invisible to this gate, so a fat-finger lot=100 there would
# have shipped unchecked.
echo ""
echo "[mac-canary-engines] gold lot-size gate (XAU/gold .lot <= 0.05)..."
LOT_VIOLATIONS=$(grep -nE "\.lot *= *[0-9.]+" include/engine_init.hpp include/omega_main.hpp \
  | grep -iE "xau|gold|xag|mgc" \
  | grep -v "LOT-GATE-OK" \
  | awk '{ if (match($0, /\.lot *= *[0-9.]+/)) { v=substr($0,RSTART,RLENGTH); sub(/.*= */,"",v); if (v+0 > 0.05) print } }')
if [ -n "$LOT_VIOLATIONS" ]; then
  echo "[mac-canary-engines] FAIL: gold-symbol engine with lot > 0.05 (the 100x-bug class):"
  echo "$LOT_VIOLATIONS"
  exit 1
fi
echo "[mac-canary-engines] gold lot-size gate PASS"

# SEED-REGISTRY STRUCTURAL GATE (added S-2026-07-14, operator: "never have these seed /
# staleness issues again"): every ACTIVE warm-seed (string-literal AND dynamic Survivor
# paths) must be refreshed by tools/seed_refresh.py or carry a KNOWN_UNREFRESHED owner
# entry. --registry-only = structural only (no freshness), so this gate is deterministic
# and can't false-fail a commit because of calendar rot. History: warmup_USTEC.F_H4 (94d)
# and warmup_GBPUSD_H1 (94d, live FxLadder cell) both rotted because a seed could be ADDED
# with no refresh path and nothing objected. Now the commit itself fails.
echo ""
echo "[mac-canary-engines] seed-registry structural gate (every active seed has a refresh path)..."
python3 "$(dirname "$0")/../tools/seed_freshness_audit.py" --registry-only || {
  echo "[mac-canary-engines] FAIL: seed-registry violation (see NO-REFRESH-PATH lines above)"
  exit 1
}

# DEAD-BOX REFERENCE GATE (added S-2026-07-14, latent-class sweep item 13): the live
# box is omega-new = 45.85.3.79; omega-vps = 185.167.119.59 is the RETIRED 2026-07-07
# box. On 2026-07-10 a whole session deployed to the dead box for hours because tooling
# still said omega-vps. All operational refs were repointed/guarded this session, but
# nothing stopped a NEW script (or a pasted ssh line) from reintroducing the dead box --
# this gate does: any omega-vps / 185.167.119.59 ref in tools/ scripts/ include/ src/
# or top-level ps1/sh/py outside the reasoned allowlist (intentional history comments +
# refuse-guards only) FAILS the canary.
echo ""
echo "[mac-canary-engines] dead-box reference gate (omega-vps must never be a target)..."
bash "$(dirname "$0")/deadbox_ref_audit.sh" || {
  echo "[mac-canary-engines] FAIL: dead-box (omega-vps/185.167.119.59) reference outside the allowlist."
  exit 1
}

# UNGATED-ENGINE AUDIT (added S-2026-07-14, latent-class sweep item 10): CLAUDE.md
# "Standing Audit Checks" §1 promoted from an inline 2-idiom grep (which had ZERO
# coverage of the pos_active_ / legs_.push_back / w_=want opener idioms — MgcFast/
# MgcSlow, CrossSectionalIndex, GoldTsmomD1V2 were invisible) to a script that
# DERIVES the wide ENTRY_RE from adverse_protection_audit.sh at runtime
# (derive-don't-copy; zero-parse = FAIL). Expected hits + reasons live in
# scripts/ungated_engine_allowlist.txt. A NEW unexplained ungated opener FAILS.
echo ""
echo "[mac-canary-engines] ungated-engine audit (every opener header cost-gated or documented)..."
bash "$(dirname "$0")/ungated_engine_audit.sh" || {
  echo "[mac-canary-engines] FAIL: ungated-engine audit -- see scripts/ungated_engine_audit.sh output above."
  exit 1
}

# ROSTER-PARITY GATE (added S-2026-07-14, latent-class sweep item 14a/14b): hand-mirrored
# duplicates that are in-sync today WILL drift -- bridge STOCKS vs engine_init BIGCAP_LAD
# (must be EQUAL), the IBKR futures contract map (IbkrExecutionEngine FUT rows must be a
# SUBSET of bridge INDEX_FUTURES with matching contract tuples; exec-only gold rows
# documented in the script), and register_omega_ibkr_bridge.ps1 ($Symbols resolvable +
# SYM(CONTRACT) comment tokens agree). Every copy is parsed from its source file; a
# parser matching ZERO entries fails loudly (the USTEC.F_H4 blindness class).
echo ""
echo "[mac-canary-engines] roster-parity audit (hand-mirrored rosters/contract maps in sync)..."
python3 "$(dirname "$0")/roster_parity_audit.py" --repo "$(dirname "$0")/.." || {
  echo "[mac-canary-engines] FAIL: a hand-mirrored roster/contract-map copy drifted -- see PARITY-FAIL lines above."
  exit 1
}

# PRE-BE LOSS GATE (added S-2026-07-17, operator mandate "why is there not a check",
# enforcing feedback-no-prebe-loss-ever). The adverse-protection audit above only
# checks a verdict TAG is PRESENT; it does NOT inspect the booking path, so every
# vulnerable companion (FxUpJumpLadderCompanion LOSS_CUT/sub-BE TRAIL; GoldTrendMimicLadder
# pre-arm LOSS_CUT/WINDOW_CAP; BeCascadeCompanionEngine PREBE_CUT/PREBE_STOP/REVERSAL_CUT)
# carried a tag, PASSED that audit, yet booked clips net<0 BEFORE break-even was covered.
# This gate flags any companion/mimic/ladder booking site that can settle pre-BE-negative
# and is NOT BE-floor-on-open protected (confirm_anchor_epx / be_floor_on_open / entry-clamp
# marker) or grandfathered in scripts/prebe_loss_allowlist.txt. Currently-vulnerable engines
# are grandfathered (backfill-owed, floor fixes landing this session); a NEW unprotected
# booking site FAILS. Complements (does not duplicate) the adverse audit -- see
# scripts/PREBE_LOSS_GATE.md.
echo ""
echo "[mac-canary-engines] pre-BE loss gate (no companion clip books net<0 before break-even)..."
bash "$(dirname "$0")/prebe_loss_audit.sh" || {
  echo "[mac-canary-engines] FAIL: a companion can book a pre-BE-negative clip -- see prebe_loss_audit.sh output above."
  exit 1
}

# PERSIST/RESTORE SEMANTICS GATE (added S-2026-07-17k, handoff item 3 "confirm no
# more trade cancellations on deploy"): closes the c1903e88 IndexBearShort class
# structurally -- (1) every PositionSnapshot.entry_ts write must be visibly
# /1000-converted, seconds-native-commented, or documented in
# scripts/persist_restore_allowlist.txt (an ms value in the seconds field broke
# the phantom-drop exemption -> a restored position's close silently vanished
# from the ledger); (2) every engine with a restore path + a bar-seq/hold-counter
# time exit must re-anchor its hold clock inside restore (or restore a persisted
# held counter) or be documented (unrestored entry_bar_seq while seed replay
# advances the bar counter -> spurious TIME_STOP at boot). A NEW unexplained hit
# = exit 1 = P1 (the deploy-eats-trades class).
echo ""
echo "[mac-canary-engines] persist/restore semantics audit (snapshot ts units + hold-clock re-anchor)..."
bash "$(dirname "$0")/persist_restore_semantics_audit.sh" || {
  echo "[mac-canary-engines] FAIL: persist/restore semantics audit -- see scripts/persist_restore_semantics_audit.sh output above."
  exit 1
}

exit 0
