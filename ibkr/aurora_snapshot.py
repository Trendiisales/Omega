#!/usr/bin/env python3
"""aurora_snapshot.py -- periodic Aurora liquidity-map writer for the GUI.

Loops over the configured futures (MGC gold, NQ nasdaq), runs the Aurora shelf
engine on TODAY's recorded bridge tape + L2, and writes one JSON snapshot per
symbol to --out-dir. A GUI / API layer reads those JSONs; this script does the
compute, nothing renders here (validate-before-route, like gex_chain).

Pairs with tools/ibkr_dom_bridge.py output:
    ibkr_trades_<SYM>_<UTCDATE>.csv  (AllLast tape)
    ibkr_l2_<SYM>_<UTCDATE>.csv      (best bid/ask for delta classification)

usage (one pass):
    python ibkr/aurora_snapshot.py --once
loop (every N sec, for a scheduled task that stays resident):
    python ibkr/aurora_snapshot.py --interval 60
"""
from __future__ import annotations
import argparse, json, os, sys, time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aurora_flow import AuroraCfg, run_file

# symbol -> (ticks_per_row, tf_min). Tuned per instrument granularity.
SYMBOLS = {
    "MGC": dict(ticks_per_row=10, tf_min=30),   # gold: mintick 0.10 -> $1.00 rows
    "NQ":  dict(ticks_per_row=4,  tf_min=30),   # nasdaq: mintick 0.25 -> 1.0pt rows
}


def utc_date():
    # caller-stamped via env if provided (no Date.now in some harnesses); else now
    ov = os.environ.get("AURORA_UTC_DATE")
    return ov if ov else datetime.now(timezone.utc).strftime("%Y-%m-%d")


def one_pass(in_dir, out_dir, date_str, stamp_ms=None):
    if stamp_ms is None:
        stamp_ms = int(time.time() * 1000)   # epoch ms -- drives AuroraGate staleness
    os.makedirs(out_dir, exist_ok=True)
    results = {}
    for sym, p in SYMBOLS.items():
        trades = os.path.join(in_dir, f"ibkr_trades_{sym}_{date_str}.csv")
        l2 = os.path.join(in_dir, f"ibkr_l2_{sym}_{date_str}.csv")
        if not os.path.exists(trades):
            results[sym] = {"sym": sym, "error": "no tape file", "path": trades}
            continue
        cfg = AuroraCfg(ticks_per_row=p["ticks_per_row"], tf_min=p["tf_min"])
        try:
            snap = run_file(trades, l2, cfg, sym)
        except Exception as e:
            snap = {"sym": sym, "error": f"{type(e).__name__}: {e}"}
        snap["stamp_ms"] = stamp_ms
        out = os.path.join(out_dir, f"aurora_{sym}.json")
        with open(out, "w") as fh:
            json.dump(snap, fh, indent=2)
        results[sym] = snap
    # combined index the GUI can fetch in one call
    with open(os.path.join(out_dir, "aurora_all.json"), "w") as fh:
        json.dump({"stamp_ms": stamp_ms, "date": date_str,
                   "symbols": list(SYMBOLS), "snaps": results}, fh, indent=2)

    # Flat gate file consumed by the C++ entry gate (include/AuroraGate.hpp).
    # Maps each futures-tape symbol to the tradable symbol(s) it proxies (spot
    # gold has no tape -> MGC; NQ -> NAS100). A symbol with no/errored gate is
    # OMITTED so the C++ side fails open (allows) for it -- never block on a gap.
    GATE_MAP = {"MGC": ["XAUUSD"], "NQ": ["NAS100"]}
    try:
        st = int(stamp_ms) if stamp_ms else 0
        lines = ["# SYMBOL\tallow_long\tallow_short\tbias\troom_long_atr\troom_short_atr\tstamp_ms"]
        for fut, tradables in GATE_MAP.items():
            g = (results.get(fut) or {}).get("gate")
            if not g:
                continue
            al   = 1 if g.get("allow_long")  else 0
            ash  = 1 if g.get("allow_short") else 0
            bias = g.get("bias", "neutral")
            rl   = g.get("room_long_atr", 99.0)
            rs   = g.get("room_short_atr", 99.0)
            for t in tradables:
                lines.append(f"{t}\t{al}\t{ash}\t{bias}\t{rl}\t{rs}\t{st}")
        with open(os.path.join(out_dir, "aurora_gate.tsv"), "w") as fh:
            fh.write("\n".join(lines) + "\n")
    except Exception:
        pass  # gate-file write must never break the snapshotter
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-dir", default=r"C:\Omega\logs\ibkr_l2")
    ap.add_argument("--out-dir", default=r"C:\Omega\logs\aurora")
    ap.add_argument("--interval", type=int, default=0, help="loop every N sec; 0 = once")
    ap.add_argument("--once", action="store_true")
    a = ap.parse_args()

    def _pass():
        ds = utc_date()
        res = one_pass(a.in_dir, a.out_dir, ds)
        for sym, s in res.items():
            m = s.get("_meta", {})
            print(f"[aurora] {sym} {ds}: trades={m.get('trades','?')} "
                  f"shelves={s.get('n_shelves','?')} delta={m.get('delta_via','?')} "
                  f"{'ERR '+s['error'] if 'error' in s else ''}", flush=True)

    if a.once or a.interval <= 0:
        _pass(); return
    while True:
        try:
            _pass()
        except Exception as e:
            print(f"[aurora] pass error: {e}", flush=True)
        time.sleep(a.interval)


if __name__ == "__main__":
    main()
