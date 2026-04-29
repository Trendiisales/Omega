#!/usr/bin/env python3
# =============================================================================
# mce_diag.py -- post-mortem analysis of mce_duka_bt_trades.csv
# 2026-04-29 audit -- inputs to the MCE adaptivity discussion (Task #6)
#
# PURPOSE
#   Surface every dimension along which MCE's avg P&L per trade varies by
#   >= 2x corpus baseline. Those are the dimensions where adaptive thresholds
#   are most likely to pay off. Pure data-out: no recommendations are made
#   here, only the empirical breakdowns.
#
# IDIOM
#   Single CSV read. All aggregations are vectorised pandas/numpy -- zero
#   per-row Python loops. Memory is never a concern (MCE produced 345 trades
#   over 26 months) but the vectorised style would scale linearly to a 100x
#   larger trade file without changes.
#
# DIMENSIONS REPORTED
#   1. Per-month       -- n / WR / net / avg
#   2. Per-session     -- Asia (22:00-05:00 UTC, slot=6) vs other
#   3. Per-hour UTC    -- 24-bucket distribution
#   4. Per-DOW         -- Mon..Sun
#   5. Per-side        -- LONG vs SHORT
#   6. Per-exit-reason -- DOLLAR_STOP / SL_HIT / TRAIL_STOP / MAX_HOLD / etc.
#   7. MFE / MAE distributions per month (median, p75, p95)
#   8. Hold-time distribution per exit reason
#   9. Rolling-30-trade WR series  (regime-shift signal)
#  10. Adaptivity flagging        -- any dim with >2x avg-magnitude variance
#                                    AND each bucket >= 20 trades
#
# OPTIONAL OUTPUTS
#   --csv-rollup PATH  -- writes per-(dim, bucket) avg/trade rollup as a long
#                          CSV for downstream tools. Useful for plotting.
#
# RUN
#   python3 backtest/mce_diag.py
#   python3 backtest/mce_diag.py mce_duka_bt_trades.csv
#   python3 backtest/mce_diag.py mce_duka_bt_trades.csv --csv-rollup mce_rollup.csv
#
# CSV SCHEMA EXPECTED
#   id,symbol,side,exit_reason,entry,exit,sl,tp,size,gross_pnl,net_pnl,
#   mfe,mae,entry_ts,exit_ts
# =============================================================================

import argparse
import math
import os
import sys
import numpy as np
import pandas as pd

DEFAULT_PATH = "mce_duka_bt_trades.csv"

# Adaptivity flag thresholds. A dimension is "adaptive opportunity" if:
#   - every bucket has >= MIN_BUCKET_N trades
#   - max(bucket_avg) - min(bucket_avg) >= ADAPT_VAR_MULT * mean(|bucket_avg|)
MIN_BUCKET_N   = 20
ADAPT_VAR_MULT = 2.0


# ----------------------------------------------------------------------------
# IO + normalisation
# ----------------------------------------------------------------------------
def parse_args():
    ap = argparse.ArgumentParser(
        description="Post-mortem analysis of mce_duka_bt_trades.csv")
    ap.add_argument("path", nargs="?", default=DEFAULT_PATH,
                    help=f"input CSV (default: {DEFAULT_PATH})")
    ap.add_argument("--csv-rollup", default=None,
                    help="optional output: per-(dim,bucket) avg rollup CSV")
    return ap.parse_args()


def load_trades(path: str) -> pd.DataFrame:
    """Read the BT trades CSV and add derived time/session columns."""
    df = pd.read_csv(path)
    if len(df) == 0:
        return df

    # Coerce numerics
    for c in ["entry", "exit", "sl", "tp", "size",
              "gross_pnl", "net_pnl", "mfe", "mae",
              "entry_ts", "exit_ts"]:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")

    # Time bucket columns -- vectorised, no apply()
    entry_dt = pd.to_datetime(df["entry_ts"], unit="s", utc=True)
    exit_dt  = pd.to_datetime(df["exit_ts"],  unit="s", utc=True)
    df["entry_dt"]  = entry_dt
    df["exit_dt"]   = exit_dt
    df["ym"]        = entry_dt.dt.strftime("%Y-%m")
    df["hour_utc"]  = entry_dt.dt.hour.astype("int16")
    df["dow"]       = entry_dt.dt.dayofweek.astype("int8")  # 0=Mon
    df["session"]   = np.where(
        (df["hour_utc"] >= 22) | (df["hour_utc"] < 5),
        "Asia", "Other"
    )
    df["hold_s"]    = (df["exit_ts"] - df["entry_ts"]).clip(lower=0)
    df["win"]       = (df["net_pnl"] > 0).astype("int8")
    return df


# ----------------------------------------------------------------------------
# Vectorised bucket aggregation
# ----------------------------------------------------------------------------
def bucket_stats(df: pd.DataFrame, by) -> pd.DataFrame:
    """Group `df` by `by` and compute n / wins / WR% / net / avg / dd-proxy
    in a single vectorised pass (uses agg + named columns)."""
    if len(df) == 0:
        return pd.DataFrame(columns=["bucket", "n", "wins", "wr",
                                     "net", "avg", "med_mfe", "med_mae"])
    g = df.groupby(by, dropna=False, observed=True)
    out = g.agg(
        n       = ("net_pnl", "size"),
        wins    = ("win",     "sum"),
        net     = ("net_pnl", "sum"),
        avg     = ("net_pnl", "mean"),
        med_mfe = ("mfe",     "median"),
        med_mae = ("mae",     "median"),
    ).reset_index()
    out["wr"] = np.where(out["n"] > 0, 100.0 * out["wins"] / out["n"], 0.0)
    return out.rename(columns={by: "bucket"})


def hour_stats(df: pd.DataFrame) -> pd.DataFrame:
    """Per-hour stats, padded to all 24 buckets so gaps are explicit."""
    base = bucket_stats(df, "hour_utc")
    full = pd.DataFrame({"bucket": np.arange(24, dtype="int16")})
    return full.merge(base, on="bucket", how="left").fillna(
        {"n": 0, "wins": 0, "net": 0.0, "avg": 0.0,
         "wr": 0.0, "med_mfe": 0.0, "med_mae": 0.0})


def dow_stats(df: pd.DataFrame) -> pd.DataFrame:
    base = bucket_stats(df, "dow")
    full = pd.DataFrame({"bucket": np.arange(7, dtype="int8")})
    out = full.merge(base, on="bucket", how="left").fillna(
        {"n": 0, "wins": 0, "net": 0.0, "avg": 0.0,
         "wr": 0.0, "med_mfe": 0.0, "med_mae": 0.0})
    out["bucket"] = out["bucket"].map(
        {0: "Mon", 1: "Tue", 2: "Wed", 3: "Thu",
         4: "Fri", 5: "Sat", 6: "Sun"})
    return out


def hold_time_distribution(df: pd.DataFrame) -> pd.DataFrame:
    """Per-exit-reason hold-time percentiles. All-vectorised via groupby+quantile."""
    if len(df) == 0:
        return pd.DataFrame()
    q = (df.groupby("exit_reason")["hold_s"]
           .quantile([0.25, 0.5, 0.75, 0.95])
           .unstack(level=-1)
           .rename(columns={0.25: "p25", 0.5: "p50",
                            0.75: "p75", 0.95: "p95"}))
    n = df.groupby("exit_reason").size().rename("n")
    return q.join(n).reset_index().sort_values("n", ascending=False)


def rolling_wr(df: pd.DataFrame, window: int = 30) -> pd.DataFrame:
    """Rolling N-trade WR / avg series, in time order. Vectorised .rolling()."""
    if len(df) == 0:
        return pd.DataFrame()
    sorted_df = df.sort_values("entry_ts").reset_index(drop=True)
    roll = (sorted_df["win"]
              .rolling(window=window, min_periods=window)
              .mean() * 100.0)
    avg  = (sorted_df["net_pnl"]
              .rolling(window=window, min_periods=window)
              .mean())
    return pd.DataFrame({
        "trade_idx": sorted_df.index,
        "entry_dt":  sorted_df["entry_dt"],
        "ym":        sorted_df["ym"],
        f"wr_roll{window}":  roll,
        f"avg_roll{window}": avg,
    }).dropna()


# ----------------------------------------------------------------------------
# Adaptivity flagging
# ----------------------------------------------------------------------------
def adaptivity_flag(df: pd.DataFrame, dim_name: str,
                    stats: pd.DataFrame) -> dict:
    """Decide whether `dim_name`'s per-bucket avg shows enough variance to
    warrant adaptive thresholds. Returns a dict of metrics + a verdict."""
    valid = stats[stats["n"] >= MIN_BUCKET_N].copy()
    n_valid = len(valid)
    if n_valid < 2:
        return {"dim": dim_name, "n_buckets": n_valid,
                "spread_avg": float("nan"), "scale": float("nan"),
                "verdict": "INSUFFICIENT_DATA"}
    avg_min  = float(valid["avg"].min())
    avg_max  = float(valid["avg"].max())
    spread   = avg_max - avg_min
    # Scale = mean of |bucket avg|; protects against divide-by-zero
    scale    = float(np.mean(np.abs(valid["avg"])))
    ratio    = spread / scale if scale > 1e-9 else float("inf")
    verdict  = "ADAPTIVE_CANDIDATE" if ratio >= ADAPT_VAR_MULT else "FLAT"
    return {"dim": dim_name, "n_buckets": n_valid,
            "spread_avg": spread, "scale": scale,
            "ratio": ratio, "verdict": verdict}


# ----------------------------------------------------------------------------
# Pretty print
# ----------------------------------------------------------------------------
def _fmt(v, fmt):
    if v is None or (isinstance(v, float) and (math.isnan(v) or math.isinf(v))):
        return "nan"
    return f"{v:{fmt}}"


def print_section(title: str):
    print()
    print(f"=== {title} ===")


def print_bucket_table(df: pd.DataFrame, label: str, sort_col=None):
    if len(df) == 0:
        print(f"(no rows for {label})")
        return
    if sort_col and sort_col in df.columns:
        df = df.sort_values(sort_col)
    cols = [
        ("bucket",   12, "s",    "bucket"),
        ("n",         6, "d",    "n"),
        ("wr",        6, ".1f",  "WR%"),
        ("net",      10, ".2f",  "net"),
        ("avg",       8, ".3f",  "avg/tr"),
        ("med_mfe",   8, ".2f",  "med_mfe"),
        ("med_mae",   8, ".2f",  "med_mae"),
    ]
    header = " ".join(f"{lbl:>{w}}" for _, w, _, lbl in cols)
    print(header)
    print("-" * len(header))
    for _, row in df.iterrows():
        parts = []
        for col, w, fmt, _ in cols:
            v = row.get(col)
            if fmt == "s":
                parts.append(f"{str(v):>{w}}")
            elif fmt == "d":
                parts.append(f"{int(v):>{w}d}")
            else:
                parts.append(f"{_fmt(v, fmt):>{w}}")
        print(" ".join(parts))


def print_summary(df: pd.DataFrame):
    n = len(df)
    if n == 0:
        print("No trades in input.")
        return
    wins = int(df["win"].sum())
    net  = float(df["net_pnl"].sum())
    avg  = float(df["net_pnl"].mean())
    print(f"Trades        : {n}")
    print(f"Wins / WR     : {wins} ({100.0*wins/n:.1f}%)")
    print(f"Net P&L       : ${net:.2f}")
    print(f"Avg / trade   : ${avg:.3f}")
    print(f"Date range    : {df['entry_dt'].min()} -> {df['entry_dt'].max()}")
    print(f"Symbols       : {sorted(df['symbol'].unique().tolist())}")


# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------
def main() -> int:
    args = parse_args()
    if not os.path.exists(args.path):
        print(f"ERROR: input not found: {args.path}", file=sys.stderr)
        print("Run mce_duka_bt first to produce the trades CSV.",
              file=sys.stderr)
        return 2

    df = load_trades(args.path)

    print(f"Read {len(df):,} rows from {args.path}")
    print_section("Overall summary")
    print_summary(df)

    if len(df) == 0:
        return 1

    # Build all per-dim stats vectorised
    stats_month   = bucket_stats(df, "ym")
    stats_session = bucket_stats(df, "session")
    stats_hour    = hour_stats(df)
    stats_dow     = dow_stats(df)
    stats_side    = bucket_stats(df, "side")
    stats_exit    = bucket_stats(df, "exit_reason")
    holdtime      = hold_time_distribution(df)
    roll_wr       = rolling_wr(df, window=30)

    print_section("Per-month")
    print_bucket_table(stats_month, "month", sort_col="bucket")

    print_section("Per-session (Asia = 22:00-05:00 UTC)")
    print_bucket_table(stats_session, "session")

    print_section("Per-hour UTC")
    print_bucket_table(stats_hour, "hour", sort_col="bucket")

    print_section("Per-DOW")
    print_bucket_table(stats_dow, "dow")

    print_section("Per-side")
    print_bucket_table(stats_side, "side")

    print_section("Per-exit-reason")
    print_bucket_table(stats_exit, "exit_reason", sort_col="n")

    print_section("Hold-time percentiles by exit reason (seconds)")
    if len(holdtime):
        cols = ["exit_reason", "n", "p25", "p50", "p75", "p95"]
        widths = [16, 6, 8, 8, 8, 8]
        head = " ".join(f"{c:>{w}}" for c, w in zip(cols, widths))
        print(head)
        print("-" * len(head))
        for _, r in holdtime.iterrows():
            print(" ".join([
                f"{str(r['exit_reason']):>16}",
                f"{int(r['n']):>6d}",
                f"{r['p25']:>8.1f}",
                f"{r['p50']:>8.1f}",
                f"{r['p75']:>8.1f}",
                f"{r['p95']:>8.1f}",
            ]))

    print_section("Rolling 30-trade WR (last 10 windows)")
    if len(roll_wr):
        tail = roll_wr.tail(10)
        for _, r in tail.iterrows():
            print(f"  trade={int(r['trade_idx']):4d}  "
                  f"entry={r['entry_dt']}  "
                  f"WR30={r['wr_roll30']:5.1f}%  "
                  f"avg30=${r['avg_roll30']:+.3f}")

    # Adaptivity flagging
    print_section("Adaptivity candidates")
    flags = [
        adaptivity_flag(df, "month",       stats_month),
        adaptivity_flag(df, "session",     stats_session),
        adaptivity_flag(df, "hour_utc",    stats_hour),
        adaptivity_flag(df, "dow",         stats_dow),
        adaptivity_flag(df, "side",        stats_side),
        adaptivity_flag(df, "exit_reason", stats_exit),
    ]
    print(f"{'dim':<14}{'n_buckets':>11}{'spread':>10}{'scale':>10}"
          f"{'ratio':>9}  verdict")
    print("-" * 64)
    for f in flags:
        print(
            f"{f['dim']:<14}"
            f"{f['n_buckets']:>11d}"
            f"{_fmt(f.get('spread_avg'), '.3f'):>10}"
            f"{_fmt(f.get('scale'),      '.3f'):>10}"
            f"{_fmt(f.get('ratio'),      '.2f'):>9}  "
            f"{f['verdict']}"
        )

    # Optional rollup CSV: long-form per-(dim, bucket) for plotting tools
    if args.csv_rollup:
        rollup_frames = []
        for dim_name, sdf in [
            ("month",       stats_month),
            ("session",     stats_session),
            ("hour_utc",    stats_hour),
            ("dow",         stats_dow),
            ("side",        stats_side),
            ("exit_reason", stats_exit),
        ]:
            if len(sdf) == 0:
                continue
            tmp = sdf.copy()
            tmp.insert(0, "dim", dim_name)
            tmp["bucket"] = tmp["bucket"].astype(str)
            rollup_frames.append(tmp[["dim", "bucket", "n", "wins",
                                      "wr", "net", "avg",
                                      "med_mfe", "med_mae"]])
        if rollup_frames:
            pd.concat(rollup_frames, ignore_index=True).to_csv(
                args.csv_rollup, index=False)
            print(f"\nWrote rollup to {args.csv_rollup}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
