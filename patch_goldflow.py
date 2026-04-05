#!/usr/bin/env python3
"""
patch_goldflow.py -- applies 3 calibration fixes to GoldFlowEngine.hpp
Run from Mac: python3 patch_goldflow.py

Fixes:
  [FIX-A] GFE_DRIFT_PERSIST_TICKS  20 → 30  (stricter entry confirmation)
  [FIX-B] TIME_STOP multiplier     0.25 → 0.50 (threshold fires later on real moves)
  [FIX-C] STEP1_DOLLAR_TRIGGER     35.0 → 50.0 (don't bank too early, let trade breathe)

Rationale:
  ATR at entry is ~5.5pt (from backtest MAE analysis).
  TIME_STOP at 0.25*5.5 = 1.375pt adverse fires within spread noise on $3000+ gold.
  At 0.50*5.5 = 2.75pt it requires a genuine directional failure before bailing.

  STEP1 at $35 fires at 35/(0.33*100) = 1.06pt -- banked before the trade has moved
  anywhere meaningful. At $50 it fires at 1.52pt, giving the trail room to arm.

  DRIFT_PERSIST_TICKS 20→30: requires 21/30 ticks directional (70%) over a 3s window
  instead of 14/20. Reduces noise entries especially in OVERLAP hours (12-16 UTC).
"""

import re
import sys
import os
import shutil
from datetime import datetime

TARGET = os.path.expanduser("~/omega_repo/include/GoldFlowEngine.hpp")

if not os.path.exists(TARGET):
    print(f"[ERROR] File not found: {TARGET}")
    sys.exit(1)

# Archive first
stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
backup = TARGET + f".bak_{stamp}"
shutil.copy2(TARGET, backup)
print(f"[OK] Backup: {backup}")

with open(TARGET, 'r') as f:
    src = f.read()

changes = 0

# ── FIX-A: GFE_DRIFT_PERSIST_TICKS 20 → 30 ──────────────────────────────────
OLD_A = 'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 20;   // was 40 -- too long for no-L2 broker'
NEW_A = 'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 30;   // raised 20→30: 70% of 30 ticks = stricter entry confirmation, reduces OVERLAP noise entries'

if OLD_A in src:
    src = src.replace(OLD_A, NEW_A, 1)
    print("[FIX-A] GFE_DRIFT_PERSIST_TICKS: 20 → 30  ✓")
    changes += 1
else:
    # Try without the trailing comment (in case file was edited)
    OLD_A2 = 'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 20;'
    if OLD_A2 in src:
        src = src.replace(OLD_A2,
            'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 30;   // raised 20→30: stricter entry confirmation',
            1)
        print("[FIX-A] GFE_DRIFT_PERSIST_TICKS: 20 → 30  ✓ (comment-free match)")
        changes += 1
    else:
        print("[WARN-A] GFE_DRIFT_PERSIST_TICKS pattern not found -- check manually")

# ── FIX-B: TIME_STOP multiplier 0.25 → 0.50 ─────────────────────────────────
OLD_B = 'const double  time_stop_adverse = std::max(1.0, 0.25 * pos.atr_at_entry);'
NEW_B = ('const double  time_stop_adverse = std::max(1.0, 0.50 * pos.atr_at_entry);'
         '  // raised 0.25→0.50: at ATR=5.5pt old threshold=1.375pt = spread noise at $3000 gold')

if OLD_B in src:
    src = src.replace(OLD_B, NEW_B, 1)
    print("[FIX-B] TIME_STOP multiplier: 0.25 → 0.50  ✓")
    changes += 1
else:
    print("[WARN-B] TIME_STOP multiplier pattern not found -- check manually")

# ── FIX-C: STEP1_DOLLAR_TRIGGER 35.0 → 50.0 ─────────────────────────────────
OLD_C = 'static constexpr double STEP1_DOLLAR_TRIGGER         = 35.0;   // normal: raised 20->35'
NEW_C = 'static constexpr double STEP1_DOLLAR_TRIGGER         = 50.0;   // raised 35→50: at 0.33 lots $35 fired at 1.06pt (too early), $50 fires at 1.52pt'

if OLD_C in src:
    src = src.replace(OLD_C, NEW_C, 1)
    print("[FIX-C] STEP1_DOLLAR_TRIGGER: 35.0 → 50.0  ✓")
    changes += 1
else:
    OLD_C2 = 'static constexpr double STEP1_DOLLAR_TRIGGER         = 35.0;'
    if OLD_C2 in src:
        src = src.replace(OLD_C2,
            'static constexpr double STEP1_DOLLAR_TRIGGER         = 50.0;   // raised 35→50: fires at 1.52pt at 0.33 lots',
            1)
        print("[FIX-C] STEP1_DOLLAR_TRIGGER: 35.0 → 50.0  ✓ (comment-free match)")
        changes += 1
    else:
        print("[WARN-C] STEP1_DOLLAR_TRIGGER pattern not found -- check manually")

# ── Write ─────────────────────────────────────────────────────────────────────
if changes == 0:
    print("\n[ERROR] No changes applied. File may have already been patched or patterns changed.")
    sys.exit(1)

with open(TARGET, 'w') as f:
    f.write(src)

print(f"\n[DONE] {changes}/3 fixes applied to {TARGET}")
print("Next: bash backtest/build_mac.sh && backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v5_24m.csv --report ~/tick/bt_report_v5_24m.csv")
