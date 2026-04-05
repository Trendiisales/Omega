#!/usr/bin/env python3
"""
patch_goldflow_v4.py — two critical fixes for v8 backtest
Run from: ~/omega_repo/  →  python3 patch_goldflow_v4.py

DIAGNOSIS from v7 data:
  - Engine ATR still 2.9pt median (should be 8-12pt)
  - GFE_ATR_EWM_ALPHA=0.05: takes 340s to converge from 2pt floor to real 8pt ATR
    Most of each session trades at ATR=2pt floor → lots=0.30, ts_thresh=1pt
  - Non-TS P&L: v4=+$198k, v7=-$18k → FIX-D (threshold 0.5→1.0) cut winners
  - Entry count: 25,800 (v7) vs 96,435 (v4) — 73% reduction cuts good trades

FIX-F: GFE_ATR_EWM_ALPHA 0.05 → 0.20
  At 0.20: converges from 2pt to 8pt in ~80s (was 340s at 0.05)
  Within 2 min of session start, ATR reflects real volatility
  Directly fixes: lot sizing, SL distance, TIME_STOP threshold, drift threshold

FIX-G: GFE_DRIFT_FALLBACK_THRESHOLD revert 1.0 → 0.5
  FIX-D raised this from 0.5→1.0 expecting it would cut noise
  Reality: non-TS P&L went from +$198k to -$18k — it cut winners
  The ATR-proportional term (0.10*atr) handles meaningful scaling
  At correct ATR=8pt: eff_threshold = max(0.5, 0.10*8) = 0.8pt (better than 1.0pt floor)
  At ATR=20pt: max(0.5, 0.10*20) = 2.0pt (scales correctly for high vol)
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
        print(f"[WARN-{label}] Pattern not found — check manually")

# ── FIX-F: GFE_ATR_EWM_ALPHA 0.05 → 0.20 ────────────────────────────────────
apply("FIX-F",
    'static constexpr double GFE_ATR_EWM_ALPHA     = 0.05;  // EWM smoothing alpha for ATR (20-tick equivalent half-life)',
    'static constexpr double GFE_ATR_EWM_ALPHA     = 0.20;  // raised 0.05→0.20: converges from 2pt floor to real 8pt ATR in ~80s not 340s')

# ── FIX-G: GFE_DRIFT_FALLBACK_THRESHOLD 1.0 → 0.5 (revert FIX-D) ────────────
# FIX-D changed 0.5→1.0 but non-TS P&L went from +$198k to -$18k
# The 1.0pt floor is cutting winners. Revert to 0.5, let ATR-proportional term scale.
apply("FIX-G",
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 1.0;  // raised 0.5→1.0: 0.5pt = spread noise at $3000 gold',
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 0.5;  // reverted 1.0→0.5: 1.0pt floor cut winners (non-TS P&L: +$198k→-$18k)')

# ── Keep all prior fixes idempotent ───────────────────────────────────────────
apply("FIX-A",
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 20;   // was 40 -- too long for no-L2 broker',
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 30;   // raised 20→30')

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
    print(f"\n[DONE] {changes} fix(es) applied → {TARGET}")
else:
    print("\n[INFO] Nothing to do")

print("\nNext:")
print("  cd ~/omega_repo")
print("  bash backtest/build_mac.sh")
print("  backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v8_24m.csv --report ~/tick/bt_report_v8_24m.csv")
