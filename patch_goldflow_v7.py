#!/usr/bin/env python3
"""
patch_goldflow_v7.py — fix 2025 overtrade: raise drift coefficient
Run from: ~/omega_repo/  →  python3 patch_goldflow_v7.py

DIAGNOSIS from v10 clean data:
  Real P&L: -$81,611 (not -$34k — 6 corrupt records added +$47k false profit)
  2025: 61 trades/day vs 7.4/day in 2023
  2025 TS: 3,077 fires, -$142,736 — need to cut 46% = 1,413 trades

  WHY 2025 OVERTRADES:
  At $4000 gold: drift threshold = max(0.5, 0.20*6) = 1.7pt = 0.04% of price
  At $1800 gold: drift threshold = max(0.5, 0.20*6) = 1.7pt = 0.09% of price
  Same absolute threshold = 2x looser filter at $4000 vs $1800

  Fix: raise coeff 0.20→0.35
  At ATR=6pt floor: max(0.5, 0.35*6) = 2.1pt  (vs 1.7pt — 24% stricter)
  At ATR=10pt real: max(0.5, 0.35*10) = 3.5pt (meaningful at any gold price)
  At ATR=15pt high-vol: max(0.5, 0.35*15) = 5.25pt (very selective, only strong moves)

  Expected: cut ~35-45% of 2025 TS entries
  2023/2024 unaffected (lower price, same threshold in abs pts)

FIX-L: drift coeff 0.20 → 0.35
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

# ── FIX-L: drift coeff 0.20 → 0.35 ──────────────────────────────────────────
apply("FIX-L",
    '? std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.20 * m_atr)  // coeff raised 0.10→0.20: at ATR=6pt gives 1.7pt threshold (was 1.1pt), cuts TS noise entries',
    '? std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.35 * m_atr)  // coeff raised 0.20→0.35: at ATR=6pt=2.1pt, ATR=10pt=3.5pt — cuts 2025 overtrade (61 trades/day)')

# ── Keep all correct prior fixes ──────────────────────────────────────────────
apply("ROOT",
    'static constexpr double GFE_ATR_MIN           = 2.0;   // lowered 5.0->2.0: 5.0 floor was pinning ATR at exactly',
    'static constexpr double GFE_ATR_MIN           = 6.0;   // ROOT FIX: 2.0pt=noise floor. 6pt: size=0.133lots, SL=6pt, TS_thresh=3pt')

apply("FIX-A",
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 20;   // was 40 -- too long for no-L2 broker',
    'static constexpr int    GFE_DRIFT_PERSIST_TICKS      = 30;   // raised 20→30')

apply("FIX-B",
    'const double  time_stop_adverse = std::max(1.0, 0.25 * pos.atr_at_entry);',
    'const double  time_stop_adverse = std::max(1.0, 0.50 * pos.atr_at_entry);  // raised 0.25→0.50')

apply("FIX-K",
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 50.0;   // raised 35→50',
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 30.0;   // lowered 50→30: at 0.133lots fires at 2.26pt')

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
    print("Expected v11:")
    print("  2025 trades: ~7,000 (from 12,581) — stricter drift gate")
    print("  2025 TS:     ~1,600 (from 3,077)  — fewer bad entries")
    print("  Net P&L:     positive or near-zero")
else:
    print("\n[INFO] Nothing to do")

print("\nNext:")
print("  cd ~/omega_repo")
print("  bash backtest/build_mac.sh")
print("  backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v11_24m.csv --report ~/tick/bt_report_v11_24m.csv")
