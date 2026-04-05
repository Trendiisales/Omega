#!/usr/bin/env python3
"""
fix_drift_coeff.py — direct line replacement using line number
Run from: ~/omega_repo/  →  python3 fix_drift_coeff.py
"""
import os, shutil
from datetime import datetime

TARGET = os.path.expanduser("~/omega_repo/include/GoldFlowEngine.hpp")
stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
shutil.copy2(TARGET, TARGET + f".bak_{stamp}")
print(f"[OK] Backup created")

with open(TARGET, 'r') as f:
    lines = f.readlines()

# Find and replace the exact line containing the drift coeff
target_fragment = 'std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.20 * m_atr)'
replacement = '                ? std::max(GFE_DRIFT_FALLBACK_THRESHOLD, 0.35 * m_atr)  // coeff raised 0.20→0.35: ATR=6pt→2.1pt, ATR=10pt→3.5pt, cuts 2025 overtrade\n'

found = False
for i, line in enumerate(lines):
    if target_fragment in line:
        print(f"[FOUND] Line {i+1}: {line.rstrip()}")
        lines[i] = replacement
        print(f"[REPLACED] with: {replacement.rstrip()}")
        found = True
        break

if not found:
    print("[ERROR] Target line not found. Current matching lines:")
    for i, line in enumerate(lines):
        if 'm_atr' in line and 'max(GFE_DRIFT' in line:
            print(f"  Line {i+1}: {line.rstrip()}")
    exit(1)

with open(TARGET, 'w') as f:
    f.writelines(lines)

print("\n[DONE] Written successfully")
print("\nNext:")
print("  bash backtest/build_mac.sh")
print("  backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v12_24m.csv --report ~/tick/bt_report_v12_24m.csv")
