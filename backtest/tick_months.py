#!/usr/bin/env python3
# =============================================================================
# tick_months.py -- diagnostic tool for tick file integrity
# 2026-04-29 audit -- determines whether the May-Sep 2025 + March 2026 "missing
# months" in the CFE/MCE BTs are a source-data gap or an engine rejection.
#
# Reads only column 0 (timestamp) of the source CSV, buckets by year-month,
# and reports tick counts. Auto-detects unix-seconds vs unix-ms vs ISO string.
#
# Run:
#   python3 /Users/jo/omega_repo/backtest/tick_months.py
#
# Optional: pass a custom CSV path as the first argument.
#   python3 /Users/jo/omega_repo/backtest/tick_months.py /path/to/other.csv
#
# Output: rows / dtype / ts min-max / unit chosen / per-month tick counts.
# =============================================================================

import sys
import pandas as pd

DEFAULT_PATH = "/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv"

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PATH
    print(f"Reading: {path}")

    # Read only the first column. Don't assume it's numeric -- could be ISO string.
    df = pd.read_csv(path, usecols=[0], header=0, names=["raw"], dtype=str)
    n = len(df)
    print(f"Rows (excluding header): {n:,}")
    if n == 0:
        print("File is empty after header.")
        return

    sample = df["raw"].iloc[0]
    print(f"First-row timestamp sample: {sample!r}")

    # Try numeric first.
    parsed = pd.to_numeric(df["raw"], errors="coerce")
    n_numeric = parsed.notna().sum()
    if n_numeric / n > 0.95:
        print(f"Numeric parse: {n_numeric:,} of {n:,} rows -> using numeric")
        ts_max = parsed.max()
        # Matches the C++ parser convention in cfe_duka_bt.cpp / mce_duka_bt.cpp:
        #   > 1e12  -> epoch milliseconds
        #   > 1e9   -> epoch seconds
        # Unix-ms in 2024-2026 is ~1.7e12 to 1.8e12; unix-s same era is ~1.7e9 to 1.8e9.
        # Original threshold of 2e12 misclassified 2024-2026 ms as s -- bug fix 2026-04-29.
        if ts_max > 1e12:
            unit = "ms"
        elif ts_max > 1e9:
            unit = "s"
        else:
            unit = "ms"  # fallback for already-millisecond-encoded values below 1e9
        print(f"Unit chosen: {unit} (max raw = {ts_max:.0f})")
        df["dt"] = pd.to_datetime(parsed, unit=unit, utc=True)
    else:
        print("Numeric parse < 95% -- treating as ISO string")
        df["dt"] = pd.to_datetime(df["raw"], utc=True, errors="coerce")
        bad = df["dt"].isna().sum()
        if bad > 0:
            print(f"WARNING: {bad:,} rows failed timestamp parse")

    # Drop unparseable rows for the bucketing step
    df = df.dropna(subset=["dt"])
    if len(df) == 0:
        print("No parseable timestamps after coercion.")
        return

    df["ym"] = df["dt"].dt.strftime("%Y-%m")
    counts = df.groupby("ym").size().rename("ticks").reset_index()
    counts["pct"] = (counts["ticks"] / counts["ticks"].sum() * 100).round(2)

    print()
    print(f"Date range: {df['dt'].min()} -> {df['dt'].max()}")
    print(f"Total parseable ticks: {len(df):,}")
    print()
    print(f"{'month':<10} {'ticks':>14} {'pct':>8}")
    print("-" * 34)

    # Print every month from min to max so gaps are explicit (zero rows shown).
    start = df["dt"].min().to_period("M")
    end = df["dt"].max().to_period("M")
    period_range = pd.period_range(start, end, freq="M")
    expected_yms = [str(p) for p in period_range]
    counts_by_ym = dict(zip(counts["ym"], counts["ticks"]))
    total = counts["ticks"].sum()

    for ym in expected_yms:
        n_ticks = counts_by_ym.get(ym, 0)
        pct = (100.0 * n_ticks / total) if total else 0.0
        flag = "  <-- ZERO" if n_ticks == 0 else ""
        print(f"{ym:<10} {n_ticks:>14,} {pct:>7.2f}%{flag}")

    # Also print the raw groupby in case some months are present but tiny
    print()
    print("Months with fewer than 10,000 ticks (potential thin-data months):")
    thin = counts[counts["ticks"] < 10000]
    if len(thin) == 0:
        print("  (none)")
    else:
        for _, row in thin.iterrows():
            print(f"  {row['ym']}: {int(row['ticks']):,} ticks")

if __name__ == "__main__":
    main()
