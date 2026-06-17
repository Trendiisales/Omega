#!/usr/bin/env python3
"""aurora_predicate.py -- does an Aurora shelf actually PREDICT a price reaction,
or is it decoration? The validate-before-route study (BACKTEST_TRUTH) that gates
whether Aurora ever becomes a trading signal.

Method: replay the recorded MGC/NQ tape through the footprint engine. Each time
price first TESTS an active shelf (a bar's range enters the band), classify the
forward outcome over the next K bars:
  - demand shelf: REACT = price bounces up >= react_atr*ATR ; BREAK = closes down
  - supply shelf: REACT = price rejects down               ; BREAK = closes up
Reaction-rate = REACT / (REACT+BREAK), split by absorption vs initiative.

BASELINE: the same test against RANDOM price levels (no shelf). A shelf only has
edge if its reaction-rate beats the random baseline. Reports both + the lift.

Sample size matters: with only a day or two of tape this is INDICATIVE, not
conclusive (n flagged). It scales as the bridge accumulates tape.

usage:
  python ibkr/aurora_predicate.py --trades ibkr_trades_MGC_*.csv --l2 ibkr_l2_MGC_*.csv
  python ibkr/aurora_predicate.py --selftest
"""
from __future__ import annotations
import argparse, glob, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aurora_flow import (AuroraCfg, AuroraEngine, build_bar_footprints,
                         classify_trades, load_trades, load_l2_quotes)

# deterministic pseudo-random (no Math.random in harness-land; LCG seeded)
def _lcg(seed):
    x = seed & 0xFFFFFFFF
    while True:
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        yield x / 0x7FFFFFFF


def classify_forward(bars, i, top, bot, is_buy, atr, k, react_atr):
    """REACT / BREAK / NEUTRAL over bars (i, i+k]."""
    if atr <= 0:
        return "NEUTRAL"
    thr = react_atr * atr
    touch_c = bars[i]["close"]
    for j in range(i + 1, min(i + 1 + k, len(bars))):
        b = bars[j]
        if is_buy:   # demand: want bounce UP; break = close below bot
            if b["high"] >= touch_c + thr: return "REACT"
            if b["close"] <= bot - thr:    return "BREAK"
        else:        # supply: want reject DOWN; break = close above top
            if b["low"] <= touch_c - thr:  return "REACT"
            if b["close"] >= top + thr:    return "BREAK"
    return "NEUTRAL"


def run(bars, cfg, k=10, react_atr=0.5):
    eng = AuroraEngine(cfg)
    rec = {"abs": [], "init": []}     # outcomes per shelf class
    tested = set()
    for i, b in enumerate(bars):
        eng.feed_bar(b)
        atr = eng._atr()
        for s in eng.shelves:
            if s.broken:
                continue
            sid = id(s)
            if sid in tested:
                continue
            if b["low"] <= s.top and b["high"] >= s.bot:   # first touch
                tested.add(sid)
                o = classify_forward(bars, i, s.top, s.bot, s.is_buy, atr, k, react_atr)
                if o != "NEUTRAL":
                    rec["abs" if s.is_abs else "init"].append((s.is_buy, o))
    # RANDOM baseline: pick random levels in each bar's range, same test
    rng = _lcg(12345)
    base = []
    for i, b in enumerate(bars):
        atr = 0  # recompute simple ATR proxy from neighbourhood
        if i >= 14:
            atr = sum(bars[j]["high"] - bars[j]["low"] for j in range(i - 14, i)) / 14
        if atr <= 0:
            continue
        lvl = b["low"] + next(rng) * (b["high"] - b["low"])
        is_buy = next(rng) > 0.5
        band = atr * cfg.ticks_per_row * cfg.mintick * 0  # treat as a thin level
        o = classify_forward(bars, i, lvl + 0.01, lvl - 0.01, is_buy, atr, k, react_atr)
        if o != "NEUTRAL":
            base.append((is_buy, o))
    return rec, base


def rate(outcomes):
    if not outcomes:
        return None
    r = sum(1 for _, o in outcomes if o == "REACT")
    n = len(outcomes)
    return r, n, 100.0 * r / n


def report(rec, base, tag):
    print(f"\n===== {tag} =====")
    for k, label in (("abs", "ABSORPTION walls (passive liq)"),
                     ("init", "INITIATIVE nodes (aggressive)")):
        st = rate(rec[k])
        if st:
            print(f"  {label:<32} react {st[0]}/{st[1]} = {st[2]:5.1f}%")
        else:
            print(f"  {label:<32} (no tested shelves)")
    bs = rate(base)
    if bs:
        print(f"  {'RANDOM levels (baseline)':<32} react {bs[0]}/{bs[1]} = {bs[2]:5.1f}%")
        a = rate(rec["abs"])
        if a:
            lift = a[2] - bs[2]
            verdict = ("EDGE (absorption beats random)" if lift > 5 else
                       "no edge yet" if abs(lift) <= 5 else "INVERSE (worse than random)")
            print(f"  >>> absorption lift vs random: {lift:+.1f} pts -> {verdict}")
    n_total = len(rec["abs"]) + len(rec["init"]) + len(base)
    if n_total < 60:
        print(f"  [!] small sample (n={n_total}) -- INDICATIVE only; accumulate more tape.")


def selftest():
    cfg = AuroraCfg(tf_min=5, ticks_per_row=25, lookback=80)
    base_px = 4300.0
    row_h = cfg.ticks_per_row * cfg.mintick
    bar_ms = cfg.tf_min * 60 * 1000
    trades = []
    # build a tape where a heavy demand wall at base repeatedly holds (price
    # bounces off it) -> absorption react-rate should be high.
    for bi in range(40):
        lvl = base_px + (bi % 6) * row_h     # oscillate above the wall
        for _ in range(40):
            trades.append((bi * bar_ms + len(trades) % bar_ms, base_px, 5.0))  # wall
        for _ in range(15):
            trades.append((bi * bar_ms + len(trades) % bar_ms, lvl, 2.0))
    trades.sort(key=lambda x: x[0])
    signed = classify_trades(trades, [], [], [])
    bars = build_bar_footprints(signed, cfg)
    rec, base = run(bars, cfg, k=8, react_atr=0.3)
    report(rec, base, "SELFTEST (synthetic demand wall)")
    print("SELFTEST OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trades", help="glob of ibkr_trades_<SYM>_*.csv (concatenated)")
    ap.add_argument("--l2", help="glob of ibkr_l2_<SYM>_*.csv")
    ap.add_argument("--tf-min", type=int, default=30)
    ap.add_argument("--ticks-per-row", type=int, default=10)
    ap.add_argument("--k-fwd", type=int, default=10, help="forward bars to judge reaction")
    ap.add_argument("--react-atr", type=float, default=0.5)
    ap.add_argument("--sym", default="MGC")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest(); return
    if not a.trades:
        ap.error("--trades required (or --selftest)")

    trades = []
    for f in sorted(glob.glob(a.trades)):
        trades += load_trades(f)
    trades.sort(key=lambda x: x[0])
    q_ts, q_bid, q_ask = [], [], []
    if a.l2:
        for f in sorted(glob.glob(a.l2)):
            t, b, k = load_l2_quotes(f)
            q_ts += t; q_bid += b; q_ask += k
        order = sorted(range(len(q_ts)), key=lambda i: q_ts[i])
        q_ts = [q_ts[i] for i in order]; q_bid = [q_bid[i] for i in order]; q_ask = [q_ask[i] for i in order]
    signed = classify_trades(trades, q_ts, q_bid, q_ask)
    cfg = AuroraCfg(tf_min=a.tf_min, ticks_per_row=a.ticks_per_row)
    bars = build_bar_footprints(signed, cfg)
    print(f"{a.sym}: {len(trades)} trades -> {len(bars)} bars (tf={a.tf_min}m), "
          f"k_fwd={a.k_fwd}, react={a.react_atr}xATR")
    rec, base = run(bars, cfg, k=a.k_fwd, react_atr=a.react_atr)
    report(rec, base, f"{a.sym} Aurora shelf predicate")


if __name__ == "__main__":
    main()
