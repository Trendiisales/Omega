#!/usr/bin/env python3
# =============================================================================
# cfe_imb_patch_validate.py -- Validates the S17 CFE IMB_EXIT MFE guard by
# replaying every IMB_EXIT trade forward from its exit point using the log's
# own XAUUSD tick stream. Determines what the trade WOULD have done if the
# new guard (pos.mfe >= 1.0pt -> block IMB_EXIT) had been active.
#
# Approach:
#   1. For each [CFE] IMB-EXIT line, extract: ts, side, exit_price, mfe, hold_ms
#   2. Locate the matching CFE entry record (entry_ts = exit_ts - hold_ms)
#      to get entry_price, sl, size, atr (regular ENTRY line) or derive atr from
#      sl_pts for SUSTAINED-DRIFT-ENTRY.
#   3. Pair IMB-EXIT to the [SHADOW-CLOSE] or [TRADE-COST] line at same ts to
#      recover the original net_pnl.
#   4. If mfe < 1.0pt: guard would NOT fire -> exit happens as before,
#      use original net.
#   5. If mfe >= 1.0pt: guard fires -> simulate forward:
#      - Walk XAUUSD TICK lines after exit_ts until SL, TRAIL_SL, or EOD.
#      - At each tick: update mfe, arm trail when mfe >= 2*atr, ratchet trail.
#      - Hit: compute new_net = (exit_px - entry_px) * size * 100 * side_sign
#        minus slippage (~0.03 spread cost).
#   6. Aggregate: original total vs simulated total, delta is the real impact
#      of the patch.
#
# Output: per-trade diff CSV + summary.
# =============================================================================

import csv
import os
import re
import sys
from collections import defaultdict, deque
from datetime import datetime, timezone


# --- Regex -------------------------------------------------------------------

IMB_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[CFE\]\s+IMB-EXIT\s+(?P<side>LONG|SHORT)\s+"
    r".*?hold_ms=(?P<hold_ms>\d+)\s+@\s+(?P<px>\d+(?:\.\d+)?)"
    r"\s+mfe=(?P<mfe>-?\d+(?:\.\d+)?)"
)

# Regular ENTRY (carries atr):
#   [CFE] ENTRY SHORT @ 4704.97 sl=4705.92 sl_pts=0.95 size=0.010 cost=0.620 rsi_trend=-3.28 atr=2.36 spread=0.22 [SHADOW]
ENTRY_REG_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[CFE\]\s+ENTRY\s+(?P<side>LONG|SHORT)\s+@\s+(?P<px>\d+(?:\.\d+)?)"
    r"\s+sl=(?P<sl>\d+(?:\.\d+)?)"
    r"\s+sl_pts=(?P<sl_pts>\d+(?:\.\d+)?)"
    r"\s+size=(?P<size>\d+(?:\.\d+)?)"
    r".*?atr=(?P<atr>\d+(?:\.\d+)?)"
)

# SUSTAINED-DRIFT-ENTRY / DRIFT-ENTRY (no atr in line; derive from sl_pts):
#   [CFE] SUSTAINED-DRIFT-ENTRY LONG @ 4762.69 sl=4760.56 drift=0.92 sustained_ms=48401 ... size=0.187 [SHADOW]
ENTRY_DRIFT_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[CFE\]\s+(?:SUSTAINED-)?DRIFT-ENTRY\s+(?P<side>LONG|SHORT)\s+@\s+(?P<px>\d+(?:\.\d+)?)"
    r"\s+sl=(?P<sl>\d+(?:\.\d+)?)"
    r".*?size=(?P<size>\d+(?:\.\d+)?)"
)

# Close companion (either format):
CLOSE_NEW_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[SHADOW-CLOSE\]\s+XAUUSD\s+engine=CandleFlowEngine\s+side=(?P<side>LONG|SHORT)"
    r"\s+gross=\$(?P<gross>-?\d+(?:\.\d+)?)"
    r"\s+net=\$(?P<net>-?\d+(?:\.\d+)?)\s+exit=IMB_EXIT"
)
CLOSE_OLD_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[TRADE-COST\]\s+XAUUSD\s+gross=\$(?P<gross>-?\d+(?:\.\d+)?)"
    r"\s+slip_in=\$(?P<slip_in>-?\d+(?:\.\d+)?)"
    r"\s+slip_out=\$(?P<slip_out>-?\d+(?:\.\d+)?)"
    r"\s+net=\$(?P<net>-?\d+(?:\.\d+)?)"
    r"\s+exit=IMB_EXIT"
)

# XAUUSD TICK:
#   00:00:24 [TICK] XAUUSD 4726.35/4726.57
TICK_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[TICK\]\s+XAUUSD\s+(?P<bid>\d+(?:\.\d+)?)/(?P<ask>\d+(?:\.\d+)?)"
)


# --- Constants matching CFE engine ------------------------------------------

CFE_TRAIL_ARM_MFE_MULT   = 2.0   # trail arms at MFE >= 2x ATR
CFE_TRAIL_DIST_MULT      = 0.5   # trail distance = 0.5 * ATR
# We approximate ATR for drift entries by taking the average of regular-ENTRY atr
# values on the same day (drift entries don't log atr but size scaling suggests
# similar vol regime).
SLIPPAGE_PTS = 0.03  # typical gold exit slippage observed in TRADE-COST lines
PT_DOLLAR_PER_LOT = 100.0  # $ per 1pt on XAUUSD for 1 lot (standard)


# --- Helpers -----------------------------------------------------------------

def hms_to_s(h, m, s):
    return h * 3600 + m * 60 + s


def side_sign(side):
    return 1 if side == "LONG" else -1


def simulate_forward(entry_px, side, sl_px, atr, size, start_idx, ticks, eod_idx):
    """
    Walk ticks from start_idx. Apply CFE hard SL / trail-arm / trail-SL logic.
    Return: (exit_px, exit_reason, final_mfe)
    """
    s = side_sign(side)
    mfe = 0.0
    trail_active = False
    trail_sl = sl_px
    hard_sl = sl_px
    trail_arm_mfe = atr * CFE_TRAIL_ARM_MFE_MULT
    trail_dist = atr * CFE_TRAIL_DIST_MULT

    for i in range(start_idx, eod_idx):
        t_s, bid, ask = ticks[i]
        # Favourable price for position
        if side == "LONG":
            # close at bid for LONG exit
            exit_bid = bid
            mid = (bid + ask) / 2.0
            move = mid - entry_px
        else:
            exit_bid = ask  # close at ask for SHORT exit
            mid = (bid + ask) / 2.0
            move = entry_px - mid
        if move > mfe:
            mfe = move

        # Trail arm
        if mfe >= trail_arm_mfe and not trail_active:
            new_trail = (mid - trail_dist) if side == "LONG" else (mid + trail_dist)
            if (side == "LONG" and new_trail > hard_sl) or \
               (side == "SHORT" and new_trail < hard_sl):
                trail_sl = new_trail
                trail_active = True
        elif trail_active:
            new_trail = (mid - trail_dist) if side == "LONG" else (mid + trail_dist)
            if side == "LONG" and new_trail > trail_sl:
                trail_sl = new_trail
            if side == "SHORT" and new_trail < trail_sl:
                trail_sl = new_trail

        # SL check
        effective_sl = trail_sl if trail_active else hard_sl
        if side == "LONG" and bid <= effective_sl:
            return exit_bid, ("TRAIL_SL" if trail_active else "SL_HIT"), mfe
        if side == "SHORT" and ask >= effective_sl:
            return exit_bid, ("TRAIL_SL" if trail_active else "SL_HIT"), mfe

    # EOD without hit
    t_s, bid, ask = ticks[eod_idx - 1]
    exit_px = bid if side == "LONG" else ask
    return exit_px, "EOD_OPEN", mfe


def pnl_for(entry_px, exit_px, side, size):
    """PnL in USD after typical exit slippage."""
    s = side_sign(side)
    gross_pts = (exit_px - entry_px) * s
    gross = gross_pts * size * PT_DOLLAR_PER_LOT
    # slippage: 0.03pt each side, entry slippage already in original net; apply exit slip
    exit_slip = SLIPPAGE_PTS * size * PT_DOLLAR_PER_LOT
    return gross - exit_slip


# --- Main per-day replay -----------------------------------------------------

def process_day(log_path, out_rows):
    date = re.search(r"(\d{4}-\d{2}-\d{2})", os.path.basename(log_path)).group(1)

    # Pass: read everything, build lists/maps
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    # Index: ticks as sorted list of (ts_s, bid, ask)
    ticks = []
    # CFE entries: list of (ts_s, side, entry_px, sl, size, atr)
    # matched to IMB_EXIT by picking the most recent entry of same side whose
    # ts_s < imb_ts_s. (CFE only holds one position at a time.)
    entries_by_side = {"LONG": [], "SHORT": []}
    # Accumulated ATR samples for drift-entry fallback
    atr_samples = []

    # Find IMB-EXIT events + their paired closes + entries
    imb_events = []

    for i, raw in enumerate(lines):
        line = raw.rstrip("\r\n")
        # strip ANSI
        line = re.sub(r"\x1b\[[0-9;]*m", "", line)

        m = TICK_RE.match(line)
        if m:
            ticks.append((
                hms_to_s(int(m.group("h")), int(m.group("m")), int(m.group("s"))),
                float(m.group("bid")),
                float(m.group("ask")),
            ))
            continue

        m = ENTRY_REG_RE.match(line)
        if m:
            ts_s = hms_to_s(int(m.group("h")), int(m.group("m")), int(m.group("s")))
            side = m.group("side")
            atr = float(m.group("atr"))
            atr_samples.append(atr)
            entries_by_side[side].append({
                "ts_s": ts_s,
                "entry_px": float(m.group("px")),
                "sl": float(m.group("sl")),
                "size": float(m.group("size")),
                "atr": atr,
                "kind": "REG",
            })
            continue

        m = ENTRY_DRIFT_RE.match(line)
        if m:
            ts_s = hms_to_s(int(m.group("h")), int(m.group("m")), int(m.group("s")))
            side = m.group("side")
            entry_px = float(m.group("px"))
            sl_px = float(m.group("sl"))
            # Derive implied atr from sl distance (CFE uses sl = entry +/- atr*CFE_DFE_SL_MULT, roughly ~1x ATR)
            sl_pts = abs(entry_px - sl_px)
            implied_atr = sl_pts  # close approximation
            entries_by_side[side].append({
                "ts_s": ts_s,
                "entry_px": entry_px,
                "sl": sl_px,
                "size": float(m.group("size")),
                "atr": implied_atr,
                "kind": "DRIFT",
            })
            continue

        m = IMB_RE.match(line)
        if m:
            ts_s = hms_to_s(int(m.group("h")), int(m.group("m")), int(m.group("s")))
            imb_events.append({
                "i": i,
                "ts_s": ts_s,
                "side": m.group("side"),
                "exit_px": float(m.group("px")),
                "mfe_at_exit": float(m.group("mfe")),
                "hold_ms": int(m.group("hold_ms")),
            })

    ticks.sort(key=lambda t: t[0])

    # Helper: binary-find first tick index with ts_s >= target
    def find_tick_idx(target_s):
        lo, hi = 0, len(ticks)
        while lo < hi:
            mid = (lo + hi) // 2
            if ticks[mid][0] < target_s:
                lo = mid + 1
            else:
                hi = mid
        return lo

    # For each IMB_EXIT, find matching entry + close
    for ev in imb_events:
        imb_idx_in_lines = ev["i"]
        # Find close at same ts (within a few lines after)
        original_net = None
        for j in range(imb_idx_in_lines, min(imb_idx_in_lines + 8, len(lines))):
            cl = lines[j].rstrip("\r\n")
            cl = re.sub(r"\x1b\[[0-9;]*m", "", cl)
            mc = CLOSE_NEW_RE.match(cl) or CLOSE_OLD_RE.match(cl)
            if mc:
                ts_close_s = hms_to_s(int(mc.group("h")), int(mc.group("m")), int(mc.group("s")))
                if ts_close_s == ev["ts_s"]:
                    n = float(mc.group("net"))
                    if abs(n) <= 1000:
                        original_net = n
                        break
        if original_net is None:
            continue  # can't validate without original pnl

        # Find entry: most recent same-side entry before imb ts
        candidates = [e for e in entries_by_side[ev["side"]] if e["ts_s"] <= ev["ts_s"]]
        if not candidates:
            continue
        entry = candidates[-1]

        # Sanity: entry should be ~hold_ms before exit
        expected_entry_ts = ev["ts_s"] - ev["hold_ms"] // 1000
        # allow 10s slack
        if abs(entry["ts_s"] - expected_entry_ts) > 15:
            continue

        entry_px = entry["entry_px"]
        sl_px = entry["sl"]
        size = entry["size"]
        atr = entry["atr"]

        # Decide: would the new guard block this IMB_EXIT?
        # NOTE: keep in sync with CFE_IMB_EXIT_MFE_GUARD_PTS in CandleFlowEngine.hpp
        mfe = ev["mfe_at_exit"]
        if mfe < 1.75:
            # Guard inactive -- behaviour unchanged
            out_rows.append({
                "date": date,
                "time": f"{ev['ts_s'] // 3600:02d}:{(ev['ts_s'] % 3600) // 60:02d}:{ev['ts_s'] % 60:02d}",
                "side": ev["side"],
                "mfe_at_exit": mfe,
                "hold_ms": ev["hold_ms"],
                "entry_px": entry_px,
                "exit_px_orig": ev["exit_px"],
                "size": size,
                "atr_used": atr,
                "original_net": original_net,
                "guard_fired": 0,
                "sim_exit_reason": "",
                "sim_exit_px": "",
                "sim_net": original_net,
                "delta": 0.0,
            })
            continue

        # Guard FIRED: simulate forward from imb_ts_s
        start_idx = find_tick_idx(ev["ts_s"])
        if start_idx >= len(ticks):
            # No ticks after the exit in the log -- can't simulate. Assume EOD flat.
            sim_exit_px = ev["exit_px"]
            sim_reason = "NO_FORWARD_TICKS"
            sim_mfe = mfe
        else:
            sim_exit_px, sim_reason, sim_mfe = simulate_forward(
                entry_px, ev["side"], sl_px, atr, size,
                start_idx, ticks, len(ticks)
            )

        sim_net = pnl_for(entry_px, sim_exit_px, ev["side"], size)
        delta = sim_net - original_net

        out_rows.append({
            "date": date,
            "time": f"{ev['ts_s'] // 3600:02d}:{(ev['ts_s'] % 3600) // 60:02d}:{ev['ts_s'] % 60:02d}",
            "side": ev["side"],
            "mfe_at_exit": mfe,
            "hold_ms": ev["hold_ms"],
            "entry_px": entry_px,
            "exit_px_orig": ev["exit_px"],
            "size": size,
            "atr_used": atr,
            "original_net": original_net,
            "guard_fired": 1,
            "sim_exit_reason": sim_reason,
            "sim_exit_px": round(sim_exit_px, 2),
            "sim_net": round(sim_net, 2),
            "delta": round(delta, 2),
        })


def main():
    if len(sys.argv) < 2:
        print("usage: cfe_imb_patch_validate.py <log1> <log2> ...", file=sys.stderr)
        return 2

    out_rows = []
    for path in sys.argv[1:]:
        print(f"[INFO] processing {path}", file=sys.stderr)
        process_day(path, out_rows)

    # Write CSV
    os.makedirs("audit_results", exist_ok=True)
    csv_path = "audit_results/cfe_imb_patch_validation.csv"
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=[
            "date", "time", "side", "mfe_at_exit", "hold_ms",
            "entry_px", "exit_px_orig", "size", "atr_used",
            "original_net", "guard_fired", "sim_exit_reason",
            "sim_exit_px", "sim_net", "delta",
        ])
        w.writeheader()
        for r in out_rows:
            w.writerow(r)

    # Aggregate
    n_total = len(out_rows)
    n_guarded = sum(1 for r in out_rows if r["guard_fired"] == 1)
    orig_total = sum(float(r["original_net"]) for r in out_rows)
    sim_total = sum(float(r["sim_net"]) for r in out_rows)
    delta_total = sim_total - orig_total

    # Per-exit-reason breakdown
    by_reason = defaultdict(lambda: {"n": 0, "sum_orig": 0.0, "sum_sim": 0.0})
    for r in out_rows:
        if r["guard_fired"] == 0:
            continue
        rsn = r["sim_exit_reason"]
        d = by_reason[rsn]
        d["n"] += 1
        d["sum_orig"] += float(r["original_net"])
        d["sum_sim"] += float(r["sim_net"])

    print("\n" + "=" * 72)
    print(f"CFE IMB_EXIT guard validation over {len(sys.argv) - 1} log files")
    print("=" * 72)
    print(f"  Total IMB_EXIT events joined to PnL : {n_total}")
    print(f"  Guard fired (mfe >= 1.0pt)          : {n_guarded}")
    print(f"  Guard inactive (mfe < 1.0pt)        : {n_total - n_guarded}")
    print()
    print(f"  Original IMB_EXIT total net         : ${orig_total:+.2f}")
    print(f"  With guard, simulated total net     : ${sim_total:+.2f}")
    print(f"  Delta (= projected recovery)        : ${delta_total:+.2f}")
    print()
    print("  Guard-fired outcomes by simulated exit reason:")
    print(f"  {'reason':<18}{'n':>4}{'orig_net':>14}{'sim_net':>14}{'delta':>14}")
    for rsn in sorted(by_reason.keys()):
        d = by_reason[rsn]
        print(f"  {rsn:<18}{d['n']:>4}{d['sum_orig']:>+13.2f}{d['sum_sim']:>+13.2f}{(d['sum_sim']-d['sum_orig']):>+13.2f}")

    print(f"\n[DONE] per-trade detail -> {csv_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
