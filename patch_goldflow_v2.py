#!/usr/bin/env python3
"""
patch_goldflow_v2.py -- GoldFlowEngine.hpp calibration fixes for v6 backtest
Run from: ~/omega_repo/  →  python3 patch_goldflow_v2.py

Applies only fixes not yet in the file (idempotent -- safe to run multiple times).

Fixes:
  [FIX-A] GFE_DRIFT_PERSIST_TICKS  20 → 30  (already applied in v5 patch)
  [FIX-B] TIME_STOP multiplier   0.25 → 0.50 (already applied in v5 patch)
  [FIX-C] STEP1_DOLLAR_TRIGGER  35.0 → 50.0  (already applied in v5 patch)
  [FIX-D] GFE_DRIFT_FALLBACK_THRESHOLD 0.5 → 1.0  (NEW)
           At $3000 gold a 0.5pt drift = 0.017% of price = spread noise.
           1.0pt still triggers on real directional moves but kills noise entries.
           Combined with correct ATR (8-12pt): drift_threshold = max(1.0, 0.3*10) = 3.0pt
           Was: max(0.5, 0.3*2.4) = 0.72pt (noise level)
"""

import sys
import os
import shutil
from datetime import datetime

TARGET = os.path.expanduser("~/omega_repo/include/GoldFlowEngine.hpp")

if not os.path.exists(TARGET):
    print(f"[ERROR] File not found: {TARGET}")
    sys.exit(1)

stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
backup = TARGET + f".bak_{stamp}"
shutil.copy2(TARGET, backup)
print(f"[OK] Backup: {backup}")

with open(TARGET, 'r') as f:
    src = f.read()

changes = 0
skipped = 0

def apply(label, old, new, src):
    if old in src:
        print(f"[{label}] Applied ✓")
        return src.replace(old, new, 1), True
    # Check if new value already present (idempotent)
    if new in src:
        print(f"[{label}] Already applied, skipping")
        return src, False
    print(f"[WARN-{label}] Pattern not found -- check manually")
    return src, False

# FIX-A
src, ok = apply("FIX-A",
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 20;   // was 40 -- too long for no-L2 broker',
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 30;   // raised 20→30: stricter entry confirmation',
    src)
if ok: changes += 1
else: skipped += 1

# FIX-B
src, ok = apply("FIX-B",
    'const double  time_stop_adverse = std::max(1.0, 0.25 * pos.atr_at_entry);',
    'const double  time_stop_adverse = std::max(1.0, 0.50 * pos.atr_at_entry);  // raised 0.25→0.50',
    src)
if ok: changes += 1
else: skipped += 1

# FIX-C
src, ok = apply("FIX-C",
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 35.0;   // normal: raised 20->35',
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 50.0;   // raised 35→50',
    src)
if ok: changes += 1
else: skipped += 1

# FIX-D: GFE_DRIFT_FALLBACK_THRESHOLD 0.5 → 1.0
# This is the absolute floor for the ATR-proportional drift threshold.
# Was 0.5pt: at ATR=2.4pt → threshold=max(0.5,0.72)=0.72pt (noise at $3000 gold)
# Now 1.0pt: at correct ATR=10pt → threshold=max(1.0,3.0)=3.0pt (meaningful)
#            at Asia ATR=2pt     → threshold=max(1.0,0.6)=1.0pt (still catches slow grinds)
src, ok = apply("FIX-D",
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 0.5;  // was 1.5 -- too strict for no-L2 broker',
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 1.0;  // raised 0.5→1.0: 0.5pt = spread noise at $3000 gold',
    src)
if ok: changes += 1
else: skipped += 1

if changes == 0 and skipped > 0:
    print(f"\n[INFO] All {skipped} fixes already applied. No changes needed.")
elif changes == 0:
    print(f"\n[ERROR] No patterns matched. File may have diverged.")
    sys.exit(1)
else:
    with open(TARGET, 'w') as f:
        f.write(src)
    print(f"\n[DONE] {changes} new fixes applied, {skipped} already present → {TARGET}")

print("Next:")
print("  cd ~/omega_repo")
print("  bash backtest/build_mac.sh")
print("  backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v6_24m.csv --report ~/tick/bt_report_v6_24m.csv")
