#!/usr/bin/env python3
"""
patch_goldflow_v5.py — the actual root cause fix
Run from: ~/omega_repo/  →  python3 patch_goldflow_v5.py

ROOT CAUSE (confirmed from v7 CSV analysis):
  GFE_ATR_MIN = 2.0pt
  size = risk_dollars / (atr * tick_mult) = 80 / (2.0 * 100) = 0.40 lots at floor
  
  ATR EWM is slow → engine spends most of session at or near 2pt floor
  Result: 0.30-0.40 lots traded → each TIME_STOP loses $40-44 regardless
  
  KEY INSIGHT: TS loss = 0.5 * atr * size * 100 = 0.5 * atr * (80/(atr*100)) * 100 = $40
  It's ALWAYS ~$40 regardless of ATR — set purely by risk_dollars.
  No ATR-alpha tuning can fix this. Only fewer fires OR lower risk_dollars.

  RAISING GFE_ATR_MIN from 2.0 to 6.0:
    - At floor (atr=6pt): lots=0.133, ts_thresh=max(1,0.5*6)=3pt (not 1pt noise)
    - At London (atr=10pt): lots=0.080, ts_thresh=5pt  
    - TS now fires only on 3-5pt adverse → genuine failures, not spread noise
    - Asia real ATR=2-4pt → floor kicks in → ts_thresh=3pt (wider, correct for gaps)
    - TS loss still ~$40 but fires FAR LESS OFTEN (3pt threshold vs 1pt)

FIX-H: GFE_ATR_MIN 2.0 → 6.0
FIX-I: GFE_ATR_EWM_ALPHA revert 0.20 → 0.05 (0.20 caused MORE entries, not fewer)

All prior fixes kept idempotent.
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
        print(f"[WARN-{label}] Pattern not found — check file manually")

# ── FIX-H: GFE_ATR_MIN 2.0 → 6.0 ────────────────────────────────────────────
# This is the ROOT CAUSE fix. All other fixes addressed symptoms.
# At 2.0pt floor: size=0.40lots, ts_thresh=1pt (noise) → 25k TS fires/year
# At 6.0pt floor: size=0.133lots, ts_thresh=3pt (real move) → far fewer TS fires
# Asia real ATR=2-4pt will hit this floor → correct: wider SL for gappy sessions
apply("FIX-H",
    'static constexpr double GFE_ATR_MIN           = 2.0;   // lowered 5.0->2.0: 5.0 floor was pinning ATR at exactly',
    'static constexpr double GFE_ATR_MIN           = 6.0;   // raised 2.0→6.0: 2.0pt floor = 0.40lots oversized, ts_thresh=1pt fires on noise')

# ── FIX-I: GFE_ATR_EWM_ALPHA revert 0.20 → 0.05 ─────────────────────────────
# 0.20 caused MORE entries (faster warmup = fires sooner each session)
# The ATR_MIN fix makes the alpha less critical -- revert to stable default
apply("FIX-I",
    'static constexpr double GFE_ATR_EWM_ALPHA     = 0.20;  // raised 0.05→0.20: converges from 2pt floor to real 8pt ATR in ~80s not 340s',
    'static constexpr double GFE_ATR_EWM_ALPHA     = 0.05;  // reverted 0.20→0.05: 0.20 increased entry count, ATR_MIN fix makes alpha less critical')

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

apply("FIX-G",
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 1.0;  // raised 0.5→1.0: 0.5pt = spread noise at $3000 gold',
    'static constexpr double GFE_DRIFT_FALLBACK_THRESHOLD = 0.5;  // reverted 1.0→0.5: 1.0pt floor cut winners')

# ── Also revert ASIA_ATR_MIN which references ATR_MIN ─────────────────────────
# GFE_ASIA_ATR_MIN is a separate constant, leave it as-is (1.5pt for Asia)
# Asia real ATR of 1.5-3.5pt is fine for Asia -- only the general floor changes

if changes > 0:
    with open(TARGET, 'w') as f:
        f.write(src)
    print(f"\n[DONE] {changes} fix(es) applied → {TARGET}")
    print()
    print("Expected v9 results:")
    print("  Trades:   ~50-70k (ATR_MIN filter cuts noise entries)")
    print("  Avg loss: ~$5-8   (ts_thresh=3pt not 1pt = fewer fires)")  
    print("  Lots:     ~0.10-0.13 (sized correctly)")
    print("  P&L:      positive or near-zero if non-TS signal holds")
else:
    print("\n[INFO] Nothing to do")

print("\nNext:")
print("  cd ~/omega_repo")
print("  bash backtest/build_mac.sh")
print("  backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v9_24m.csv --report ~/tick/bt_report_v9_24m.csv")
