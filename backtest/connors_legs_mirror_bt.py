#!/usr/bin/env python3
"""
ConnorsMR breadth-book legs — mirror dump-check (S-2026-07-07t operator ask).

Per leg: REAL engine parents (connors_legs_dump.cpp) -> x2 mirror sim on the
symbol's H1 (reuses connors_mirror_bt.sim, mode="close" worse-of fills).
Validated cell (ConnorsMirror_NAS100 @059918cd): arm 2.0% / gb 0.75% / retrig 2%.
Also prints a small plateau (arm 1.5/2.0 x gb 0.5/0.75/1.0 x retrig 0/2) + the
ex-best-trade check per cell. Judged STANDALONE (companion rule).

Per-symbol real rt_bp (engine_init rt tables): NAS100 3, US500 4, DJ30 2, GER40 2.

run: TICKDIR=~/Tick python3 backtest/connors_legs_mirror_bt.py
"""
import os, sys, subprocess, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("TICKDIR", os.path.expanduser("~/Tick"))
import rider_sweep_wf as RW
import connors_mirror_bt as CM

TICK = os.path.expanduser("~/Tick")
DUMP = "/tmp/connors_legs_dump"

RT_BP = {"NAS100": 3.0, "US500": 4.0, "DJ30": 2.0, "GER40": 2.0}
DAILY = {"NAS100": f"{TICK}/NDX_daily_2016_2026.csv",
         "US500":  f"{TICK}/SPX_daily_2016_2026.csv",
         "DJ30":   f"{TICK}/DJ30_daily_2016_2026.csv"}

# (leg_name, symbol, entry_mode, regime_gate) — exact live shadow configs
# (engine_init Connors MR breadth book; ConnorsRSI2 NAS100 itself already
# validated; ConnorsRSI2_GER parent DISABLED S-2026-06-24 -> skipped).
LEGS = [
    ("ConnorsIBS_NAS",    "NAS100", 1, 1),
    ("ConnorsStreak_NAS", "NAS100", 2, 0),
    ("ConnorsDouble_NAS", "NAS100", 5, 0),
    ("ConnorsRSI3_NAS",   "NAS100", 4, 1),
    ("ConnorsStreak_SPX", "US500",  2, 0),
    ("ConnorsDouble_SPX", "US500",  5, 0),
    ("ConnorsIBS_SPX",    "US500",  1, 0),
    ("ConnorsRSI2_SPX",   "US500",  0, 0),
    ("ConnorsIBS_DJ",     "DJ30",   1, 0),
    ("ConnorsRSI2_DJ",    "DJ30",   0, 0),
    ("ConnorsDouble_DJ",  "DJ30",   5, 0),
]

ARMS = [0.015, 0.020]
GBS  = [0.005, 0.0075, 0.010]
RETS = [0.0, 0.02]

def exbest(trades, rt_bp_unused=None):
    if len(trades) < 2: return 0.0
    best = max(p for _, p in trades)
    return sum(p for _, p in trades) - best

def main():
    h1cache = {}
    for (name, sym, mode, gate) in LEGS:
        pcsv = f"/tmp/parents_{name}.csv"
        r = subprocess.run([DUMP, DAILY[sym], pcsv, str(mode), str(gate)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"{name}: DUMP FAILED\n{r.stderr}"); continue
        parents = CM.load_parents(pcsv)
        if sym not in h1cache:
            H1, src = RW.find_data(sym)
            h1cache[sym] = H1
            print(f"\n### {sym} H1: {len(H1[0])} bars ({src}) "
                  f"{time.strftime('%Y-%m-%d', time.gmtime(H1[0][0]))} .. "
                  f"{time.strftime('%Y-%m-%d', time.gmtime(H1[0][-1]))}")
        H1 = h1cache[sym]
        ts0, ts1 = H1[0][0], H1[0][-1]
        cov = [p for p in parents if p[0] >= ts0 - 86400 and p[1] <= ts1 + 86400]
        print(f"\n=== {name} (mode {mode} gate {gate} rt {RT_BP[sym]}bp): "
              f"{len(parents)} parents, {len(cov)} in H1 coverage ===")
        if not cov:
            print("    no coverage -> UNTESTABLE"); continue
        print(f"  {'arm%':>5s} {'gb%':>5s} {'ret':>3s} | {'n':>4s} {'net_pt':>9s} {'PF':>5s} "
              f"{'WR%':>5s} {'bothH':>5s} {'H1':>8s} {'H2':>8s} {'bear22':>8s} {'exbest':>9s} {'net@2x':>9s}")
        for arm in ARMS:
            for gb in GBS:
                for ret in RETS:
                    tr = CM.sim(cov, H1, arm, gb, "close", RT_BP[sym], ret)
                    s = CM.stat(tr)
                    if s["n"] == 0: continue
                    s2 = CM.stat(CM.sim(cov, H1, arm, gb, "close", RT_BP[sym] * 2, ret))
                    star = " <== validated cell" if (arm, gb, ret) == (0.020, 0.0075, 0.02) else ""
                    print(f"  {arm*100:5.2f} {gb*100:5.2f} {('Y' if ret else '-'):>3s} | "
                          f"{s['n']:4d} {s['net']:9.1f} {s['pf']:5.2f} {s['wr']:5.1f} "
                          f"{('YES' if s['bh'] else 'no'):>5s} {s['h1']:8.1f} {s['h2']:8.1f} "
                          f"{s['bear']:8.1f} {exbest(tr):9.1f} {s2['net']:9.1f}{star}")

if __name__ == "__main__":
    main()
