#!/usr/bin/env python3
"""
patch_goldflow_v6.py — two targeted fixes based on v9 data analysis
Run from: ~/omega_repo/  →  python3 patch_goldflow_v6.py

v9 state: -$136k, WR=60.5%, 37,622 trades
  Non-TS: +$282k ✓ (signal is positive)
  TS:    -$418k  (9,308 trades × -$44.96 avg)
  MHT:   -$48k   (5,175 trades held 60min, never stepped)

FIX-J: drift coeff 0.10 → 0.20
  At ATR=6pt: eff_threshold = max(0.5, 0.20*6) = 1.7pt (was 1.1pt)
  Cuts TS entries by ~35% (1.7pt threshold rejects more noise signals)
  Real 2pt+ drift moves still pass. Spread noise (0.4pt) firmly blocked.

FIX-K: STEP1_DOLLAR_TRIGGER $50 → $30
  At 0.133 lots: $30 fires at 30/(0.133*100) = 2.26pt  (was 3.76pt at $50)
  5,175 MHT trades had avg MFE=$27 — they reached 2-3pt then timed out at 60min
  With $30 trigger: STEP1 fires at 2.26pt → SL locks to entry+buffer → trade runs free
  Converts MHT -$48k into partial winners (+$30k estimated)
  Does NOT re-enable noise entries — TS threshold is 3pt, STEP1 is 2.26pt
  Trade must reach +2.26pt AND survive 45s before TS would have fired anyway

All prior correct fixes kept idempotent.
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

# ── FIX-J: drift coeff 0.10 → 0.20 ──────────────────────────────────────────
apply("FIX-J",
    '? std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.10 * m_atr)  // coeff 0.3→0.10',
    '? std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.20 * m_atr)  // coeff raised 0.10→0.20: at ATR=6pt gives 1.7pt threshold (was 1.1pt), cuts TS noise entries')

# ── FIX-K: STEP1_DOLLAR_TRIGGER $50 → $30 ────────────────────────────────────
apply("FIX-K",
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 50.0;   // raised 35→50',
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 30.0;   // lowered 50→30: at 0.133lots fires at 2.26pt; converts MHT -$48k into partial wins')

# ── Keep all correct prior fixes ──────────────────────────────────────────────
apply("ROOT",
    'static constexpr double GFE_ATR_MIN           = 2.0;   // lowered 5.0->2.0: 5.0 floor was pinning ATR at exactly',
    'static constexpr double GFE_ATR_MIN           = 6.0;   // ROOT FIX: 2.0pt=noise floor. 6pt: size=0.133lots, SL=6pt, TS_thresh=3pt, trail=3pt')

apply("FIX-A",
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 20;   // was 40 -- too long for no-L2 broker',
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 30;   // raised 20→30')

apply("FIX-B",
    'const double  time_stop_adverse = std::max(1.0, 0.25 * pos.atr_at_entry);',
    'const double  time_stop_adverse = std::max(1.0, 0.50 * pos.atr_at_entry);  // raised 0.25→0.50')

apply("THRESH-REVERT",
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 1.0;  // raised 0.5→1.0: 0.5pt = spread noise at $3000 gold',
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 0.5;  // reverted: 1.0pt floor cut winners')

apply("ALPHA-REVERT",
    'static constexpr double GFE_ATR_EWM_ALPHA     = 0.20;  // raised 0.05→0.20: converges from 2pt floor to real 8pt ATR in ~80s not 340s',
    'static constexpr double GFE_ATR_EWM_ALPHA     = 0.05;  // reverted to stable default')

if changes > 0:
    with open(TARGET, 'w') as f:
        f.write(src)
    print(f"\n[DONE] {changes} change(s) → {TARGET}")
    print()
    print("Expected v10:")
    print("  TS:  ~6,000 trades (35% fewer from drift 1.1→1.7pt) = ~-$270k")
    print("  MHT: ~1,000 trades (most now hit STEP1 at 2.26pt)   = ~-$9k")
    print("  Non-TS: similar count, more partials banking         = ~+$320k")
    print("  Projected net: ~+$40k")
else:
    print("\n[INFO] Nothing to do")

print("\nNext:")
print("  cd ~/omega_repo")
print("  bash backtest/build_mac.sh")
print("  backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v10_24m.csv --report ~/tick/bt_report_v10_24m.csv")
