#!/usr/bin/env python3
# =============================================================================
# disparity_post_mortem.py
#
# Layer 3 verification harness (HANDOFF_S26_PART1B_VERIFICATION_REBUILD.md §6.2):
# the cheapest, highest-signal sanity check against fictional engine profit.
#
# Joins Omega's per-trade close log against a cTrader history export and
# reports, for each matched trade:
#
#     engine_net_pnl  -  broker_net_pnl   ==  per-trade disparity
#
# A clean engine should produce sum(disparity) within commission+rounding of $0.
# A bleeding engine reporting fictional wins (the 2026-05-11 incident pattern)
# will show disparity == sum(engine_net) - sum(broker_net), i.e. exactly the
# fictional profit your dashboard is hallucinating.
#
# INPUTS:
#   --omega    Path to omega_trade_closes_YYYY-MM-DD.csv
#              (header: trade_id,trade_ref,entry_ts_unix,...,net_pnl,...)
#   --ctrader  Path to cTrader history CSV export
#              (column names auto-detected; supports the standard cTrader
#               "Account history" export format)
#   --window   Seconds tolerance for matching entry timestamps (default 5)
#   --out      Optional output CSV summarising matched pairs and disparity
#
# OUTPUT:
#   Human-readable summary on stdout + optional CSV.
#   Exit 0 if matched, non-zero if no matches or schema problem.
#
# Usage:
#   ./scripts/disparity_post_mortem.py \
#       --omega  /c/Omega/logs/trades/omega_trade_closes_2026-05-11.csv \
#       --ctrader ~/Downloads/cTrader_history_2026-05-11.csv \
#       --out /c/Omega/logs/disparity_2026-05-11.csv
#
# DEPENDENCIES: stdlib only (csv, argparse, sys, datetime, statistics).
# Runs on any Python 3.8+. No pandas, no third-party deps -- can ship to the
# VPS without setup.
# =============================================================================

from __future__ import annotations

import argparse
import csv
import datetime as dt
import statistics
import sys
from pathlib import Path


# ---------- cTrader export schema detection ---------------------------------
# cTrader's "Account History" export has varied over the years. We detect the
# entry-time / side / volume / net columns by name patterns rather than fixed
# index so this works against the standard BlackBull-cTrader export AND any
# minor reshuffle. Lowercased exact match preferred; substring fallback.

CTRADER_ENTRY_TIME_COLS = [
    "entry time", "opening time", "open time", "time", "entry timestamp",
]
CTRADER_CLOSE_TIME_COLS = [
    "closing time", "close time", "exit time", "exit timestamp",
]
CTRADER_SIDE_COLS = [
    "direction", "side", "type", "trade side",
]
CTRADER_VOLUME_COLS = [
    "volume", "quantity", "lots", "size", "filled volume",
]
CTRADER_ENTRY_PX_COLS = [
    "entry price", "opening price", "open price",
]
CTRADER_CLOSE_PX_COLS = [
    "closing price", "close price", "exit price",
]
CTRADER_NET_COLS = [
    "net usd", "net", "profit", "p&l", "pnl", "net pnl", "balance change",
]
CTRADER_SYMBOL_COLS = [
    "symbol", "instrument",
]


def find_col(header: list[str], candidates: list[str]) -> str | None:
    """Return the first header field whose lowercase form matches a candidate
    (exact then substring). Returns None if nothing matches."""
    low = {h.lower().strip(): h for h in header}
    for c in candidates:
        if c in low:
            return low[c]
    for c in candidates:
        for k, orig in low.items():
            if c in k:
                return orig
    return None


def parse_ctrader_time(raw: str) -> int | None:
    """Parse cTrader-style timestamp to unix seconds. Tries a handful of
    common formats. Returns None on failure."""
    raw = (raw or "").strip()
    if not raw:
        return None
    fmts = [
        "%d/%m/%Y %H:%M:%S.%f",
        "%d/%m/%Y %H:%M:%S",
        "%Y-%m-%d %H:%M:%S.%f",
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%dT%H:%M:%S.%fZ",
        "%Y-%m-%dT%H:%M:%SZ",
        "%m/%d/%Y %H:%M:%S",
    ]
    for f in fmts:
        try:
            # cTrader exports are typically in account-local time. The user
            # may need to add a --tz-offset arg later; for now assume the
            # caller has produced a UTC export OR the offset is constant
            # within the file (so disparity timing matches consistently).
            return int(dt.datetime.strptime(raw, f).timestamp())
        except ValueError:
            continue
    return None


def normalise_side(raw: str) -> str:
    """Map cTrader side strings to LONG/SHORT to match Omega's convention."""
    s = (raw or "").strip().lower()
    if s in ("buy", "long"):
        return "LONG"
    if s in ("sell", "short"):
        return "SHORT"
    return s.upper()


def parse_number(raw: str) -> float | None:
    """Parse a float possibly containing currency symbols, thousands separators,
    or NZ$/USD prefix. Returns None on failure."""
    if raw is None:
        return None
    s = str(raw).strip().replace(",", "").replace("$", "").replace("US", "")
    s = s.replace("NZ", "").replace("AUD", "").strip()
    if not s:
        return None
    # Some exports use parentheses for negatives: "(0.45)"
    if s.startswith("(") and s.endswith(")"):
        s = "-" + s[1:-1]
    try:
        return float(s)
    except ValueError:
        return None


# ---------- Omega CSV loader -------------------------------------------------

def load_omega(path: Path) -> list[dict]:
    """Load omega_trade_closes_*.csv into a list of normalised dicts."""
    out = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            return out
        # The Omega header is fixed; we still tolerate minor variation.
        required = ["entry_ts_unix", "symbol", "side", "size", "net_pnl"]
        for col in required:
            if col not in reader.fieldnames:
                print(f"[FATAL] Omega CSV missing required column '{col}'. "
                      f"Header was: {reader.fieldnames}", file=sys.stderr)
                sys.exit(2)
        for row in reader:
            try:
                ts = int(float(row["entry_ts_unix"]))
            except (TypeError, ValueError):
                continue
            out.append({
                "ts":     ts,
                "symbol": row.get("symbol", "").strip(),
                "side":   row.get("side", "").strip().upper(),
                "size":   parse_number(row.get("size")),
                "engine": row.get("engine", "").strip(),
                "entry_px":   parse_number(row.get("entry_px")),
                "exit_px":    parse_number(row.get("exit_px")),
                "gross_pnl":  parse_number(row.get("gross_pnl")),
                "net_pnl":    parse_number(row.get("net_pnl")),
                "exit_reason": row.get("exit_reason", "").strip(),
            })
    return out


# ---------- cTrader CSV loader ----------------------------------------------

def load_ctrader(path: Path) -> list[dict]:
    """Load cTrader history export with column auto-detection."""
    out = []
    with open(path, newline="") as f:
        reader = csv.reader(f)
        rows = list(reader)
    if not rows:
        return out

    # Find the actual header row -- cTrader exports often have an account-info
    # prelude of 1-5 rows before the table header. Treat the first row with
    # >= 4 columns containing at least one of our candidate names as header.
    header_idx = -1
    for i, r in enumerate(rows):
        if len(r) >= 4:
            joined = " ".join(c.lower() for c in r)
            if any(k in joined for k in ("time", "direction", "volume", "profit")):
                header_idx = i
                break
    if header_idx < 0:
        print("[FATAL] could not locate cTrader header row.", file=sys.stderr)
        sys.exit(2)

    header = rows[header_idx]
    c_time   = find_col(header, CTRADER_ENTRY_TIME_COLS)
    c_close_time = find_col(header, CTRADER_CLOSE_TIME_COLS)
    c_side   = find_col(header, CTRADER_SIDE_COLS)
    c_vol    = find_col(header, CTRADER_VOLUME_COLS)
    c_entry  = find_col(header, CTRADER_ENTRY_PX_COLS)
    c_close  = find_col(header, CTRADER_CLOSE_PX_COLS)
    c_net    = find_col(header, CTRADER_NET_COLS)
    c_sym    = find_col(header, CTRADER_SYMBOL_COLS)

    missing = []
    for label, col in [("entry_time", c_time), ("side", c_side),
                       ("volume", c_vol), ("net", c_net)]:
        if not col:
            missing.append(label)
    if missing:
        print(f"[FATAL] cTrader export missing required columns: {missing}. "
              f"Header was: {header}", file=sys.stderr)
        sys.exit(2)

    idx_time  = header.index(c_time)
    idx_close_time = header.index(c_close_time) if c_close_time else -1
    idx_side  = header.index(c_side)
    idx_vol   = header.index(c_vol)
    idx_entry = header.index(c_entry) if c_entry else -1
    idx_close = header.index(c_close) if c_close else -1
    idx_net   = header.index(c_net)
    idx_sym   = header.index(c_sym) if c_sym else -1

    for r in rows[header_idx + 1:]:
        if len(r) <= idx_time:
            continue
        ts = parse_ctrader_time(r[idx_time])
        if ts is None:
            continue
        out.append({
            "ts":      ts,
            "ts_close": parse_ctrader_time(r[idx_close_time]) if idx_close_time >= 0 else None,
            "symbol":  r[idx_sym].strip() if idx_sym >= 0 else "",
            "side":    normalise_side(r[idx_side]),
            "size":    parse_number(r[idx_vol]),
            "entry_px": parse_number(r[idx_entry]) if idx_entry >= 0 else None,
            "close_px": parse_number(r[idx_close]) if idx_close >= 0 else None,
            "broker_net": parse_number(r[idx_net]),
        })
    return out


# ---------- Joiner -----------------------------------------------------------

def match(omega_rows: list[dict], ctrader_rows: list[dict],
          window_sec: int) -> tuple[list[dict], list[dict], list[dict]]:
    """Match each Omega close to a cTrader trade by (side, ts ± window).
    Volume match is best-effort (some cTrader exports normalise lots to units).
    Returns (matched, omega_unmatched, ctrader_unmatched).
    Each cTrader trade can only match one Omega trade (closest ts wins).
    """
    used_ctrader = [False] * len(ctrader_rows)
    matched: list[dict] = []
    omega_unmatched: list[dict] = []

    # Sort ctrader by ts so we can binary-search-ish (linear is fine at <10k).
    ctrader_idx = sorted(range(len(ctrader_rows)),
                         key=lambda i: ctrader_rows[i]["ts"])

    for o in omega_rows:
        best_i = -1
        best_dt = window_sec + 1
        for i in ctrader_idx:
            if used_ctrader[i]:
                continue
            c = ctrader_rows[i]
            if c["side"] != o["side"]:
                continue
            d = abs(c["ts"] - o["ts"])
            if d <= window_sec and d < best_dt:
                best_dt = d
                best_i = i
        if best_i >= 0:
            used_ctrader[best_i] = True
            c = ctrader_rows[best_i]
            matched.append({
                "ts":         o["ts"],
                "side":       o["side"],
                "size":       o["size"],
                "engine":     o["engine"],
                "symbol":     o["symbol"],
                "engine_net": o["net_pnl"],
                "broker_net": c["broker_net"],
                "disparity":  (o["net_pnl"] or 0.0) - (c["broker_net"] or 0.0),
                "dt_sec":     best_dt,
                "omega_exit_px":   o["exit_px"],
                "broker_close_px": c["close_px"],
                "exit_reason":     o["exit_reason"],
            })
        else:
            omega_unmatched.append(o)

    ctrader_unmatched = [c for i, c in enumerate(ctrader_rows) if not used_ctrader[i]]
    return matched, omega_unmatched, ctrader_unmatched


# ---------- Summary printing -------------------------------------------------

def summarise(matched: list[dict], om_un: list[dict], ct_un: list[dict]) -> int:
    if not matched:
        print("[FATAL] no trades matched between Omega and cTrader. "
              "Check timestamps, side mapping, and that both files cover "
              "the same window.")
        return 3

    eng_total = sum((m["engine_net"] or 0.0) for m in matched)
    bkr_total = sum((m["broker_net"] or 0.0) for m in matched)
    disp_total = eng_total - bkr_total
    disps = [m["disparity"] for m in matched]
    disp_mean = statistics.fmean(disps)
    disp_median = statistics.median(disps)
    disp_stdev = statistics.pstdev(disps) if len(disps) > 1 else 0.0

    n_eng_win = sum(1 for m in matched if (m["engine_net"] or 0) > 0)
    n_eng_los = sum(1 for m in matched if (m["engine_net"] or 0) < 0)
    n_bkr_win = sum(1 for m in matched if (m["broker_net"] or 0) > 0)
    n_bkr_los = sum(1 for m in matched if (m["broker_net"] or 0) < 0)

    print("=" * 70)
    print(f"DISPARITY POST-MORTEM  ({len(matched)} matched, "
          f"{len(om_un)} omega-only, {len(ct_un)} ctrader-only)")
    print("=" * 70)
    print(f"  engine total net   :  {eng_total:+10.2f}")
    print(f"  broker total net   :  {bkr_total:+10.2f}")
    print(f"  disparity (eng-bk) :  {disp_total:+10.2f}  "
          f"<-- fictional dashboard profit if > 0")
    print()
    print(f"  per-trade disparity:  mean   {disp_mean:+8.4f}")
    print(f"                        median {disp_median:+8.4f}")
    print(f"                        stdev  {disp_stdev:8.4f}")
    print()
    print(f"  engine W/L         :  {n_eng_win:3d} / {n_eng_los:3d}   "
          f"  (real on broker: {n_bkr_win:3d} / {n_bkr_los:3d})")
    if n_eng_win > 0 and n_bkr_win < n_eng_win:
        fictional = n_eng_win - n_bkr_win
        print(f"  WARNING: {fictional} trade(s) reported as wins by engine were "
              f"losses or did not exist at broker.")
    print()
    print("WORST 5 PER-TRADE DISPARITIES:")
    print(f"  {'ts':>12s}  side  {'engine':>9s}  {'broker':>9s}  {'disp':>9s}  exit_reason")
    for m in sorted(matched, key=lambda x: x["disparity"], reverse=True)[:5]:
        print(f"  {m['ts']:12d}  {m['side']:<5s} "
              f"{(m['engine_net'] or 0):+9.4f}  "
              f"{(m['broker_net'] or 0):+9.4f}  "
              f"{m['disparity']:+9.4f}  "
              f"{m['exit_reason']}")
    if om_un:
        print()
        print(f"OMEGA-ONLY ({len(om_un)}): trades the engine recorded that have "
              f"no broker counterpart within the time window. These are the "
              f"DANGEROUS ones -- the engine thinks it traded but the broker "
              f"didn't. First 5:")
        for o in om_un[:5]:
            print(f"  ts={o['ts']} side={o['side']} engine_net={o['net_pnl']:+.4f} "
                  f"({o['engine']})")
    if ct_un:
        print()
        print(f"BROKER-ONLY ({len(ct_un)}): broker fills with no Omega close "
              f"record. These are also dangerous -- the broker traded but the "
              f"engine doesn't know. First 5:")
        for c in ct_un[:5]:
            sym = c.get("symbol") or "?"
            print(f"  ts={c['ts']} side={c['side']} sym={sym} "
                  f"broker_net={(c.get('broker_net') or 0):+.4f}")
    print()
    # Exit non-zero if disparity is suspiciously large.
    if abs(disp_total) > max(5.0, abs(bkr_total) * 0.5):
        print("VERDICT: disparity is large relative to broker net. The engine "
              "view is unreliable. Do NOT trade live until this is resolved.")
        return 1
    print("VERDICT: disparity within normal bounds (commission + rounding).")
    return 0


def write_matched_csv(out_path: Path, matched: list[dict]) -> None:
    cols = ["ts", "side", "size", "symbol", "engine",
            "engine_net", "broker_net", "disparity", "dt_sec",
            "omega_exit_px", "broker_close_px", "exit_reason"]
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for m in matched:
            w.writerow({k: m.get(k) for k in cols})


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--omega",   required=True, type=Path,
                   help="Path to omega_trade_closes_YYYY-MM-DD.csv")
    p.add_argument("--ctrader", required=True, type=Path,
                   help="Path to cTrader history CSV export")
    p.add_argument("--window",  type=int, default=5,
                   help="Seconds tolerance for matching entry timestamps "
                        "(default 5). Increase if account timezone offset "
                        "makes matches sparse.")
    p.add_argument("--out",     type=Path, default=None,
                   help="Optional output CSV of matched pairs + disparity")
    args = p.parse_args()

    if not args.omega.exists():
        print(f"[FATAL] omega CSV not found: {args.omega}", file=sys.stderr)
        return 2
    if not args.ctrader.exists():
        print(f"[FATAL] ctrader CSV not found: {args.ctrader}", file=sys.stderr)
        return 2

    omega_rows = load_omega(args.omega)
    ctrader_rows = load_ctrader(args.ctrader)
    print(f"[INFO] loaded {len(omega_rows)} omega trades, "
          f"{len(ctrader_rows)} ctrader trades")
    if not omega_rows:
        print("[FATAL] no Omega trades to compare.", file=sys.stderr)
        return 2
    if not ctrader_rows:
        print("[FATAL] no cTrader trades to compare.", file=sys.stderr)
        return 2

    matched, om_un, ct_un = match(omega_rows, ctrader_rows, args.window)
    rc = summarise(matched, om_un, ct_un)

    if args.out:
        write_matched_csv(args.out, matched)
        print(f"[INFO] wrote matched-pair CSV to {args.out}")

    return rc


if __name__ == "__main__":
    sys.exit(main())
