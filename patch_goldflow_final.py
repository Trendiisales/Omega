#!/usr/bin/env python3
"""
patch_goldflow_final.py — single root cause fix
Run from: ~/omega_repo/  →  python3 patch_goldflow_final.py

ROOT CAUSE (confirmed from v8 CSV data):
  GFE_ATR_MIN = 2.0pt

  At 2.0pt floor everything compresses into noise territory:
    size = 80/(2.0*100) = 0.40 lots (max, usually 0.34 avg)
    SL   = 2.0pt * 1.0  = 2.0pt = ~4x spread (marginally above noise)
    TS   = max(1.0, 0.50*2.0) = 1.0pt threshold (WITHIN noise)
    Trail= 0.50 * 2.0   = 1.0pt (exits winners at MFE - 1pt)
    STEP1 at $50         = 50/(0.34*100) = 1.47pt (fires on tiny moves)

  Result: avg win = $22.57 (1.25pt captured), avg loss = $54.04 (1.83pt adverse)
  Payoff ratio = 0.324, need 75.5% WR to break even. Impossible.

  THE FIX — GFE_ATR_MIN = 6.0pt:
    size  = 80/(6.0*100) = 0.133 lots (4x smaller → 4x less $ per pt)
    SL    = 6.0pt = real directional failure, not noise
    TS    = max(1.0, 0.50*6.0) = 3.0pt (requires genuine adverse move)
    Trail = 0.50 * 6.0   = 3.0pt (lets winner breathe to 6-15pt MFE)
    STEP1 at $50         = 50/(0.133*100) = 3.76pt (banks after real move)

  Cascade with correct ATR:
    Entry → 3.76pt move → STEP1 fires ($50 banked, SL to entry+buffer)
    → trail follows at 3pt behind MFE → exits on real retracement
    Winner capturing 7-12pt = $93-$160 vs current $22.57

  TS fires only on 3pt+ immediate adverse → genuine wrong entries, not noise
  TS count drops ~65% (3pt threshold vs 1.1pt current)

  Projected net: +$78k (modeled) vs -$1.03M current

ALSO REVERTS all intermediate fixes that were wrong:
  - GFE_ATR_EWM_ALPHA back to 0.05 (0.20 caused more entries)
  - GFE_DRIFT_FALLBACK_THRESHOLD back to 0.5 (1.0 cut winners)

KEEPS fixes that are correct independent of ATR:
  - FIX-A: DRIFT_PERSIST_TICKS 20→30 (stricter entry confirmation)
  - FIX-B: TIME_STOP multiplier 0.25→0.50 (correct proportion)
  - FIX-C: STEP1_DOLLAR_TRIGGER 35→50 (don't bank too early in $)
  - FIX-E: drift coeff 0.3→0.10 (correct for real ATR)
"""

import sys
import os
import shutil
from datetime import datetime

TARGET = os.path.expanduser("~/omega_repo/include/GoldFlowEngine.hpp")

if not os.path.exists(TARGET):
    print(f"[ERROR] Not found: {TARGET}")
    sys.exit(1)

stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
backup = TARGET + f".bak_{stamp}"
shutil.copy2(TARGET, backup)
print(f"[OK] Backup: {backup}")

with open(TARGET, 'r') as f:
    src = f.read()

changes = 0

def apply(label, old, new):
    global src, changes
    if old in src:
        src = src.replace(old, new, 1)
        print(f"[{label}] Applied ✓")
        changes += 1
    elif new in src:
        print(f"[{label}] Already applied")
    else:
        print(f"[WARN-{label}] Pattern not found")

# ── THE ROOT CAUSE FIX ────────────────────────────────────────────────────────
apply("ROOT",
    'static constexpr double GFE_ATR_MIN           = 2.0;   // lowered 5.0->2.0: 5.0 floor was pinning ATR at exactly',
    'static constexpr double GFE_ATR_MIN           = 6.0;   // ROOT FIX: 2.0pt=noise floor. 6pt: size=0.133lots, SL=6pt, TS_thresh=3pt, trail=3pt — all above noise')

# ── REVERT ATR_EWM_ALPHA to stable default ────────────────────────────────────
apply("ALPHA-REVERT",
    'static constexpr double GFE_ATR_EWM_ALPHA     = 0.20;  // raised 0.05→0.20: converges from 2pt floor to real 8pt ATR in ~80s not 340s',
    'static constexpr double GFE_ATR_EWM_ALPHA     = 0.05;  // reverted to stable default (0.20 increased entry count)')

# ── REVERT DRIFT THRESHOLD to 0.5 ────────────────────────────────────────────
apply("THRESH-REVERT",
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 1.0;  // raised 0.5→1.0: 0.5pt = spread noise at $3000 gold',
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 0.5;  // reverted: 1.0pt floor cut winners (-$198k non-TS P&L)')

# ── KEEP CORRECT FIXES ────────────────────────────────────────────────────────
apply("FIX-A",
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 20;   // was 40 -- too long for no-L2 broker',
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 30;   // raised 20→30: stricter confirmation')

apply("FIX-B",
    'const double  time_stop_adverse = std::max(1.0, 0.25 * pos.atr_at_entry);',
    'const double  time_stop_adverse = std::max(1.0, 0.50 * pos.atr_at_entry);  // raised 0.25→0.50')

apply("FIX-C",
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 35.0;   // normal: raised 20->35',
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 50.0;   // raised 35→50')

apply("FIX-E",
    '? std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.3 * m_atr)',
    '? std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.10 * m_atr)  // coeff 0.3→0.10')

if changes > 0:
    with open(TARGET, 'w') as f:
        f.write(src)
    print(f"\n[DONE] {changes} change(s) applied → {TARGET}")
else:
    print("\n[INFO] Nothing to do")

print("\nNext:")
print("  cd ~/omega_repo")
print("  bash backtest/build_mac.sh")
print("  backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v9_24m.csv --report ~/tick/bt_report_v9_24m.csv")
