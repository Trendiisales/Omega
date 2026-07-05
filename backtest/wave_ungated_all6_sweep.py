#!/usr/bin/env python3
"""wave_ungated_all6_sweep.py — fresh UN-GATED all-6 sweep of the CryptoWaveCompanion
(operator ask 2026-07-06, after the 200DMA bull-gate was removed, ~/Crypto b1e8ac8).

WHY: the wave companion used to bull-gate on daily>200DMA precisely because the un-gated
book bled in bear regimes ("Ungated, bear episodes bleed -> fails all-6"). The operator has
now ORDERED the 200DMA gate gone (feedback-no-200dma-crypto). This re-runs the standalone
all-6 gate on the UN-GATED book so we know, honestly, whether it still passes without the crutch.

all-6 (STANDALONE, never vs-WIDE per CompanionDominanceError):
  1 net > 0     2 PF > 1     3 WF half-1 net > 0     4 WF half-2 net > 0
  5 BULL-regime book net > 0   6 BEAR-regime book net > 0
Regime books are the same engine's clips isolated by the arm-bar regime (BULL_GATE=1 vs =bear):
their union IS the un-gated book, so "both regimes +" = the un-gated engine makes money in each.

Reuses the audited waves()/clips()/metrics() from crypto_wave_companions.py (single source of
truth for the model) — this script only splits by regime, adds thirds, and prints the verdict.

Data: ~/Crypto/backtest/data (same CSVs the live C++ engine replays). Cost RT 0.20%.
"""
import os, sys, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ.setdefault("DATA_DIR", os.path.expanduser("~/Crypto/backtest/data"))
os.environ.setdefault("COST_RT", "0.002")

spec = importlib.util.spec_from_file_location("cwc", os.path.join(HERE, "crypto_wave_companions.py"))
cwc = importlib.util.module_from_spec(spec); spec.loader.exec_module(cwc)


def book(sym, step, stag_h, rev, cold, gate):
    """Return sorted list of (ms, pnl) clips for one BULL_GATE mode."""
    os.environ["BULL_GATE"] = gate
    wv = cwc.waves(sym, step, stag_h * 3600_000, rev)
    reg = []
    for w in wv:
        for ms, p in cwc.clips(w, cold):
            reg.append((ms, p))
    reg.sort(key=lambda x: x[0])
    return wv, reg


def thirds_net(reg):
    n = len(reg); t = n // 3
    a, b, c = reg[:t], reg[t:2 * t], reg[2 * t:]
    return (cwc.metrics(a)["net"], cwc.metrics(b)["net"], cwc.metrics(c)["net"])


def evaluate(sym, step, stag_h, rev, cold):
    # un-gated (all regimes) = the actual shipped engine now
    wv, full = book(sym, step, stag_h, rev, cold, "0")
    if not full:
        return None
    m = cwc.metrics(full)
    h = len(full) // 2
    h1 = cwc.metrics(full[:h])["net"]; h2 = cwc.metrics(full[h:])["net"]
    t1, t2, t3 = thirds_net(full)
    # regime isolation (same engine, arm-bar regime split)
    _, bull = book(sym, step, stag_h, rev, cold, "1")
    _, bear = book(sym, step, stag_h, rev, cold, "bear")
    bull_net = cwc.metrics(bull)["net"]; bear_net = cwc.metrics(bear)["net"]
    pnls = [p for _, p in full]
    win = 100 * sum(1 for p in pnls if p > 0) / len(pnls)
    worst = min(pnls)
    checks = [m["net"] > 0, m["pf"] > 1, h1 > 0, h2 > 0, bull_net > 0, bear_net > 0]
    return dict(waves=len(wv), n=m["n"], net=m["net"], pf=m["pf"], dd=m["dd"], mar=m["mar"],
                h1=h1, h2=h2, t1=t1, t2=t2, t3=t3, bull=bull_net, bear=bear_net,
                win=win, worst=worst, npass=sum(checks), all6=all(checks), bear_n=len(bear))


LOCKED = {"BTC": (0.01, 48, 0.15, 0.04), "ETH": (0.01, 24, 0.15, 0.04)}


def main():
    print(f"\n{'='*96}\nUN-GATED all-6 sweep — CryptoWaveCompanion (200DMA gate REMOVED)   "
          f"cost_rt={os.environ['COST_RT']}\n{'='*96}")
    print("\n### LOCKED live config, UN-GATED (STEP 1% / per-coin STAG / REV 15% / COLD 4%) ###\n")
    hdr = (f"{'coin':4} {'waves':>5} {'nClip':>6} {'net$':>10} {'PF':>5} {'DD$':>10} {'MAR':>5} "
           f"{'H1$':>9} {'H2$':>9} | {'T1$':>8} {'T2$':>8} {'T3$':>8} | {'BULL$':>9} {'BEAR$':>9} "
           f"{'bearN':>5} | {'%win':>5} {'worst$':>9} | all-6")
    print(hdr); print("-" * len(hdr))
    for coin, (step, stag_h, rev, cold) in LOCKED.items():
        r = evaluate(coin, step, stag_h, rev, cold)
        print(f"{coin:4} {r['waves']:5d} {r['n']:6d} {r['net']:10.0f} {r['pf']:5.2f} {r['dd']:10.0f} "
              f"{r['mar']:5.2f} {r['h1']:9.0f} {r['h2']:9.0f} | {r['t1']:8.0f} {r['t2']:8.0f} "
              f"{r['t3']:8.0f} | {r['bull']:9.0f} {r['bear']:9.0f} {r['bear_n']:5d} | {r['win']:5.0f} "
              f"{r['worst']:9.0f} | {'PASS ' + str(r['npass']) + '/6' if r['all6'] else 'FAIL ' + str(r['npass']) + '/6'}")

    print("\n### UN-GATED grid — does ANY config pass all-6 without the 200DMA gate? ###\n")
    hdr2 = (f"{'coin':4} {'STEP':>4} {'STAG':>4} {'REV':>4} {'COLD':>4} | {'net$':>10} {'PF':>5} "
            f"{'H1$':>9} {'H2$':>9} {'BULL$':>9} {'BEAR$':>9} | all-6")
    print(hdr2); print("-" * len(hdr2))
    grid_pass = 0
    for coin in ("BTC", "ETH"):
        stags = (24, 48) if coin == "BTC" else (12, 24)
        for step in (0.01, 0.02, 0.03):
            for stag_h in stags:
                for rev in (0.10, 0.15):
                    for cold in (None, 0.04, 0.08):
                        r = evaluate(coin, step, stag_h, rev, cold)
                        if not r:
                            continue
                        if r["all6"]:
                            grid_pass += 1
                        cs = "none" if cold is None else f"{cold*100:.0f}%"
                        mark = "PASS 6/6" if r["all6"] else f"fail {r['npass']}/6"
                        # only print passes + the locked-adjacent rows to keep it readable
                        if r["all6"] or (step == 0.01 and rev == 0.15):
                            print(f"{coin:4} {step*100:3.0f}% {stag_h:4.0f} {rev*100:3.0f}% {cs:>4} | "
                                  f"{r['net']:10.0f} {r['pf']:5.2f} {r['h1']:9.0f} {r['h2']:9.0f} "
                                  f"{r['bull']:9.0f} {r['bear']:9.0f} | {mark}")
    print(f"\n>>> configs passing all-6 UN-GATED across the grid: {grid_pass}")
    print(">>> KEY: BEAR$ is the make-or-break column — the 200DMA gate existed to skip bear waves.\n")


if __name__ == "__main__":
    main()
