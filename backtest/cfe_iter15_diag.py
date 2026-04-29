#!/usr/bin/env python3
# =============================================================================
# cfe_iter15_diag.py -- per-MFE-bucket diagnostic for cfe_duka_bt_trades.csv
# 2026-04-29 audit -- inputs to the iter-15 0.5-1pt-bucket discussion (Task #7)
#
# PURPOSE
#   Reproduce and extend the MFE-bucket table from SESSION_HANDOFF_2026-04-29.md
#   (the "key insight" table) on whatever cfe_duka_bt_trades.csv currently
#   exists, then deep-dive on the 0.5-1pt bucket to inform the iter-15 fix:
#     A) tighten partial threshold so these trades bank earlier, OR
#     B) add a "no-progress at 90s" gate.
#
#   Strict data-out only: no recommendation rendered -- the numbers tell the
#   story.
#
# IDIOM
#   Single CSV read. All aggregations vectorised pandas/numpy, no per-row
#   Python loops. Per-parent rollup is a single groupby.
#
# CFE CSV NOTE -- PARTIALS COMPLICATE PARENT GROUPING
#   The cfe_duka_bt harness emits one CSV row per fill -- entry, partial(s)
#   and final -- so the file has more rows than parent trades (handoff:
#   905 rows / 647 parents). To match the handoff's "per-parent" semantics
#   we group by trade `id`, sum `net_pnl` (and partial banked counts as
#   wins / losses correctly), and take max(`mfe`) and min(`mae`) per id.
#   If the input CSV has unique ids per row (e.g. someone re-ran with
#   partials disabled) the script still works -- groupby of 1 is a no-op.
#
# OUTPUTS
#   stdout: handoff-style MFE bucket table + 0.5-1pt deep dive
#   --csv-rollup PATH (optional): per-bucket and per-dimension rollups
#
# RUN
#   python3 backtest/cfe_iter15_diag.py
#   python3 backtest/cfe_iter15_diag.py cfe_duka_bt_trades.csv
#   python3 backtest/cfe_iter15_diag.py cfe_duka_bt_trades.csv --csv-rollup cfe_rollup.csv
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

DEFAULT_PATH = "cfe_duka_bt_trades.csv"

# Handoff bucket boundaries (price points). MFE values are stored in $ in the
# CSV (after the harness applied tick mult of 100). Convert to points by /100.
MFE_BUCKETS_PT = [
    ("never (=0)",  0.00, 0.00),     # exactly 0
    ("0-0.5 pt",    0.00, 0.50),     # (0, 0.5]
    ("0.5-1 pt",    0.50, 1.00),     # (0.5, 1]
    ("1-2 pt",      1.00, 2.00),     # (1, 2]
    ("2-4 pt",      2.00, 4.00),     # (2, 4]
    (">4 pt",       4.00, 1e9),      # (4, inf)
]

# Hold-time bands for the 0.5-1pt deep dive (seconds)
HOLD_BANDS_S = [
    ("<= 30s",     0,    30),
    ("30-60s",     30,   60),
    ("60-90s",     60,   90),
    ("90-180s",    90,   180),
    ("180-600s",   180,  600),
    ("> 600s",     600,  1e9),
]


def parse_args():
    ap = argparse.ArgumentParser(
        description="Per-MFE-bucket post-mortem of cfe_duka_bt_trades.csv")
    ap.add_argument("path", nargs="?", default=DEFAULT_PATH,
                    help=f"input CSV (default: {DEFAULT_PATH})")
    ap.add_argument("--csv-rollup", default=None,
                    help="optional output rollup CSV")
    ap.add_argument("--mult", type=float, default=1.0,
                    help="MFE/MAE divisor to recover price points. Empirically "
                         "the cfe_duka_bt / mce_duka_bt CSV mfe column is "
                         "already in price points (despite the harness's "
                         "tr.mfe *= 100 -- the engine appears to store MFE in "
                         "1/100-pt units, so the multiply restores points). "
                         "Default 1.0 leaves values untouched. Override to 100 "
                         "if you regenerate trades with a different harness.")
    return ap.parse_args()


def load_and_rollup_to_parents(path: str, mult: float) -> pd.DataFrame:
    """Read CSV, group by trade id, aggregate to per-parent rows.

    Per-parent reductions:
      side, exit_reason, symbol -- first()      (parents share these)
      net_pnl, gross_pnl        -- sum()        (partials accumulate)
      mfe                       -- max()        (best favourable)
      mae                       -- min()        (worst adverse, mae <= 0)
      entry_ts                  -- min()
      exit_ts                   -- max()
      size                      -- mean()       (defensive)
    Then derives:
      hold_s, ym, hour_utc, dow, session, mfe_pt, mae_pt, win
    """
    df = pd.read_csv(path)
    if len(df) == 0:
        return df

    for c in ["entry", "exit", "sl", "tp", "size",
              "gross_pnl", "net_pnl", "mfe", "mae",
              "entry_ts", "exit_ts"]:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")

    n_rows    = len(df)
    n_parents = df["id"].nunique()
    print(f"Read {n_rows:,} rows / {n_parents:,} unique trade ids")

    if n_rows == n_parents:
        # Already per-parent. Skip the groupby for speed.
        parents = df.copy()
    else:
        # Single vectorised groupby + named agg
        parents = df.groupby("id", sort=False).agg(
            symbol      = ("symbol",      "first"),
            side        = ("side",        "first"),
            exit_reason = ("exit_reason", "last"),   # final close drives this
            entry       = ("entry",       "first"),
            exit_       = ("exit",        "last"),
            size        = ("size",        "mean"),
            gross_pnl   = ("gross_pnl",   "sum"),
            net_pnl     = ("net_pnl",     "sum"),
            mfe         = ("mfe",         "max"),
            mae         = ("mae",         "min"),
            entry_ts    = ("entry_ts",    "min"),
            exit_ts     = ("exit_ts",     "max"),
        ).reset_index().rename(columns={"exit_": "exit"})

    entry_dt = pd.to_datetime(parents["entry_ts"], unit="s", utc=True)
    parents["entry_dt"] = entry_dt
    parents["exit_dt"]  = pd.to_datetime(parents["exit_ts"], unit="s", utc=True)
    parents["ym"]       = entry_dt.dt.strftime("%Y-%m")
    parents["hour_utc"] = entry_dt.dt.hour.astype("int16")
    parents["dow"]      = entry_dt.dt.dayofweek.astype("int8")
    parents["session"]  = np.where(
        (parents["hour_utc"] >= 22) | (parents["hour_utc"] < 5),
        "Asia", "Other"
    )
    parents["hold_s"]   = (parents["exit_ts"] - parents["entry_ts"]).clip(lower=0)
    parents["mfe_pt"]   = parents["mfe"] / mult
    parents["mae_pt"]   = parents["mae"] / mult
    parents["win"]      = (parents["net_pnl"] > 0).astype("int8")
    return parents


# ----------------------------------------------------------------------------
# MFE bucket assignment -- vectorised (np.select / pd.cut hybrid)
# ----------------------------------------------------------------------------
def assign_mfe_bucket(parents: pd.DataFrame) -> pd.DataFrame:
    """Add an `mfe_bucket` categorical column. Vectorised np.select."""
    mfe = parents["mfe_pt"].fillna(0.0).to_numpy()
    conds = []
    labels = []
    for label, lo, hi in MFE_BUCKETS_PT:
        if label == "never (=0)":
            conds.append(mfe == 0.0)
        else:
            conds.append((mfe > lo) & (mfe <= hi))
        labels.append(label)
    bucket = np.select(conds, labels, default="UNCATEGORIZED")
    parents = parents.copy()
    parents["mfe_bucket"] = pd.Categorical(
        bucket, categories=[lbl for lbl, _, _ in MFE_BUCKETS_PT], ordered=True)
    return parents


def assign_hold_band(parents: pd.DataFrame) -> pd.DataFrame:
    hold = parents["hold_s"].fillna(0.0).to_numpy()
    conds  = [(hold > lo) & (hold <= hi) for _, lo, hi in HOLD_BANDS_S]
    labels = [lbl for lbl, _, _ in HOLD_BANDS_S]
    band   = np.select(conds, labels, default="?")
    parents = parents.copy()
    parents["hold_band"] = pd.Categorical(
        band, categories=labels, ordered=True)
    return parents


# ----------------------------------------------------------------------------
# Bucket stats -- single vectorised groupby
# ----------------------------------------------------------------------------
def bucket_stats(parents: pd.DataFrame, by) -> pd.DataFrame:
    if len(parents) == 0:
        return pd.DataFrame()
    g = parents.groupby(by, dropna=False, observed=True)
    out = g.agg(
        n         = ("net_pnl",  "size"),
        wins      = ("win",      "sum"),
        net       = ("net_pnl",  "sum"),
        avg       = ("net_pnl",  "mean"),
        med_mfe_pt= ("mfe_pt",   "median"),
        med_mae_pt= ("mae_pt",   "median"),
        med_hold  = ("hold_s",   "median"),
    ).reset_index()
    out["wr"] = np.where(out["n"] > 0, 100.0 * out["wins"] / out["n"], 0.0)
    return out.rename(columns={by: "bucket"})


# ----------------------------------------------------------------------------
# Print helpers
# ----------------------------------------------------------------------------
def _fmt(v, fmt):
    if v is None or (isinstance(v, float) and (math.isnan(v) or math.isinf(v))):
        return "nan"
    return f"{v:{fmt}}"


def print_section(title: str):
    print()
    print(f"=== {title} ===")


def print_bucket_table(df: pd.DataFrame, label: str = "bucket"):
    if len(df) == 0:
        print("(empty)")
        return
    cols = [
        ("bucket",     14, "s",   label),
        ("n",           5, "d",   "n"),
        ("wr",          6, ".1f", "WR%"),
        ("net",        10, ".2f", "net"),
        ("avg",         9, ".3f", "avg/parent"),
        ("med_mfe_pt",  8, ".2f", "med_mfe"),
        ("med_mae_pt",  8, ".2f", "med_mae"),
        ("med_hold",    8, ".0f", "med_hold"),
    ]
    header = " ".join(f"{lbl:>{w}}" for _, w, _, lbl in cols)
    print(header)
    print("-" * len(header))
    for _, r in df.iterrows():
        parts = []
        for col, w, fmt, _ in cols:
            v = r.get(col)
            if fmt == "s":
                parts.append(f"{str(v):>{w}}")
            elif fmt == "d":
                parts.append(f"{int(v):>{w}d}")
            else:
                parts.append(f"{_fmt(v, fmt):>{w}}")
        print(" ".join(parts))


# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------
def main() -> int:
    args = parse_args()
    if not os.path.exists(args.path):
        print(f"ERROR: input not found: {args.path}", file=sys.stderr)
        print("Run cfe_duka_bt first to produce the trades CSV.",
              file=sys.stderr)
        return 2

    parents = load_and_rollup_to_parents(args.path, args.mult)
    if len(parents) == 0:
        print("(no parents after rollup)")
        return 1

    parents = assign_mfe_bucket(parents)
    parents = assign_hold_band(parents)

    # Overall summary
    n      = len(parents)
    wins   = int(parents["win"].sum())
    net    = float(parents["net_pnl"].sum())
    avg    = float(parents["net_pnl"].mean())
    med_h  = float(parents["hold_s"].median())
    print_section("Overall (per-parent)")
    print(f"Parents       : {n}")
    print(f"Wins / WR     : {wins} ({100.0*wins/n:.1f}%)")
    print(f"Net P&L       : ${net:+.2f}")
    print(f"Avg / parent  : ${avg:+.3f}")
    print(f"Median hold   : {med_h:.0f} s")
    print(f"Date range    : {parents['entry_dt'].min()} -> {parents['entry_dt'].max()}")

    # Headline: handoff-style MFE bucket table
    print_section("MFE bucket breakdown (handoff schema)")
    bb = bucket_stats(parents, "mfe_bucket")
    # Keep MFE_BUCKETS_PT order
    order = pd.Categorical(bb["bucket"],
                           categories=[lbl for lbl, _, _ in MFE_BUCKETS_PT],
                           ordered=True)
    bb = bb.assign(bucket=order).sort_values("bucket")
    print_bucket_table(bb, label="mfe_bucket")

    # 0.5-1pt deep dive
    target = parents[parents["mfe_bucket"] == "0.5-1 pt"].copy()
    print_section(f"DEEP DIVE: 0.5-1pt MFE bucket  (n={len(target)})")
    if len(target) == 0:
        print("(no parents in this bucket)")
    else:
        # Per-month
        print_section("  0.5-1pt: per-month")
        print_bucket_table(bucket_stats(target, "ym").sort_values("bucket"),
                           label="month")

        # Per-session
        print_section("  0.5-1pt: per-session")
        print_bucket_table(bucket_stats(target, "session"), label="session")

        # Per-side
        print_section("  0.5-1pt: per-side")
        print_bucket_table(bucket_stats(target, "side"), label="side")

        # Per-exit-reason
        print_section("  0.5-1pt: per-exit-reason")
        s_exit = bucket_stats(target, "exit_reason").sort_values("n", ascending=False)
        print_bucket_table(s_exit, label="exit_reason")

        # Per-hold-band -- informs the "no-progress at 90s" gate decision
        print_section("  0.5-1pt: per-hold-band  (informs 90s-gate question)")
        s_hb = bucket_stats(target, "hold_band")
        order = pd.Categorical(s_hb["bucket"],
                               categories=[lbl for lbl, _, _ in HOLD_BANDS_S],
                               ordered=True)
        s_hb = s_hb.assign(bucket=order).sort_values("bucket")
        print_bucket_table(s_hb, label="hold_band")

        # MFE / MAE distribution table (percentiles) -- informs the
        # "tighten partial threshold" decision
        print_section("  0.5-1pt: MFE/MAE/hold percentile distribution")
        pct = pd.DataFrame({
            "p10": target[["mfe_pt", "mae_pt", "hold_s"]].quantile(0.10),
            "p25": target[["mfe_pt", "mae_pt", "hold_s"]].quantile(0.25),
            "p50": target[["mfe_pt", "mae_pt", "hold_s"]].quantile(0.50),
            "p75": target[["mfe_pt", "mae_pt", "hold_s"]].quantile(0.75),
            "p90": target[["mfe_pt", "mae_pt", "hold_s"]].quantile(0.90),
        }).T
        print(pct.round(3).to_string())

    # Optional CSV rollup -- long-form (dim, bucket, n, wr, net, avg)
    if args.csv_rollup:
        rollup_frames = []
        for dim_name, sdf in [
            ("mfe_bucket", bucket_stats(parents, "mfe_bucket")),
            ("month",      bucket_stats(parents, "ym")),
            ("session",    bucket_stats(parents, "session")),
            ("side",       bucket_stats(parents, "side")),
            ("exit_reason",bucket_stats(parents, "exit_reason")),
            ("hold_band",  bucket_stats(parents, "hold_band")),
        ]:
            if len(sdf) == 0:
                continue
            tmp = sdf.copy()
            tmp.insert(0, "dim", dim_name)
            tmp["bucket"] = tmp["bucket"].astype(str)
            rollup_frames.append(tmp[["dim", "bucket", "n", "wins", "wr",
                                      "net", "avg", "med_mfe_pt",
                                      "med_mae_pt", "med_hold"]])
        if rollup_frames:
            pd.concat(rollup_frames, ignore_index=True).to_csv(
                args.csv_rollup, index=False)
            print(f"\nWrote rollup to {args.csv_rollup}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
