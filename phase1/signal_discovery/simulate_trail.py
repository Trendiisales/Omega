#!/usr/bin/env python3
"""
simulate_trail.py
=================
Realistic bar-by-bar trail-engine simulation on the discovered setups.

For each entry candidate produced by discover_setups.py:
  1. Walk forward bar-by-bar through the 30-min lookahead window.
  2. Track HFP (high water mark on favorable side) since entry.
  3. Check exit conditions in order each bar:
        a. Initial SL: low/high crosses entry +/- sl_atr * atr60_at_entry
        b. Trail SL : after HFP exceeds entry by min_trail_arm_pt, exit when
                      adverse pullback from HFP exceeds (1 - trail_lock_frac) * (HFP - entry)
        c. Time stop: exit at close of bar entry+max_hold_bars
  4. Apply cooldown -- skip any candidate entry that arrives within
     cooldown_bars of the previous exit (prevents the unrealistic
     1000+/day frequency of the raw discovery).
  5. Subtract round-trip cost.

Reports:
  - phase1/signal_discovery/trail_sim_<setup>_<config>.parquet
  - phase1/signal_discovery/TRAIL_SIM_REPORT.md   (per-setup, per-config grid)
"""
from __future__ import annotations
import os, sys, time
import numpy as np
import pandas as pd

OUT_DIR = "/sessions/hopeful-friendly-meitner/mnt/omega_repo/phase1/signal_discovery"
BARS    = os.path.join(OUT_DIR, "bars_5s.parquet")
REPORT  = os.path.join(OUT_DIR, "TRAIL_SIM_REPORT.md")

MAX_HOLD_BARS = 360       # 30 min ceiling
COST_PT       = 0.65
COOLDOWN_BARS = 60        # 5 min between exit and next entry

# Trail / SL configurations to grid-search
CONFIGS = [
    # name,                     trail_lock, sl_atr, min_arm
    ("trail80_sl15_arm05",      0.80,       1.5,    0.5),
    ("trail80_sl10_arm05",      0.80,       1.0,    0.5),
    ("trail70_sl15_arm10",      0.70,       1.5,    1.0),
    ("trail60_sl20_arm10",      0.60,       2.0,    1.0),
    ("trail90_sl15_arm03",      0.90,       1.5,    0.3),
    ("trail50_sl15_arm05",      0.50,       1.5,    0.5),
]

SETUPS = ["compression_break", "spike_reverse", "momentum_pullback", "level_retest_reject"]


def simulate_setup(setup: str, bars: pd.DataFrame, fr: pd.DataFrame, cfg: dict) -> dict:
    """Walk every entry of a setup and return aggregate stats."""
    # bars: indexed by integer position; need atr60 column
    high = bars["high_mid"].to_numpy()
    low  = bars["low_mid"].to_numpy()
    close = bars["close_mid"].to_numpy()
    atr60 = bars["atr60"].to_numpy()
    # bar_ms -> integer index
    ms_to_idx = pd.Series(np.arange(len(bars), dtype=np.int64), index=bars["bar_ms"].to_numpy())

    entries = fr.merge(
        ms_to_idx.rename("ix").reset_index().rename(columns={"index": "bar_ms"}),
        on="bar_ms", how="left"
    )
    entries = entries.dropna(subset=["ix"]).sort_values("ix").reset_index(drop=True)
    entries["ix"] = entries["ix"].astype(np.int64)

    trail_lock = cfg["trail_lock"]
    sl_atr     = cfg["sl_atr"]
    min_arm    = cfg["min_arm"]
    cooldown   = cfg["cooldown"]

    last_exit = -1
    realized = []
    n_skip_cd = 0
    n_skip_lookahead = 0
    n_skip_atr = 0

    for _, e in entries.iterrows():
        i      = int(e["ix"])
        side   = int(e["side"])
        e_px   = float(e["entry_px"])
        if i + MAX_HOLD_BARS >= len(bars):
            n_skip_lookahead += 1
            continue
        a60 = atr60[i]
        if not np.isfinite(a60) or a60 <= 0:
            n_skip_atr += 1
            continue
        if i < last_exit + cooldown:
            n_skip_cd += 1
            continue

        sl_dist    = sl_atr * a60
        # Walk forward
        hfp = e_px
        exit_idx = None
        exit_px  = None
        exit_reason = "TIME"
        for j in range(i + 1, i + 1 + MAX_HOLD_BARS):
            h = high[j]; l = low[j]; c = close[j]
            if side == 1:
                # Initial SL on long: low touches entry - sl_dist
                if l <= e_px - sl_dist:
                    exit_idx = j; exit_px = e_px - sl_dist; exit_reason = "SL"
                    break
                # Update HFP
                if h > hfp:
                    hfp = h
                # Trail SL: only after HFP-e_px >= min_arm
                if hfp - e_px >= min_arm:
                    trail_stop = hfp - (1.0 - trail_lock) * (hfp - e_px)
                    if l <= trail_stop:
                        exit_idx = j; exit_px = trail_stop; exit_reason = "TRAIL"
                        break
            else:  # short
                if h >= e_px + sl_dist:
                    exit_idx = j; exit_px = e_px + sl_dist; exit_reason = "SL"
                    break
                if l < hfp:
                    hfp = l
                if e_px - hfp >= min_arm:
                    trail_stop = hfp + (1.0 - trail_lock) * (e_px - hfp)
                    if h >= trail_stop:
                        exit_idx = j; exit_px = trail_stop; exit_reason = "TRAIL"
                        break

        if exit_idx is None:
            exit_idx = i + MAX_HOLD_BARS
            exit_px  = close[exit_idx]
            exit_reason = "TIME"

        pnl = (exit_px - e_px) * side - COST_PT
        realized.append({
            "entry_ix": i,
            "exit_ix":  exit_idx,
            "side": side,
            "entry_px": e_px,
            "exit_px": exit_px,
            "hold_bars": exit_idx - i,
            "exit_reason": exit_reason,
            "pnl_pt": pnl,
            "atr60_at_entry": float(a60),
        })
        last_exit = exit_idx

    rdf = pd.DataFrame(realized)
    n = len(rdf)
    if n == 0:
        return {"n": 0}
    out = {
        "n":              n,
        "n_skip_cd":      n_skip_cd,
        "n_skip_la":      n_skip_lookahead,
        "n_skip_atr":     n_skip_atr,
        "freq_per_day":   n / 365.0,
        "mean_pt":        float(rdf["pnl_pt"].mean()),
        "stdev_pt":       float(rdf["pnl_pt"].std()),
        "median_pt":      float(rdf["pnl_pt"].median()),
        "win_rate":       float((rdf["pnl_pt"] > 0).mean()),
        "avg_win":        float(rdf.loc[rdf["pnl_pt"] > 0, "pnl_pt"].mean()) if (rdf["pnl_pt"] > 0).any() else 0.0,
        "avg_loss":       float(rdf.loc[rdf["pnl_pt"] < 0, "pnl_pt"].mean()) if (rdf["pnl_pt"] < 0).any() else 0.0,
        "total_pt":       float(rdf["pnl_pt"].sum()),
        "tstat":          float(rdf["pnl_pt"].mean() / (rdf["pnl_pt"].std() / np.sqrt(n))) if rdf["pnl_pt"].std() > 0 else 0.0,
        "exit_hist":      rdf["exit_reason"].value_counts().to_dict(),
        "rdf":            rdf,
    }
    return out


def main():
    print(f"[+] loading bars {BARS}", flush=True)
    bars = pd.read_parquet(BARS)
    bars = bars.sort_values("bar_ms").reset_index(drop=True)
    # we need atr60 -- recompute (forward_returns parquet didn't carry it)
    bars["range"] = bars["high_mid"] - bars["low_mid"]
    bars["atr60"] = bars["range"].rolling(60, min_periods=30).mean()

    grid_results = {}  # (setup, config_name) -> stats
    for setup in SETUPS:
        fr_path = os.path.join(OUT_DIR, f"forward_returns_{setup}.parquet")
        if not os.path.exists(fr_path):
            print(f"  [{setup}] no forward_returns parquet -- skipping", flush=True)
            continue
        fr = pd.read_parquet(fr_path)
        for cname, lock, sl, arm in CONFIGS:
            cfg = {"trail_lock": lock, "sl_atr": sl, "min_arm": arm, "cooldown": COOLDOWN_BARS}
            t0 = time.time()
            stats = simulate_setup(setup, bars, fr, cfg)
            elapsed = time.time() - t0
            stats["elapsed_s"] = elapsed
            stats["config"] = cname
            stats["setup"]  = setup
            print(f"  [{setup}/{cname}] n={stats.get('n',0):,}  "
                  f"mean={stats.get('mean_pt', float('nan')):+.3f}  "
                  f"win={stats.get('win_rate', 0):.1%}  "
                  f"freq/day={stats.get('freq_per_day', 0):.2f}  "
                  f"tstat={stats.get('tstat', 0):+.1f}  "
                  f"({elapsed:.1f}s)", flush=True)
            grid_results[(setup, cname)] = stats
            # save raw realized trades for the strongest configs only (file count control)
            if stats["n"] > 0 and "rdf" in stats:
                stats["rdf"].to_parquet(
                    os.path.join(OUT_DIR, f"trail_sim_{setup}_{cname}.parquet"),
                    compression="zstd",
                )

    # ------- write report -------
    lines = [
        "# Trail-Engine Simulation Report",
        "",
        f"_Generated: {pd.Timestamp.utcnow().isoformat()}_",
        "",
        f"**Corpus:** 2025-04-01 -> 2026-04-01 (~365 days)",
        f"**Cost:** {COST_PT} pt round-trip",
        f"**Cooldown:** {COOLDOWN_BARS} bars (= 5 min) between exit and next entry",
        f"**Max hold:** {MAX_HOLD_BARS} bars (= 30 min)",
        f"**Configs tested:**",
    ]
    for cname, lock, sl, arm in CONFIGS:
        lines.append(f"- `{cname}`: trail_lock={lock}, SL={sl}*atr60, arm at +{arm}pt MFE")
    lines.append("")
    lines.append("## Per-setup grid")
    for setup in SETUPS:
        lines.append(f"### {setup}")
        lines.append("")
        lines.append("| config | n trades | freq/day | mean (pt, net) | t-stat | win% | avg win | avg loss | total pt | exits |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|:---|")
        for cname, *_ in CONFIGS:
            s = grid_results.get((setup, cname))
            if not s or s.get("n", 0) == 0:
                lines.append(f"| {cname} | -- | -- | -- | -- | -- | -- | -- | -- | -- |")
                continue
            ex = s["exit_hist"]
            ex_s = " ".join(f"{k}:{v}" for k, v in ex.items())
            lines.append(
                f"| `{cname}` | {s['n']:,} | {s['freq_per_day']:.2f} | "
                f"{s['mean_pt']:+.3f} | {s['tstat']:+.1f} | "
                f"{s['win_rate']:.1%} | {s['avg_win']:+.3f} | {s['avg_loss']:+.3f} | "
                f"{s['total_pt']:+.0f} | {ex_s} |"
            )
        lines.append("")
    # winner pick
    valid = [(k, v) for k, v in grid_results.items() if v.get("n", 0) > 0]
    if valid:
        # Rank by total_pt with t-stat tiebreak
        valid.sort(key=lambda kv: (kv[1].get("total_pt", 0), kv[1].get("tstat", 0)), reverse=True)
        top = valid[:5]
        lines.append("## Top 5 by total pt over corpus")
        lines.append("")
        lines.append("| rank | setup | config | n | mean | t-stat | total pt |")
        lines.append("|---:|---|---|---:|---:|---:|---:|")
        for rk, ((setup, cname), s) in enumerate(top, 1):
            lines.append(f"| {rk} | {setup} | `{cname}` | {s['n']:,} | "
                         f"{s['mean_pt']:+.3f} | {s['tstat']:+.1f} | {s['total_pt']:+.0f} |")
        winner = top[0]
        (w_setup, w_cfg), w_stats = winner
        lines.append("")
        lines.append("## Recommended engine spec")
        lines.append("")
        if w_stats["mean_pt"] > 0 and w_stats["tstat"] > 2.0:
            lines.append(f"**Build C++ engine targeting `{w_setup}` with trail config `{w_cfg}`.**")
            lines.append(f"- Per-trade net edge: **{w_stats['mean_pt']:+.3f} pt** (t-stat {w_stats['tstat']:+.1f})")
            lines.append(f"- Frequency: **{w_stats['freq_per_day']:.1f} trades/day**")
            lines.append(f"- Annual total (1 unit): **{w_stats['total_pt']:+.0f} pt**")
        else:
            lines.append(f"Top result is `{w_setup}/{w_cfg}` with mean {w_stats['mean_pt']:+.3f} "
                         f"and t-stat {w_stats['tstat']:+.1f}.  Edge is marginal -- "
                         f"recommend either deploying as research-grade shadow engine OR "
                         f"iterating signal-discovery with tighter triggers.")
    else:
        lines.append("**No valid simulation result.**")

    with open(REPORT, "w") as fh:
        fh.write("\n".join(lines))
    print(f"[+] wrote {REPORT}", flush=True)


if __name__ == "__main__":
    main()
