#!/usr/bin/env python3
"""
macro_gold_gate.py -- daily producer for the MacroGoldGate (build #2, 2026-06-21).

WHY build #2: FRED (fred.stlouisfed.org + api.stlouisfed.org) is Akamai-WAF-blocked
from the Omega VPS (TCP 443 connects, app-layer response tarpitted on the datacenter
IP) -- no firewall fix possible. Re-sourced to hosts the VPS CAN reach:
  * 10y REAL yield  <- US Treasury daily real-yield curve (home.treasury.gov, "10 YR"
                       column). This IS the same series FRED publishes as DFII10
                       (FRED sources DFII10 from Treasury) -> validated signal PRESERVED,
                       no re-validation of backtest/macro_gold_regime.py needed.
  * broad dollar    <- Yahoo DX-Y.NYB (ICE dollar index). Substitutes FRED DTWEXBGS on
                       the weight-1 term only (real-yield is weight-2 / dominant); slope
                       SIGN is what the score uses, and DXY<->broad-dollar slope-sign
                       agree. FRED original kept at macro_gold_gate.py.fred-bak.

Writes the flat gate file include/MacroGoldGate.hpp reads:
    logs/macro/macro_gold_gate.tsv
    # macro_gold_gate  score  hostile  real_yield  dollar  stamp_ms
    MACRO  -3  1  2.31  121.40  1781679000000

score = 2*sign(-d20(real_yield)) + 1*sign(-d20(dollar))   (range -3..+3)
hostile = 1 when score <= HOSTILE_THRESH (-2): real yields rising hard => de-risk gold.
FAIL-SAFE: on ANY fetch/parse error this writes NOTHING (keeps last good file); the C++
gate self-stales after 3 days. Schedule daily after ~16:30 ET (Treasury posts ~mid-PM ET).
"""
import sys, time, json, urllib.request, datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "logs/macro/macro_gold_gate.tsv"
CACHE = ROOT / "data/macro"
SLOPE_LB = 20          # business-day slope lookback (matches the backtest)
HOSTILE_THRESH = -2    # score <= this => hostile (real yields clearly rising)
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"

def _get(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()

def fetch_real_yield():
    """US Treasury daily real-yield curve, 10 YR column, chronological (oldest->newest)."""
    yr = datetime.date.today().year
    rows = []  # (date, ten_yr)
    for y in (yr - 1, yr):
        url = ("https://home.treasury.gov/resource-center/data-chart-center/interest-rates/"
               f"daily-treasury-rates.csv/{y}/all?type=daily_treasury_real_yield_curve")
        try:
            text = _get(url)
        except Exception:
            continue
        (CACHE / f"treasury_real_{y}.csv").write_text(text)
        lines = text.splitlines()
        if not lines:
            continue
        hdr = [h.strip().strip('"') for h in lines[0].split(",")]
        try:
            ix = hdr.index("10 YR")
        except ValueError:
            continue
        for ln in lines[1:]:
            parts = [p.strip().strip('"') for p in ln.split(",")]
            if len(parts) <= ix or parts[ix] in ("", "N/A"):
                continue
            try:
                d = datetime.datetime.strptime(parts[0], "%m/%d/%Y").date()
                rows.append((d, float(parts[ix])))
            except ValueError:
                continue
    if len(rows) < SLOPE_LB + 1:
        raise ValueError(f"treasury real-yield: too few obs ({len(rows)})")
    rows.sort(key=lambda x: x[0])
    return [v for _, v in rows]

def fetch_dollar():
    """Yahoo ICE dollar index DX-Y.NYB daily closes, chronological."""
    url = "https://query1.finance.yahoo.com/v8/finance/chart/DX-Y.NYB?range=6mo&interval=1d"
    j = json.loads(_get(url))
    res = j["chart"]["result"][0]
    closes = [c for c in res["indicators"]["quote"][0]["close"] if c is not None]
    if len(closes) < SLOPE_LB + 1:
        raise ValueError(f"dollar: too few obs ({len(closes)})")
    return closes

def slope_sign(vals):
    # FALLING macro driver -> +1 bullish gold ; RISING -> -1
    diff = vals[-1] - vals[-1 - SLOPE_LB]
    return -1 if diff > 0 else (1 if diff < 0 else 0)

def main():
    CACHE.mkdir(parents=True, exist_ok=True)
    try:
        ry = fetch_real_yield()
        dx = fetch_dollar()
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
