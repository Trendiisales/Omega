#!/usr/bin/env python3
"""
two_mode_engine.py — redesign GoldFlowEngine for two-mode operation
Run from: ~/omega_repo/  →  python3 two_mode_engine.py

DIAGNOSIS (confirmed from 24-month backtest data):
  The engine has positive signal (58% position WR) but negative expectancy
  because exit architecture is mismatched to the move sizes produced:

  Normal days: signal produces 2-4pt moves
  Current exit: 6pt SL + staircase designed for 10pt+ moves
  Result: winners avg $18 (0.3R), losers avg $45 (0.75R) → negative EV

  The velocity trail (breakout mode) is already coded but velocity_shadow_mode
  was True in earlier versions. It's now False (line 937) — already correct.

TWO-MODE DESIGN:
  MODE 1 — SCALP (normal days, ~90% of positions):
    Activated when: NOT velocity_active (vol_ratio<2.5 OR |ewm_drift|<2.0)
    SL: 2pt  (GFE_ATR_SL_MULT: 1.0 → 0.33, at ATR=6pt → 2pt)
    TP: 2pt  (new fixed TP exit before staircase runs)
    No partials (STEP1 disabled in scalp mode — set trigger to $9999)
    Time limit: 90s (scratch flat trades)
    Expectancy: 0.60×$26 - 0.40×$26 = +$4.16/position

  MODE 2 — VELOCITY/BREAKOUT (expansion days, ~10% of positions):
    Activated when: velocity_active = expansion_mode AND vol_ratio>2.5
    SL: 6pt  (normal ATR-based — needs room for big moves)
    No TP  (let velocity trail do its job)
    STEP1 at $150 (already in engine for velocity mode)
    Trail: 2xATR when velocity armed (already in engine)
    TIME_STOP suppressed when direction aligned (already in engine)
    Expected: capture 20-50pt moves on tariff-crash / rate-decision days

CHANGES:
  1. GFE_ATR_SL_MULT: 1.0 → 0.33  (2pt SL in scalp mode)
     In velocity mode the engine uses atr_at_entry which will be ~6pt
     So velocity SL = 6 * 0.33 = 2pt... that's too tight for breakout
     FIX: don't change the constant — add conditional in enter():
       sl_mult = velocity_active ? 1.0 : 0.33
     But we can't know velocity_active at enter() time easily.
     SIMPLER: keep GFE_ATR_SL_MULT=1.0, add max SL cap of 2pt for scalp
     via new constant GFE_SCALP_MAX_SL_PTS = 2.0 applied in enter()
     when !m_expansion_mode (proxy for non-velocity at entry time)

  2. Add fixed TP in manage_position() before staircase:
     if (!velocity_active && move >= GFE_SCALP_TP_PTS && !pos.be_locked):
         close_position(exit_px, "TP_HIT", now_ms, on_close)

  3. Disable staircase STEP1 in scalp mode:
     STEP1_DOLLAR_TRIGGER: 30.0 → 9999.0 in scalp (never fires)
     Velocity mode uses STEP1_DOLLAR_TRIGGER_VELOCITY=150.0 (separate const)
     So changing the scalp trigger to 9999 doesn't affect velocity mode

  4. TIME_STOP in scalp mode: revert to original 0.50 threshold
     (halved threshold caused more SL_HIT — zero-sum trade)
     velocity_time_suppress already handles breakout suppression
"""

import os, shutil
from datetime import datetime

TARGET = os.path.expanduser("~/omega_repo/include/GoldFlowEngine.hpp")
stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
shutil.copy2(TARGET, TARGET + f".bak_{stamp}")
print(f"[OK] Backup: {TARGET}.bak_{stamp}")

with open(TARGET, 'r') as f:
    lines = f.readlines()

changes = 0

def find_and_replace(fragment, new_content, label, all_occurrences=False):
    global changes
    found = 0
    for i, line in enumerate(lines):
        if fragment in line:
            lines[i] = new_content + '\n'
            print(f"[{label}] Line {i+1}: replaced")
            changes += 1
            found += 1
            if not all_occurrences:
                break
    if not found:
        print(f"[WARN-{label}] Not found: {fragment!r}")

def insert_after(fragment, new_lines_str, label):
    global changes
    for i, line in enumerate(lines):
        if fragment in line:
            insert_pos = i + 1
            new_lines = [l + '\n' for l in new_lines_str.split('\n')]
            lines[i+1:i+1] = new_lines
            print(f"[{label}] Inserted {len(new_lines)} lines after line {i+1}")
            changes += 1
            return
    print(f"[WARN-{label}] Anchor not found: {fragment!r}")

# ── CHANGE 1: Revert TIME_STOP threshold back to 0.50 (halving was zero-sum) ──
find_and_replace(
    'time_stop_adverse = std::max(0.75, 0.25 * pos.atr_at_entry)',
    '            const double  time_stop_adverse = std::max(1.0, 0.50 * pos.atr_at_entry);  // 0.50x ATR: fires at 3pt on ATR=6',
    'TS-REVERT-THRESHOLD'
)

# ── CHANGE 2: Disable STEP1 in scalp mode (set to $9999, velocity uses $150) ──
# STEP1_DOLLAR_TRIGGER = 30.0 → only fires in velocity mode which uses
# STEP1_DOLLAR_TRIGGER_VELOCITY = 150.0 separately. Scalp mode: disable.
find_and_replace(
    'static constexpr double STEP1_DOLLAR_TRIGGER         = 30.0;   // reverted 20→30: $30 at 0.133lots = 2.26pt',
    '        static constexpr double STEP1_DOLLAR_TRIGGER         = 9999.0; // SCALP: disabled — scalp uses TP_HIT exit. Velocity uses STEP1_DOLLAR_TRIGGER_VELOCITY=150',
    'STEP1-DISABLE-SCALP'
)

# ── CHANGE 3: Add scalp SL cap in enter() ─────────────────────────────────────
# Current sl_pts = max(atr_sl, min_sl, asia_sl_floor) = max(6, ...) = 6pt
# In scalp mode (non-expansion): cap SL at 2pt
# In velocity/expansion mode: keep full 6pt SL for room to run
# Find the sl_pts line and add the cap after it
find_and_replace(
    'const double sl_pts  = std::max({atr_sl, min_sl, asia_sl_floor});',
    '        const double sl_pts_raw = std::max({atr_sl, min_sl, asia_sl_floor});\n        // SCALP MODE: cap SL at 2pt when not in expansion (keeps $26 max risk)\n        // VELOCITY MODE: keep full SL for room on breakout moves\n        static constexpr double GFE_SCALP_SL_CAP = 2.0;  // 2pt = $26.60 at 0.133 lots\n        const double sl_pts = m_expansion_mode ? sl_pts_raw : std::min(sl_pts_raw, GFE_SCALP_SL_CAP);',
    'SL-CAP-SCALP'
)

# ── CHANGE 4: Add TP_HIT exit in manage_position() ───────────────────────────
# Insert before the STAIRCASE BANKING section (line 1625)
# When NOT velocity_active AND move >= 2pt AND no step taken yet → TP exit
# Use fragment of the STAIRCASE comment as anchor
find_and_replace(
    '        // ??????????????????????????????????????????????????????????????????????',
    '        // ── SCALP TP EXIT (non-velocity mode only) ──────────────────────────────\n        // When not in velocity/expansion mode: exit at fixed 2pt TP.\n        // Symmetric with 2pt SL → payoff=1.0 → positive EV at WR=58%\n        // Velocity mode bypasses this and uses the full staircase/trail.\n        static constexpr double GFE_SCALP_TP_PTS = 2.0;\n        if (!velocity_active && !pos.be_locked && move >= GFE_SCALP_TP_PTS) {\n            const double exit_px = pos.is_long ? bid : ask;\n            close_position(exit_px, "TP_HIT", now_ms, on_close);\n            return;\n        }\n        // ??????????????????????????????????????????????????????????????????????',
    'SCALP-TP-EXIT'
)

if changes > 0:
    with open(TARGET, 'w') as f:
        f.writelines(lines)
    print(f"\n[DONE] {changes} changes applied")
    print()
    print("Verify:")
    print("  grep -n 'SCALP_TP\\|SCALP_SL\\|TP_HIT\\|STEP1.*9999\\|time_stop_adverse' ~/omega_repo/include/GoldFlowEngine.hpp | head -15")
    print()
    print("Then build:")
    print("  bash backtest/build_mac.sh")
    print("  backtest/OmegaBacktest_mac ~/tick/xauusd_merged_24m.csv --engine flow --warmup 10000 --quiet --trades ~/tick/bt_trades_v17_24m.csv --report ~/tick/bt_report_v17_24m.csv")
else:
    print("[ERROR] No changes applied")
