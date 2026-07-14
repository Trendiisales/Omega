#!/bin/bash
# S63 build verify + backtest A/B comparison
# ============================================================================
# Runs the OmegaBacktest twice -- once with the existing (post-S62) binary
# which has NO LOSS_CUT/BE_RATCHET in the 7 newly-patched engines, and once
# after rebuilding to include the S63 patches. Compares per-engine results
# so the operator can decide whether to keep the new defaults, tune them,
# or revert to 0.0 (plumbing-only).
#
# Usage:
#   bash ~/Omega/S63_build_verify_compare.sh [tick_csv]
#
# Default tick file: ~/tick/data/xauusd_2024_2025.csv (same default as the
# build_mac.sh / run_grid.sh in this repo). Override with a path arg to
# test a different dataset. The OmegaBacktest parser only accepts:
#   A: timestamp_ms,bid,ask
#   B: timestamp_ms,bid,ask,vol
#   C: YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol  (Dukascopy)
#   D: timestamp_ms,open,high,low,close,vol (5-column OHLCV)
# The data/l2_ticks_*.csv files (14 columns) are NOT compatible and will
# produce "[ERROR] No valid ticks parsed."
#
# Outputs (all in /Users/jo/Omega/outputs/):
#   s63_baseline_trades.csv   raw trade tape, pre-S63 binary
#   s63_patched_trades.csv    raw trade tape, post-S63 binary
#   s63_baseline_report.csv   per-engine aggregate, pre-S63
#   s63_patched_report.csv    per-engine aggregate, post-S63
#   s63_compare_summary.md    side-by-side delta report
#
# This script is idempotent: re-running clobbers prior outputs and reruns
# both backtests from scratch.
# ============================================================================

set -e
cd "$HOME/Omega"

TICK_FILE="${1:-$HOME/tick/data/xauusd_2024_2025.csv}"
OUT="outputs"
mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# 0. Sanity: only the 7 engine headers + this script should be dirty.
# ---------------------------------------------------------------------------
echo "=== [S63] git status BEFORE A/B ==="
git status --short

EXPECTED=(
  "include/PDHLReversionEngine.hpp"
  "include/RSIReversalEngine.hpp"
  "include/CrossAssetEngines.hpp"
  "include/XauusdFvgEngine.hpp"
  "include/XauThreeBar30mEngine.hpp"
  "include/IndexFlowEngine.hpp"
)
UNEXPECTED=$(git diff --name-only \
              ':(exclude)include/PDHLReversionEngine.hpp' \
              ':(exclude)include/RSIReversalEngine.hpp' \
              ':(exclude)include/CrossAssetEngines.hpp' \
              ':(exclude)include/XauusdFvgEngine.hpp' \
              ':(exclude)include/XauThreeBar30mEngine.hpp' \
              ':(exclude)include/IndexFlowEngine.hpp' \
              ':(exclude)S63_build_verify_compare.sh' \
              ':(exclude)outputs/SESSION_HANDOFF_2026-05-13b.md' \
              ':(exclude)docs/handoffs/SESSION_HANDOFF_2026-05-13a.md' \
              ':(exclude)outputs/IN_FLIGHT_PROTECTION_AUDIT_2026-05-13.md' \
              ':(exclude)outputs/GBP_LDN_OPEN_LOCKOUT_DIAGNOSIS_2026-05-13.md' \
              || true)
if [ -n "$UNEXPECTED" ]; then
  echo "!! [S63] WARN: unexpected files in working tree:"
  echo "$UNEXPECTED"
  echo "!! continuing anyway -- review before commit."
fi

# ---------------------------------------------------------------------------
# 1. Pre-flight: tick file must exist.
# ---------------------------------------------------------------------------
if [ ! -f "$TICK_FILE" ]; then
  echo "!! [S63] tick file not found: $TICK_FILE"
  echo "!! available candidates:"
  find backtest -maxdepth 2 -name "*.csv" -size +1M | head -10
  exit 1
fi
echo "=== [S63] using tick file: $TICK_FILE ==="

# Auto-detect Dukascopy 5-column format (timestamp_ms,ask,bid,ask_vol,bid_vol)
# and preprocess to 3-column (ts,ask,bid). OmegaBacktest's sniff() counts
# commas in the first data line and treats >=4 commas as OHLCV -- which mis-
# parses the 5-col Dukascopy export. Preprocessing keeps the header in the
# "ask before bid" order so the parser's ask-first detection sets the
# columns correctly.
TICK_HEADER=$(head -1 "$TICK_FILE")
TICK_COMMAS=$(echo "$TICK_HEADER" | tr -cd ',' | wc -c | tr -d ' ')
if echo "$TICK_HEADER" | grep -qiE '(ask.*bid|bid.*ask)' && [ "$TICK_COMMAS" -ge 4 ]; then
  PREP_FILE="$OUT/s63_tick_3col.csv"
  echo "=== [S63] preprocessing 5-col Dukascopy CSV -> 3-col ts,ask,bid -> $PREP_FILE ==="
  # Detect column order from header
  if echo "$TICK_HEADER" | awk -F, '{
        ask_col=0; bid_col=0;
        for (i=1; i<=NF; i++) {
          if (tolower($i) ~ /^ask$/) ask_col=i;
          if (tolower($i) ~ /^bid$/) bid_col=i;
        }
        print ask_col" "bid_col
      }' | read ASK_COL BID_COL; :; then :; fi
  # Re-do the parse simply
  ASK_COL=$(echo "$TICK_HEADER" | awk -F, '{for(i=1;i<=NF;i++) if(tolower($i)=="ask") print i}')
  BID_COL=$(echo "$TICK_HEADER" | awk -F, '{for(i=1;i<=NF;i++) if(tolower($i)=="bid") print i}')
  if [ -z "$ASK_COL" ] || [ -z "$BID_COL" ]; then
    echo "!! [S63] could not locate ask/bid columns in header: $TICK_HEADER"
    exit 1
  fi
  echo "=== [S63] header has ask=col$ASK_COL bid=col$BID_COL ==="
  awk -F, -v ac="$ASK_COL" -v bc="$BID_COL" '
    NR==1 { print "timestamp_ms,ask,bid"; next }
    { print $1","$ac","$bc }
  ' "$TICK_FILE" > "$PREP_FILE"
  PREP_LINES=$(wc -l < "$PREP_FILE")
  echo "=== [S63] preprocessed $PREP_LINES rows ==="
  TICK_FILE="$PREP_FILE"
fi

# ---------------------------------------------------------------------------
# 2. Capture pre-S63 binary as baseline.
#    The /build/OmegaBacktest binary as of right now was built BEFORE the
#    S63 patches were applied to working tree -- it represents post-S62 HEAD.
# ---------------------------------------------------------------------------
if [ ! -f build/OmegaBacktest ]; then
  echo "!! [S63] /build/OmegaBacktest missing -- nothing to use as baseline."
  echo "!! run cmake --build build --target OmegaBacktest -j once first."
  exit 1
fi
cp build/OmegaBacktest build/OmegaBacktest_s63_baseline
echo "=== [S63] baseline binary cached at build/OmegaBacktest_s63_baseline ==="

# ---------------------------------------------------------------------------
# 3. Run baseline (no S63).
# ---------------------------------------------------------------------------
echo "=== [S63] running BASELINE backtest (no S63 patches in binary) ==="
./build/OmegaBacktest_s63_baseline "$TICK_FILE" \
  --engine all \
  --trades "$OUT/s63_baseline_trades.csv" \
  --report "$OUT/s63_baseline_report.csv" \
  2>&1 | tail -50

# ---------------------------------------------------------------------------
# 4. Rebuild -- new binary incorporates S63 patches.
# ---------------------------------------------------------------------------
echo "=== [S63] rebuilding OmegaBacktest with S63 patches ==="
cmake --build build --target OmegaBacktest -j

# ---------------------------------------------------------------------------
# 5. Run patched.
# ---------------------------------------------------------------------------
echo "=== [S63] running PATCHED backtest (S63 active) ==="
./build/OmegaBacktest "$TICK_FILE" \
  --engine all \
  --trades "$OUT/s63_patched_trades.csv" \
  --report "$OUT/s63_patched_report.csv" \
  2>&1 | tail -50

# ---------------------------------------------------------------------------
# 6. Compare via inline Python (uses pandas if available, else stdlib).
# ---------------------------------------------------------------------------
echo "=== [S63] running A/B comparison ==="
python3 - "$OUT/s63_baseline_trades.csv" "$OUT/s63_patched_trades.csv" \
         "$OUT/s63_compare_summary.md" <<'PY'
import sys, csv, statistics, os
from collections import defaultdict

baseline_path, patched_path, out_md = sys.argv[1], sys.argv[2], sys.argv[3]

# Engines we care about for S63 verification.
TARGET_ENGINES = {
    # canonical reference (should be unchanged)
    "VWAPReversion",
    # Tier 1 (full pattern added)
    "NoiseBandMomentum", "NBM",
    "PDHLReversion",
    # Tier 2 (full or partial pattern added)
    "RSIReversal",
    "XauusdFvg",
    "XauThreeBar30m",
    "IndexMacroCrash", "IMACRO",
    "IndexFlow", "IFLOW",
}

def load_trades(path):
    by_engine = defaultdict(list)
    if not os.path.exists(path):
        return by_engine
    with open(path) as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            eng = row.get("engine", "")
            by_engine[eng].append(row)
    return by_engine

def summarize(rows):
    n = len(rows)
    if n == 0:
        return dict(n=0, pnl=0.0, wins=0, wr=0.0, mae_p10=0.0, mfe_p70=0.0,
                    loss_cuts=0, be_cuts=0, sl_hits=0, tp_hits=0, timeouts=0, others=0)
    pnl = sum(float(r.get("pnl", 0) or 0) for r in rows)
    wins = sum(1 for r in rows if float(r.get("pnl", 0) or 0) > 0)
    maes = sorted([abs(float(r.get("mae", 0) or 0)) for r in rows])
    mfes = sorted([float(r.get("mfe", 0) or 0) for r in rows])
    def pct(arr, p):
        if not arr: return 0.0
        k = int(round((p / 100.0) * (len(arr) - 1)))
        return arr[k]
    reasons = defaultdict(int)
    for r in rows:
        reasons[r.get("exitReason", "")] += 1
    return dict(
        n=n, pnl=pnl, wins=wins, wr=wins / n if n else 0.0,
        mae_p10=pct(maes, 10), mfe_p70=pct(mfes, 70),
        loss_cuts=reasons.get("LOSS_CUT", 0),
        be_cuts=reasons.get("BE_CUT", 0),
        sl_hits=reasons.get("SL_HIT", 0),
        tp_hits=reasons.get("TP_HIT", 0),
        timeouts=reasons.get("MAX_HOLD", 0) + reasons.get("TIMEOUT", 0)
                + reasons.get("TIME_STOP", 0),
        others=n - sum([reasons.get(k, 0) for k in
                        ("LOSS_CUT","BE_CUT","SL_HIT","TP_HIT","MAX_HOLD",
                         "TIMEOUT","TIME_STOP")]),
    )

baseline = load_trades(baseline_path)
patched  = load_trades(patched_path)

engines = sorted(set(list(baseline.keys()) + list(patched.keys())))

lines = []
lines.append("# S63 In-Flight Protection -- A/B Backtest Comparison\n")
lines.append("Tick file: `" + os.environ.get("S63_TICK", "(see script header)") + "`\n")
lines.append("\n## Per-engine summary\n\n")
lines.append("| engine | n_base | n_patched | pnl_base | pnl_patched | dpnl | wr_base | wr_patched | LOSS_CUT | BE_CUT | TP_base->patched | SL_base->patched | TO_base->patched |\n")
lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|\n")

target_seen = []
for eng in engines:
    bs = summarize(baseline.get(eng, []))
    ps = summarize(patched.get(eng, []))
    is_target = eng in TARGET_ENGINES or any(t in eng for t in TARGET_ENGINES)
    if is_target:
        target_seen.append(eng)
    marker = "**" if is_target else ""
    lines.append(
        f"| {marker}{eng}{marker} | {bs['n']} | {ps['n']} | "
        f"{bs['pnl']:.2f} | {ps['pnl']:.2f} | {ps['pnl']-bs['pnl']:+.2f} | "
        f"{bs['wr']*100:.1f}% | {ps['wr']*100:.1f}% | "
        f"{ps['loss_cuts']} | {ps['be_cuts']} | "
        f"{bs['tp_hits']}->{ps['tp_hits']} | "
        f"{bs['sl_hits']}->{ps['sl_hits']} | "
        f"{bs['timeouts']}->{ps['timeouts']} |\n"
    )

lines.append("\n## Target engines detected in this run: " + (", ".join(target_seen) if target_seen else "NONE") + "\n")

lines.append("\n## Decision matrix (per target engine):\n\n")
lines.append("- **dpnl > 0** AND **LOSS_CUT/BE_CUT firing**: ship as-is.\n")
lines.append("- **dpnl < 0** OR **excessive LOSS_CUT firing replacing winners**: set that engine's _PCT to 0.0 in the header before commit.\n")
lines.append("- **n_patched == 0** while n_base > 0: investigate -- patch may have broken the engine.\n")
lines.append("- **VWAPReversion dpnl != 0**: investigation required -- VWR was the reference and should be unchanged.\n")
lines.append("- **engine not in 'Target engines detected'**: this tick file didn't fire that engine. Try a longer tick file or a different symbol coverage.\n")

with open(out_md, "w") as f:
    f.writelines(lines)
print(f"=== [S63] wrote {out_md} ===")
PY

S63_TICK="$TICK_FILE" python3 - <<PYEND
import os
print("=== [S63] summary preview ===")
with open(os.path.expanduser("~/Omega/$OUT/s63_compare_summary.md")) as f:
    print(f.read())
PYEND

echo "=== [S63] DONE -- review $OUT/s63_compare_summary.md ==="
echo "    If acceptable: bash S63_commit_push.sh (TODO)"
echo "    If not       : edit defaults in headers, re-run this script"
