#!/usr/bin/env python3
"""
macro_gold_gate.py -- daily producer for the MacroGoldGate (build #1).

Fetches FRED 10y real-yield (DFII10) + broad-dollar (DTWEXBGS), computes the
macro score, and writes the flat gate file that include/MacroGoldGate.hpp reads:

    logs/macro/macro_gold_gate.tsv
    # macro_gold_gate  score  hostile  real_yield  dollar  stamp_ms
    MACRO  -3  1  2.31  121.40  1781679000000

score = 2*sign(-d20(real_yield)) + 1*sign(-d20(dollar))   (range -3..+3)
hostile = 1 when score <= HOSTILE_THRESH (-2): real yields rising hard => de-risk
gold longs. Validated cross-regime in backtest/macro_gold_regime.py (asymmetric
bear-insurance: near-free in bull, flips the bear). FAIL-SAFE: on any fetch error
this writes NOTHING (keeps the last good file); the C++ gate self-stales after 3d.

Schedule daily (gold daily data is daily). Run AFTER ~16:30 ET when FRED updates.
"""
import sys, time, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "logs/macro/macro_gold_gate.tsv"
CACHE = ROOT / "data/macro"
SLOPE_LB = 20          # business-day slope lookback (matches the backtest)
HOSTILE_THRESH = -2    # score <= this => hostile (real yields clearly rising)

SERIES = {"ry": "DFII10", "dx": "DTWEXBGS"}

def fetch(series_id):
    url = (f"https://fred.stlouisfed.org/graph/fredgraph.csv?id={series_id}"
           f"&cosd=2024-01-01")
    req = urllib.request.Request(url, headers={"User-Agent": "omega-macro-gate/1.0"})
    with urllib.request.urlopen(req, timeout=30) as r:
        text = r.read().decode()
    vals = []
    for i, ln in enumerate(text.splitlines()):
        if i == 0:
            continue
        d, v = ln.split(",")
        if v not in ("", "."):
            vals.append(float(v))
    if len(vals) < SLOPE_LB + 1:
        raise ValueError(f"{series_id}: too few obs ({len(vals)})")
    # cache for reproducibility / offline fallback
    (CACHE / f"{series_id}.csv").write_text(text)
    return vals

def slope_sign(vals):
    # FALLING macro driver -> +1 bullish gold ; RISING -> -1
    diff = vals[-1] - vals[-1 - SLOPE_LB]
    return -1 if diff > 0 else (1 if diff < 0 else 0)

def main():
    try:
        ry = fetch(SERIES["ry"])
        dx = fetch(SERIES["dx"])
    except Exception as e:
        sys.stderr.write(f"[macro_gold_gate] FETCH FAILED ({e}) -- leaving last good file untouched\n")
        return 1
    score = 2 * slope_sign(ry) + 1 * slope_sign(dx)
    hostile = 1 if score <= HOSTILE_THRESH else 0
    stamp_ms = int(time.time() * 1000)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(
        "# macro_gold_gate  score  hostile  real_yield  dollar  stamp_ms\n"
        f"MACRO\t{score}\t{hostile}\t{ry[-1]:.2f}\t{dx[-1]:.2f}\t{stamp_ms}\n"
    )
    print(f"[macro_gold_gate] score={score} hostile={hostile} "
          f"real_yield={ry[-1]:.2f} dollar={dx[-1]:.2f} -> {OUT}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
