#!/usr/bin/env python3
"""
fix_to_v11_plus_trail.py — revert drift coeff, fix payoff ratio via trail width
Run from: ~/omega_repo/  →  python3 fix_to_v11_plus_trail.py

DIAGNOSIS:
  Drift coeff tuning is the wrong lever:
  - v11 (0.20): 16,321 trades, TS=24.3%, non-TS avg=$8.08
  - v12 (0.35):  2,790 trades, TS=20.1%, non-TS avg=$2.16
  TS rate barely moved (-4%) but good trades cut 83%. Wrong tool.

  The ACTUAL problem: payoff ratio = 0.292 (need ~0.44 to break even at WR=69%)
  Trail exits at 2.15pt avg, MFE=$31 ($310/lot). Capturing only 7% of MFE.
  Trail is exiting way too early.

  WHY TRAIL IS TOO TIGHT:
  In GoldFlowEngine manage_position() after step1+step2:
    trail_mult = 0.25 (step2 done, move < 3x ATR)
    trail_sl = entry + mfe - atr_live * 0.25
    At ATR=6pt, trail = mfe - 1.5pt behind peak
    Winners with MFE=5pt exit at 3.5pt. Avg capture = 2.15pt ✓ (matches data)

  FIX: raise trail multipliers at stage2+ from 0.25→0.40 and final from 0.20→0.30
  This gives the trail 2.4pt of breathing room instead of 1.5pt.
  Winners with MFE=5pt now exit at 2.6pt (vs 3.5pt). Slightly less captured.
  BUT: winners with MFE=10pt exit at 7.6pt (vs 8.5pt). Better for large moves.
  AND: fewer premature trail hits on normal 1-2pt retracements.

  The real gain: PARTIAL_2R fires at $30 (2.26pt) → SL locks to step1 exit → 
  remainder runs with wide trail. Currently most trail hits are at stage1 only.

  ALSO REVERT: drift coeff 0.35→0.20

Targets GFE_TRAIL_STAGE3_MULT and GFE_TRAIL_STAGE4_MULT constants
AND the inline trail_mult values in manage_position().
"""

import os, shutil
from datetime import datetime

TARGET = os.path.expanduser("~/omega_repo/include/GoldFlowEngine.hpp")
stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
shutil.copy2(TARGET, TARGET + f".bak_{stamp}")
print(f"[OK] Backup created")

with open(TARGET, 'r') as f:
    lines = f.readlines()

changes = 0

def replace_line(fragment, new_line, label):
    global changes
    for i, line in enumerate(lines):
        if fragment in line:
            print(f"[{label}] Line {i+1}: {line.rstrip()}")
            lines[i] = new_line + '\n'
            print(f"        → {new_line.rstrip()}")
            changes += 1
            return True
    print(f"[WARN-{label}] Not found: {fragment}")
    return False

# ── Revert drift coeff 0.35 → 0.20 ───────────────────────────────────────────
replace_line(
    'std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.35 * m_atr)',
    '                ? std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.20 * m_atr)  // coeff 0.20: at ATR=6pt=1.7pt, ATR=10pt=2.0pt',
    'COEFF-REVERT'
)

# ── Widen stage2 trail: 0.25 → 0.40 ─────────────────────────────────────────
# In manage_position() normal trail section:
#   (!pos.partial_closed_2)                 ? 0.50 :  // step1 done, running free
#   (move < atr * 3.0)                      ? 0.25 :  // step2 done, mid tighten
#                                             0.20;   // final remainder squeeze
# Change: 0.25→0.40, 0.20→0.30
# This gives trail more room — winners with 5-10pt MFE stop getting clipped at 2pt retrace
replace_line(
    '(move < atr * 3.0)                      ? 0.25 :  // step2 done, mid tighten',
    '                    (move < atr * 3.0)                      ? 0.40 :  // step2 done, mid tighten -- raised 0.25→0.40 for wider trail',
    'TRAIL-STAGE3'
)

replace_line(
    '                                          0.20;   // final remainder squeeze',
    '                                          0.30;   // final remainder squeeze -- raised 0.20→0.30',
    'TRAIL-STAGE4'
)

if changes > 0:
    with open(TARGET, 'w') as f:
        f.writelines(lines)
    print(f"\n[DONE] {changes} change(s) applied")
    print()
    print("Expected v13:")
    print("  Trades: ~16k (coeff back to 0.20)")
    print("  Trail wider: captures 3-4pt vs current 2.15pt")
    print("  Avg win: ~$35+ (vs $14-18 currently)")
    print("  Payoff ratio: ~0.55+ (need 0.44 to break even)")
else:
    print("\n[INFO] No changes made")

print("\nNext:")
print("  bash backtest/build_mac.sh")
print("  backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v13_24m.csv --report ~/tick/bt_report_v13_24m.csv")
