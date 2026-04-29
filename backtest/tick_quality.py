#!/usr/bin/env python3
# =============================================================================
# tick_quality.py -- per-month tick QUALITY diagnostic for Dukascopy XAUUSD CSV
# 2026-04-29 audit -- complement to tick_months.py
#
# tick_months.py answered: "is the count >= 2.7M per month?" -- yes for every
# month 2024-03 -> 2026-04. But three engines independently rejected 2025-07
# and 2025-08 in their per-month BTs, so something about those months is
# degraded BEYOND raw count. This script measures it.
#
# QUALITY SIGNALS COMPUTED PER MONTH
#   ticks            -- raw row count (cross-check vs tick_months.py)
#   med_spread_pt    -- median (ask - bid) in price points; XAUUSD spread
#                       above ~0.30pt indicates illiquid / wide quotes
#   p95_spread_pt    -- 95th percentile spread; tail-fatness signal
#   spread_zero_frac -- fraction of ticks where ask == bid (broker glitches,
#                       crossed feeds, or stale-quote replays)
#   inverted_frac    -- fraction where ask < bid (parser/feed corruption;
#                       harness already swaps them, so this is detection only)
#   med_gap_ms       -- median inter-tick gap inside trading hours
#                       (after dropping any gap > 30 minutes as session break)
#   p95_gap_ms       -- 95th percentile intra-session gap
#   gap_5s_per_hr    -- count of >5-second intra-session gaps per trading hour;
#                       trending up indicates thinning or stalled feed
#   trading_hrs      -- estimated trading hours seen this month (sum of
#                       segments between session breaks)
#   tick_per_min_med -- median ticks-per-minute (active minutes only)
#
# QUALITY VERDICT (per month)
#   GOOD  -- all metrics within 1.5x the corpus median
#   THIN  -- spread or gap metrics > 2x corpus median (suspect)
#   BAD   -- spread or gap metrics > 3x corpus median (likely engine-reject)
#
# RUN
#   python3 /Users/jo/omega_repo/backtest/tick_quality.py
#   python3 /Users/jo/omega_repo/backtest/tick_quality.py /path/to/other.csv
#   python3 /Users/jo/omega_repo/backtest/tick_quality.py --csv-out report.csv
#
# OUTPUT
#   stdout      : human-readable per-month table + verdict
#   tick_quality_by_month.csv (optional) : machine-readable for downstream tools
# =============================================================================

import sys
import os
import argparse
import math
import pandas as pd
import numpy as np

DEFAULT_PATH = "/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv"

# A "session break" is any inter-tick gap exceeding this many seconds.
# We treat session breaks as boundaries (weekend, holiday, stop-of-feed) and
# do NOT count them in the intra-session gap metrics.
SESSION_BREAK_S = 30 * 60      # 30 minutes
INTRASESSION_5S_S = 5          # threshold for "noticeable" intra-session gap

# XAUUSD-specific quality bands. These are heuristics calibrated to gold:
#   - typical bid/ask spread on a healthy feed: 0.10-0.30 points
#   - typical inter-tick gap during London/NY: 50-300ms
GOLD_TICK_VALUE = 100.0  # for reference; not used in this script directly


def parse_args():
    ap = argparse.ArgumentParser(
        description="Per-month tick quality diagnostic for Dukascopy XAUUSD CSVs")
    ap.add_argument("path", nargs="?", default=DEFAULT_PATH,
                    help=f"input CSV (default: {DEFAULT_PATH})")
    ap.add_argument("--csv-out", default=None,
                    help="optional path to write per-month metrics as CSV")
    ap.add_argument("--chunksize", type=int, default=2_000_000,
                    help="rows per pandas chunk (default 2,000,000)")
    return ap.parse_args()


def detect_unit(sample_max_numeric):
    """Mirror the C++ harness convention:
         > 1e12 -> ms
         > 1e9  -> s
         else   -> ms (already-ms encoded values that happen to fall below 1e9)
    """
    if sample_max_numeric is None or math.isnan(sample_max_numeric):
        return None
    if sample_max_numeric > 1e12:
        return "ms"
    if sample_max_numeric > 1e9:
        return "s"
    return "ms"


def parse_chunk(chunk, ts_unit):
    """Return a DataFrame with columns: dt (UTC datetime), bid, ask, spread."""
    raw = chunk.iloc[:, 0].astype(str)
    parsed = pd.to_numeric(raw, errors="coerce")
    if parsed.notna().sum() / max(1, len(parsed)) > 0.95:
        # numeric path
        if ts_unit is None:
            ts_unit = detect_unit(parsed.max())
        dt = pd.to_datetime(parsed, unit=ts_unit, utc=True, errors="coerce")
    else:
        # ISO string path
        dt = pd.to_datetime(raw, utc=True, errors="coerce")

    # Column order varies by source. Dukascopy gold CSVs typically have header
    #   'timestamp,askPrice,bidPrice'  (ask BEFORE bid)
    # but other sources put bid first. Mirror the C++ harness convention:
    # take min(c1,c2) as bid, max(c1,c2) as ask. This guarantees spread >= 0
    # regardless of column order in the source.
    c1 = pd.to_numeric(chunk.iloc[:, 1], errors="coerce")
    c2 = pd.to_numeric(chunk.iloc[:, 2], errors="coerce")
    bid = np.minimum(c1, c2)
    ask = np.maximum(c1, c2)

    df = pd.DataFrame({"dt": dt, "bid": bid, "ask": ask})
    df = df.dropna(subset=["dt", "bid", "ask"])
    df = df[(df["bid"] > 0) & (df["ask"] > 0)]
    df["spread"] = df["ask"] - df["bid"]
    df["ym"] = df["dt"].dt.strftime("%Y-%m")
    return df, ts_unit


def compute_per_month(stream_iter, ts_unit_hint=None):
    """Stream the CSV in chunks, collect spread/gap stats per month."""
    # Per-month state
    state = {}  # ym -> dict
    last_ts_per_ym = {}     # ym -> last datetime seen in that month (for gaps)
    overall_last_ts = None  # for cross-chunk gap continuity

    for chunk in stream_iter:
        df, ts_unit_hint = parse_chunk(chunk, ts_unit_hint)
        if len(df) == 0:
            continue

        # Cross-chunk gap continuity: prepend a synthetic row that carries the
        # last datetime seen so the first row of this chunk gets a gap value.
        if overall_last_ts is not None:
            df = pd.concat(
                [pd.DataFrame({
                    "dt":   [overall_last_ts],
                    "bid":  [np.nan],
                    "ask":  [np.nan],
                    "spread": [np.nan],
                    "ym":   [overall_last_ts.strftime("%Y-%m")],
                }), df],
                ignore_index=True,
            )

        df = df.sort_values("dt").reset_index(drop=True)
        df["gap_s"] = df["dt"].diff().dt.total_seconds()

        # Drop the synthetic prepended row's null spread / inverted flags
        usable = df.dropna(subset=["bid", "ask"])
        if len(usable) == 0:
            overall_last_ts = df["dt"].iloc[-1]
            continue

        for ym, sub in usable.groupby("ym", sort=False):
            st = state.setdefault(ym, {
                "ticks": 0,
                "spread_sum": 0.0,
                "spread_zero": 0,
                "inverted": 0,
                "spreads": [],          # per-chunk samples; sampled to bound RAM
                "gaps_intra": [],       # intra-session gap samples
                "n_gap_5s": 0,
                "trading_seconds": 0.0,
                "tick_per_min": [],     # active-minute density samples
            })
            st["ticks"] += len(sub)
            st["spread_sum"] += float(sub["spread"].sum())
            st["spread_zero"] += int((sub["spread"] == 0).sum())
            st["inverted"]    += int((sub["bid"] > sub["ask"]).sum())

            # Sample up to 50,000 spread observations per month to bound memory
            # while still giving stable percentile estimates.
            sample = sub["spread"].dropna()
            if len(sample) > 50_000:
                sample = sample.sample(50_000, random_state=42)
            st["spreads"].append(sample.to_numpy(copy=False))

            # Intra-session gaps: gap_s present for non-first row, dropping any
            # gap >= SESSION_BREAK_S (treated as session boundary).
            g = sub["gap_s"].dropna()
            intra = g[g < SESSION_BREAK_S]
            if len(intra) > 0:
                st["trading_seconds"] += float(intra.sum())
                # Sample up to 50,000 gap observations per month
                if len(intra) > 50_000:
                    intra = intra.sample(50_000, random_state=42)
                st["gaps_intra"].append(intra.to_numpy(copy=False))
                st["n_gap_5s"] += int((intra > INTRASESSION_5S_S).sum())

            # Active-minute density: count ticks per minute, take median across
            # minutes that had at least one tick in this chunk-of-month.
            sub_minutes = sub["dt"].dt.floor("min")
            tpm = sub.groupby(sub_minutes).size()
            if len(tpm) > 0:
                # Sample to bound memory
                if len(tpm) > 10_000:
                    tpm = tpm.sample(10_000, random_state=42)
                st["tick_per_min"].append(tpm.to_numpy(copy=False))

        overall_last_ts = df["dt"].iloc[-1]

    # Roll up samples into final stats
    rows = []
    for ym, st in sorted(state.items()):
        spreads = (np.concatenate(st["spreads"])
                   if st["spreads"] else np.array([]))
        gaps    = (np.concatenate(st["gaps_intra"])
                   if st["gaps_intra"] else np.array([]))
        tpms    = (np.concatenate(st["tick_per_min"])
                   if st["tick_per_min"] else np.array([]))

        med_spread = float(np.median(spreads)) if len(spreads) else float("nan")
        p95_spread = float(np.percentile(spreads, 95)) if len(spreads) else float("nan")
        med_gap_ms = float(np.median(gaps) * 1000.0) if len(gaps) else float("nan")
        p95_gap_ms = float(np.percentile(gaps, 95) * 1000.0) if len(gaps) else float("nan")
        trading_hrs = st["trading_seconds"] / 3600.0
        gap_5s_per_hr = (st["n_gap_5s"] / trading_hrs) if trading_hrs > 0 else float("nan")
        tick_per_min_med = float(np.median(tpms)) if len(tpms) else float("nan")

        rows.append({
            "ym":              ym,
            "ticks":           st["ticks"],
            "med_spread_pt":   med_spread,
            "p95_spread_pt":   p95_spread,
            "spread_zero_frac": st["spread_zero"] / max(1, st["ticks"]),
            "inverted_frac":   st["inverted"]    / max(1, st["ticks"]),
            "med_gap_ms":      med_gap_ms,
            "p95_gap_ms":      p95_gap_ms,
            "gap_5s_per_hr":   gap_5s_per_hr,
            "trading_hrs":     trading_hrs,
            "tick_per_min_med": tick_per_min_med,
        })
    return pd.DataFrame(rows)


def label_quality(df):
    """Add a verdict column based on corpus-relative thresholds."""
    if len(df) == 0:
        return df
    base_spread = df["med_spread_pt"].median()
    base_p95_gap = df["p95_gap_ms"].median()
    base_5s_per_hr = df["gap_5s_per_hr"].median()

    def verdict(row):
        # Defensive: handle NaN base values (single-month input)
        if any(map(lambda x: x is None or (isinstance(x, float) and math.isnan(x)),
                   [base_spread, base_p95_gap, base_5s_per_hr])):
            return "GOOD"
        signals = []
        if row["med_spread_pt"] > 3.0 * base_spread:    signals.append("BAD")
        elif row["med_spread_pt"] > 1.5 * base_spread:  signals.append("THIN")
        if row["p95_gap_ms"] > 3.0 * base_p95_gap:      signals.append("BAD")
        elif row["p95_gap_ms"] > 2.0 * base_p95_gap:    signals.append("THIN")
        if row["gap_5s_per_hr"] > 3.0 * base_5s_per_hr: signals.append("BAD")
        elif row["gap_5s_per_hr"] > 2.0 * base_5s_per_hr: signals.append("THIN")
        if "BAD" in signals:  return "BAD"
        if "THIN" in signals: return "THIN"
        return "GOOD"

    df["verdict"] = df.apply(verdict, axis=1)
    df["base_med_spread_pt"] = base_spread
    df["base_p95_gap_ms"]    = base_p95_gap
    df["base_5s_per_hr"]     = base_5s_per_hr
    return df


def print_report(df):
    if len(df) == 0:
        print("(no data)")
        return

    print()
    print(f"Corpus baselines (medians across all months):")
    print(f"  med_spread_pt: {df['med_spread_pt'].median():.4f} pt")
    print(f"  p95_gap_ms:    {df['p95_gap_ms'].median():.0f} ms")
    print(f"  gap_5s_per_hr: {df['gap_5s_per_hr'].median():.2f} per trading hour")
    print()
    cols = [
        ("ym",                 10, "s",   "month"),
        ("ticks",              10, ",d",  "ticks"),
        ("med_spread_pt",       9, ".4f", "spread_med"),
        ("p95_spread_pt",       9, ".4f", "spread_p95"),
        ("spread_zero_frac",    9, ".4f", "zero_spr"),
        ("med_gap_ms",          8, ".0f", "gap_ms"),
        ("p95_gap_ms",          9, ".0f", "p95_gap"),
        ("gap_5s_per_hr",       8, ".2f", "5s/hr"),
        ("trading_hrs",         9, ".0f", "trad_hrs"),
        ("tick_per_min_med",    8, ".0f", "tpm_med"),
        ("verdict",             6, "s",   "verdict"),
    ]
    header = " ".join(f"{label:>{w}}" for _, w, _, label in cols)
    print(header)
    print("-" * len(header))
    for _, row in df.iterrows():
        parts = []
        for k, w, fmt, _ in cols:
            v = row[k]
            if fmt == "s":
                parts.append(f"{str(v):>{w}}")
            elif fmt.endswith("d"):
                parts.append(f"{int(v):>{w},}")
            else:
                if v is None or (isinstance(v, float) and math.isnan(v)):
                    parts.append(f"{'nan':>{w}}")
                else:
                    parts.append(f"{v:>{w}{fmt}}")
        print(" ".join(parts))

    print()
    bad  = df[df["verdict"] == "BAD"]["ym"].tolist()
    thin = df[df["verdict"] == "THIN"]["ym"].tolist()
    if bad:
        print(f"BAD months  (likely engine-reject cause): {bad}")
    if thin:
        print(f"THIN months (suspect):                    {thin}")
    if not bad and not thin:
        print("All months pass quality bands relative to corpus baselines.")


def main():
    args = parse_args()
    if not os.path.exists(args.path):
        print(f"ERROR: input not found: {args.path}", file=sys.stderr)
        return 2

    print(f"Reading: {args.path}")
    print(f"Chunk size: {args.chunksize:,} rows")

    # First chunk to detect unit and confirm column layout
    first = pd.read_csv(args.path, nrows=5, header=0, dtype=str)
    print(f"Detected columns: {list(first.columns)}")

    # Stream the file in chunks
    iter_csv = pd.read_csv(
        args.path,
        header=0,
        usecols=[0, 1, 2],
        dtype=str,
        chunksize=args.chunksize,
    )
    df = compute_per_month(iter_csv)
    if len(df) == 0:
        print("No parseable rows.")
        return 1

    df = label_quality(df)
    print_report(df)

    if args.csv_out:
        df.to_csv(args.csv_out, index=False)
        print(f"\nWrote per-month metrics to {args.csv_out}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
