#!/usr/bin/env python3
"""
patch_goldflow_v3.py — fix ATR-proportional drift coefficient 0.3→0.10
Run from: ~/omega_repo/  →  python3 patch_goldflow_v3.py

Problem:
  GoldFlowEngine.hpp drift gate (L2-unavailable path):
    eff_drift_threshold = max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.3 * m_atr)

  With correct ATR=10pt:  max(1.0, 0.3*10) = 3.0pt → 73% of entries blocked
  With correct ATR=20pt:  max(1.0, 0.3*20) = 6.0pt → almost no entries

  0.3 was calibrated when ATR was stuck at 2-3pt (broken tick-EWM).
  max(1.0, 0.3*2.4) = 1.0pt — accidentally reasonable at wrong ATR.
  Now ATR is correct, coefficient must drop proportionally.

Fix:
  0.3 → 0.10
  At ATR=10pt: max(1.0, 0.10*10) = 1.0pt  ← same effective threshold as before
  At ATR=20pt: max(1.0, 0.10*20) = 2.0pt  ← scales correctly for high-vol sessions
  At ATR=3pt:  max(1.0, 0.10*3)  = 1.0pt  ← floor holds for low-vol Asia

Also keeps FIX-A/B/C/D from v5 patch (idempotent).
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
        print(f"[{label}] Already applied, skipping")
    else:
        print(f"[WARN-{label}] Pattern not found")

# ── FIX-E: ATR-proportional drift coefficient 0.3 → 0.10 ─────────────────────
apply("FIX-E",
    '? std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.3 * m_atr)',
    '? std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.10 * m_atr)  // coeff 0.3→0.10: 0.3 calibrated at broken ATR=2.4pt, now ATR correct at 8-12pt')

# ── Keep prior fixes idempotent ────────────────────────────────────────────────
apply("FIX-A",
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 20;   // was 40 -- too long for no-L2 broker',
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 30;   // raised 20→30')

apply("FIX-B",
    'const double  time_stop_adverse = std::max(1.0, 0.25 * pos.atr_at_entry);',
    'const double  time_stop_adverse = std::max(1.0, 0.50 * pos.atr_at_entry);  // raised 0.25→0.50')

apply("FIX-C",
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 35.0;   // normal: raised 20->35',
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 50.0;   // raised 35→50')

apply("FIX-D",
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 0.5;  // was 1.5 -- too strict for no-L2 broker',
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 1.0;  // raised 0.5→1.0')

if changes > 0:
    with open(TARGET, 'w') as f:
        f.write(src)
    print(f"\n[DONE] {changes} fix(es) applied → {TARGET}")
else:
    print("\n[INFO] Nothing to do — all fixes already applied")

print("\nNext:")
print("  cd ~/omega_repo")
print("  bash backtest/build_mac.sh")
print("  backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v7_24m.csv --report ~/tick/bt_report_v7_24m.csv")
