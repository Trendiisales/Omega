#!/usr/bin/env python3
"""
fix_staircase.py — fix the staircase/trail interaction
Run from: ~/omega_repo/  →  python3 fix_staircase.py

ROOT CAUSE (confirmed from v13 data):
  Stage1 trail_mult = 0.50 → trail_dist = 3.0pt at ATR=6pt
  STEP1 fires at 2.26pt → SL-at-BE = entry + ~2.635pt
  Trail only beats BE-SL when MFE > 5.635pt ($50)
  Only 15% of trades have MFE > $50
  → 85% of 'TRAIL_HIT' are actually BE-SL exits at ~0.5pt gain = $4.90
  → 48% of trail exits are 0-1pt gain: engine is essentially a BE machine

  Position P&L: P1R($9.51) + BE_trail($4.90) = $14.41 avg on winners
  TS losses: -$45.81 avg
  0.70 × $14.41 - 0.30 × $48 = $10.09 - $14.40 = -$4.31/trade ← matches data

FIX-M: Stage1 trail_mult 0.50 → 0.18
  trail_dist = 0.18 * 6 = 1.08pt
  Trail beats BE-SL when MFE > 1.08 + 2.635 = 3.715pt ($33)
  ~40% of trades have MFE > $33 → real trail captures
  Modelled delta: +$51k

FIX-N: STEP1_DOLLAR_TRIGGER $30 → $20  
  P1R fires at 1.50pt (was 2.26pt)
  788 TS trades with MFE $13-20 now bank P1R then exit at BE
  Saves ~$31k vs current TS losses on these trades
  Lower P1R value: -$16k (P1R banks $6.60 not $9.51)
  Net STEP1 change: +$15k

Combined modelled improvement: ~+$66k → estimated P&L: ~-$15k to breakeven
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
    print(f"[WARN-{label}] Not found: {fragment!r}")
    return False

# FIX-M: Stage1 trail_mult 0.50 → 0.18
# This is the line: (!partial_closed_2) ? 0.50 : (step1 done, running free)
replace_line(
    '(!pos.partial_closed_2)                 ? 0.50 :  // step1 done, running free',
    '                    (!pos.partial_closed_2)                 ? 0.18 :  // step1 done, trail tight: 0.18*6=1.08pt lets trail beat BE-SL at MFE>3.7pt',
    'FIX-M'
)

# FIX-N: STEP1_DOLLAR_TRIGGER $30 → $20
replace_line(
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 30.0;   // lowered 50→30: at 0.133lots fires at 2.26pt',
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 20.0;   // lowered 30→20: at 0.133lots fires at 1.50pt, saves 788 TS trades/year',
    'FIX-N'
)

if changes > 0:
    with open(TARGET, 'w') as f:
        f.writelines(lines)
    print(f"\n[DONE] {changes} change(s) applied")
else:
    print("\n[WARN] No changes applied")

print("\nNext:")
print("  bash backtest/build_mac.sh")
print("  backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v14_24m.csv --report ~/tick/bt_report_v14_24m.csv")
