#!/usr/bin/env python3
"""Orchestrate the faithful FX up-jump ladder sweep on IBKR H1 data.

Runs /tmp/fx_ibkr_sweep (real FxLadderPair) for every (pair,W,thr,exit) cell in a
FRESH mktemp -d cwd (engine writes persistence to cwd -> fresh dir = deterministic).
Applies the all-6 STANDALONE verdict: net>0 AND PF>=1.3 AND WF_H1>0 AND WF_H2>0
AND bull>0 AND bear>0. Judged additively (separate independent book), NOT vs WIDE.
"""
import subprocess, tempfile, shutil, os, sys

BIN = "/tmp/fx_ibkr_sweep"
TICK = "/Users/jo/Tick"
PAIRS = [  # pair, csv, short_downjump, rt_cost_bp (task: 2.0; NZD live uses 2.5)
    ("EURUSD", f"{TICK}/EURUSD_IBKR_H1.csv", 0, 2.0),
    ("GBPUSD", f"{TICK}/GBPUSD_IBKR_H1.csv", 0, 2.0),
    ("NZDUSD", f"{TICK}/NZDUSD_IBKR_H1.csv", 0, 2.0),
    ("AUDUSD", f"{TICK}/AUDUSD_IBKR_H1.csv", 0, 2.0),
    ("USDCAD", f"{TICK}/USDCAD_IBKR_H1.csv", 1, 2.0),
]
LIVE = {"EURUSD": (48, 0.5), "GBPUSD": (48, 1.0), "NZDUSD": (24, 1.5),
        "AUDUSD": (72, 0.75), "USDCAD": (96, 0.5)}
WS = [24, 48, 72, 96]
THRS = [0.5, 0.75, 1.0, 1.5]
CUR_EXIT = (1.0, 0.08, 0.10)   # wide_arm, be_entry, wide_gb (shipped)

def run(csv, W, thr, wide_arm, be, gb, short, rt):
    d = tempfile.mkdtemp()
    try:
        p = subprocess.run([BIN, csv, str(W), str(thr), str(wide_arm), str(be),
                            str(gb), str(short), str(rt)],
                           cwd=d, capture_output=True, text=True, timeout=120)
        for ln in p.stdout.splitlines():
            if ln.startswith("RESULT"):
                kv = dict(x.split("=") for x in ln.split()[1:])
                return {k: float(v) for k, v in kv.items()}
    finally:
        shutil.rmtree(d, ignore_errors=True)
    return None

def verdict(r):
    return (r and r["net"] > 0 and r["pf"] >= 1.3 and r["h1"] > 0 and r["h2"] > 0
            and r["bull"] > 0 and r["bear"] > 0)

def fmt(r):
    if not r: return "  (no result)"
    v = "PASS" if verdict(r) else "fail"
    return (f"n={int(r['n']):5d} net={r['net']:+7.1f} PF={r['pf']:4.2f} "
            f"H1={r['h1']:+6.1f} H2={r['h2']:+6.1f} bull={r['bull']:+6.1f} "
            f"bear={r['bear']:+6.1f} [{v}]")

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "entry"
    if mode == "entry":
        wa, be, gb = CUR_EXIT
        for pair, csv, short, rt in PAIRS:
            print(f"\n===== {pair} {'SHORT' if short else 'LONG'}  ENTRY GRID (current exits arm{wa}/be{be}/gb{gb}, rt{rt}bp) =====")
            best = None
            for W in WS:
                for thr in THRS:
                    r = run(csv, W, thr, wa, be, gb, short, rt)
                    star = " <LIVE" if LIVE[pair] == (W, thr) else ""
                    print(f"  W{W:<3d} thr{thr:<5.2f} {fmt(r)}{star}")
                    if r and verdict(r) and (best is None or r["net"] > best[1]["net"]):
                        best = ((W, thr), r)
            if best:
                print(f"  >> best PASS: W{best[0][0]} thr{best[0][1]} net={best[1]['net']:+.1f} PF{best[1]['pf']:.2f}")
            else:
                print(f"  >> NO all-6 PASS in entry grid")
    elif mode == "exit":
        # exit sweep on a specified pair's cell: argv2=pair W thr
        pair, W, thr = sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
        csv = dict((p[0], p[1]) for p in PAIRS)[pair]
        short = dict((p[0], p[2]) for p in PAIRS)[pair]
        rt = dict((p[0], p[3]) for p in PAIRS)[pair]
        print(f"\n===== {pair} W{W} thr{thr} EXIT SWEEP (wide_arm x be_entry, gb=0.10, rt{rt}) =====")
        for wa in [0.0, 0.5, 1.0]:
            for be in [0.0, 0.08, 0.15]:
                r = run(csv, W, thr, wa, be, 0.10, short, rt)
                print(f"  arm{wa:<4.1f} be{be:<5.2f} {fmt(r)}")

if __name__ == "__main__":
    main()
