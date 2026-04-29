#!/usr/bin/env python3
"""
discover_setups.py
==================
Signal-discovery on the post-2025-04 5-second bar corpus.  Catalogs four
candidate bidirectional intraday setups:

  A. Compression-Break    -- low-vol coil into directional break-out
  B. Spike-Reverse        -- one-bar adverse spike then mean-revert
  C. Momentum-Pullback    -- trend with shallow pullback that resumes
  D. Level-Retest-Reject  -- recent S/R retested then rejected away

For each setup it finds long+short entry bars, then computes forward returns
at +6 / +24 / +60 / +180 / +360 bars (= 30s / 2min / 5min / 15min / 30min) on
the close-to-close mid, plus per-trade MFE / MAE.  Applies the gates from
SESSION_HANDOFF_2026-04-29_wrap.md Step 6:

  (a) t-stat > 2 on the 30s-5min horizons (mean / stderr)
  (b) win-rate * avg-win > 1.2 * loss-rate * avg-loss after 0.65 pt cost
  (c) frequency >= 5 trades / day across the 365-day corpus (>= 1825 entries)

Inputs:  phase1/signal_discovery/bars_5s.parquet (must already exist)
Outputs:
  phase1/signal_discovery/forward_returns_<setup>.parquet  (raw per-entry)
  phase1/signal_discovery/setup_catalog.md                 (the report)
  phase1/signal_discovery/CHOSEN_SETUP.md                  (winner OR negative-result memo)

Run from anywhere; paths are absolute.
"""
from __future__ import annotations
import os, sys, time, math
import numpy as np
import pandas as pd

# -----------------------------------------------------------------------------
# Paths and constants
# -----------------------------------------------------------------------------
OUT_DIR  = "/sessions/hopeful-friendly-meitner/mnt/omega_repo/phase1/signal_discovery"
BARS     = os.path.join(OUT_DIR, "bars_5s.parquet")
CAT_OUT  = os.path.join(OUT_DIR, "setup_catalog.md")
CHOSEN   = os.path.join(OUT_DIR, "CHOSEN_SETUP.md")

# Forward-return horizons in 5-second bars: 30s, 2min, 5min, 15min, 30min
HORIZONS = {
    "30s":   6,
    "2min":  24,
    "5min":  60,
    "15min": 180,
    "30min": 360,
}
SHORT_HORIZONS = ["30s", "2min", "5min"]   # gate (a) is restricted to these

COST_PT          = 0.65   # round-trip cost in points (spread + slippage)
GATE_TSTAT       = 2.0
GATE_FREQ_PER_DY = 5.0
GATE_RR_RATIO    = 1.2    # win-rate * avg-win > 1.2 * loss-rate * avg-loss
DAYS_IN_CORPUS   = 365

# -----------------------------------------------------------------------------
# Load bars and derive features
# -----------------------------------------------------------------------------
def load_bars() -> pd.DataFrame:
    if not os.path.exists(BARS):
        sys.exit(f"missing input: {BARS}.  Run aggregate_5s_bars.py first.")
    print(f"[+] loading {BARS}", flush=True)
    df = pd.read_parquet(BARS)
    print(f"    rows={len(df):,}", flush=True)
    return df

def add_features(df: pd.DataFrame) -> pd.DataFrame:
    print("[+] computing features", flush=True)
    df = df.sort_values("bar_ms").reset_index(drop=True)
    df["ret"]      = df["close_mid"].diff()                 # absolute pt return per 5s
    df["range"]    = df["high_mid"] - df["low_mid"]
    df["atr12"]    = df["range"].rolling(12, min_periods=6).mean()    # 1m lookback
    df["atr60"]    = df["range"].rolling(60, min_periods=30).mean()   # 5m lookback
    df["vol12"]    = df["ret"].rolling(12, min_periods=6).std()
    df["vol60"]    = df["ret"].rolling(60, min_periods=30).std()
    df["z_ret"]    = df["ret"] / df["vol12"].replace(0, np.nan)
    df["compress"] = df["vol12"] / df["vol60"].replace(0, np.nan)
    # 60-bar (5-min) rolling extremes -- shift so the bar at i is compared
    # against the prior 60 bars only (no look-ahead)
    df["roll_max60"] = df["close_mid"].rolling(60, min_periods=30).max().shift(1)
    df["roll_min60"] = df["close_mid"].rolling(60, min_periods=30).min().shift(1)
    df["roll_max24"] = df["high_mid"].rolling(24, min_periods=12).max().shift(1)
    df["roll_min24"] = df["low_mid"].rolling(24, min_periods=12).min().shift(1)
    # 24-bar momentum (2 min)
    df["mom24"] = (df["close_mid"] - df["close_mid"].shift(24)) / df["atr60"].replace(0, np.nan)
    # session hour (UTC)
    df["hour"] = ((df["bar_ms"] // 1000 // 3600) % 24).astype("int16")
    return df

# -----------------------------------------------------------------------------
# Setup definitions -- each returns a Series of {-1, 0, +1} signal where
# +1=long entry on this bar's close, -1=short entry, 0=no entry.
# Bars without a fully formed feature set get 0.
# -----------------------------------------------------------------------------
def setup_compression_break(df: pd.DataFrame) -> pd.Series:
    """Compression coil into directional break of the 5-min range."""
    sig = pd.Series(0, index=df.index, dtype="int8")
    coiled = (df["compress"] < 0.50)              # vol_12 well below vol_60
    long_brk  = coiled & (df["close_mid"] > df["roll_max60"]) & (df["mom24"] > 0)
    short_brk = coiled & (df["close_mid"] < df["roll_min60"]) & (df["mom24"] < 0)
    sig.loc[long_brk]  = 1
    sig.loc[short_brk] = -1
    return sig

def setup_spike_reverse(df: pd.DataFrame) -> pd.Series:
    """One-bar adverse z-score spike, current bar closes opposite."""
    sig = pd.Series(0, index=df.index, dtype="int8")
    prev_z   = df["z_ret"].shift(1)
    cur_ret  = df["ret"]
    big_dn   = (prev_z < -3.0) & (cur_ret > 0)    # spike down then up close
    big_up   = (prev_z > +3.0) & (cur_ret < 0)    # spike up then down close
    sig.loc[big_dn] = 1
    sig.loc[big_up] = -1
    return sig

def setup_momentum_pullback(df: pd.DataFrame) -> pd.Series:
    """Strong 2-min trend, last 3 bars retraced, current bar resumes."""
    sig = pd.Series(0, index=df.index, dtype="int8")
    # mom24 measured 4 bars ago (gives the trend BEFORE the pullback started)
    trend = df["mom24"].shift(4)
    pull  = df["close_mid"] - df["close_mid"].shift(3)   # last-3-bar move
    cur   = df["ret"]
    long_pb  = (trend >  1.0) & (pull < 0) & (cur > 0)
    short_pb = (trend < -1.0) & (pull > 0) & (cur < 0)
    sig.loc[long_pb]  = 1
    sig.loc[short_pb] = -1
    return sig

def setup_level_retest_reject(df: pd.DataFrame) -> pd.Series:
    """Price retests recent 5-min extreme then closes away (rejection)."""
    sig = pd.Series(0, index=df.index, dtype="int8")
    near_high = (df["high_mid"] >= df["roll_max60"] - 0.5 * df["atr60"]) & \
                (df["close_mid"] < df["roll_max60"] - 0.2 * df["atr60"]) & \
                (df["ret"] < 0)
    near_low  = (df["low_mid"]  <= df["roll_min60"] + 0.5 * df["atr60"]) & \
                (df["close_mid"] > df["roll_min60"] + 0.2 * df["atr60"]) & \
                (df["ret"] > 0)
    sig.loc[near_high] = -1   # reject high -> short
    sig.loc[near_low]  =  1   # reject low  -> long
    return sig

SETUPS = {
    "compression_break":     setup_compression_break,
    "spike_reverse":         setup_spike_reverse,
    "momentum_pullback":     setup_momentum_pullback,
    "level_retest_reject":   setup_level_retest_reject,
}

# -----------------------------------------------------------------------------
# Forward-return engine
# -----------------------------------------------------------------------------
def forward_returns(df: pd.DataFrame, sig: pd.Series, setup_name: str) -> pd.DataFrame:
    """For each non-zero signal bar, compute close-to-close returns at each
    horizon (signed by side), plus MFE/MAE within the horizon window."""
    n_bars = len(df)
    entries = np.flatnonzero(sig.values != 0)
    if len(entries) == 0:
        return pd.DataFrame()
    print(f"    [{setup_name}] {len(entries):,} entries", flush=True)
    close = df["close_mid"].to_numpy()
    high  = df["high_mid"].to_numpy()
    low   = df["low_mid"].to_numpy()
    bar_ms = df["bar_ms"].to_numpy()
    side  = sig.to_numpy()
    rows = []
    max_h = max(HORIZONS.values())
    # restrict to entries with enough lookahead
    keep = entries[entries + max_h < n_bars]
    for i in keep:
        s = int(side[i])
        e_px = close[i]
        rec = {"bar_ms": int(bar_ms[i]), "side": s, "entry_px": float(e_px)}
        # build slice for MFE/MAE on the longest horizon window once
        h_max = max_h
        h_high = high[i+1:i+1+h_max]
        h_low  = low [i+1:i+1+h_max]
        if s == 1:
            rec["mfe_30min"] = float(h_high.max() - e_px)
            rec["mae_30min"] = float(h_low.min()  - e_px)
        else:
            rec["mfe_30min"] = float(e_px - h_low.min())
            rec["mae_30min"] = float(e_px - h_high.max())
        for hz_name, hz in HORIZONS.items():
            x_px = close[i + hz]
            ret_pt = (x_px - e_px) * s    # signed pt return
            rec[f"ret_{hz_name}"] = float(ret_pt)
        rows.append(rec)
    return pd.DataFrame(rows)

# -----------------------------------------------------------------------------
# Stats and gates
# -----------------------------------------------------------------------------
def summarise(df_fr: pd.DataFrame) -> dict:
    """Per-horizon summary stats + gate evaluation."""
    out = {"n_total": int(len(df_fr))}
    out["freq_per_day"] = out["n_total"] / DAYS_IN_CORPUS
    by_horizon = {}
    for hz_name in HORIZONS:
        col = f"ret_{hz_name}"
        if col not in df_fr.columns:
            continue
        x = df_fr[col].to_numpy()
        x_net = x - COST_PT  # cost adjustment per round trip
        n = len(x_net)
        mean = float(np.mean(x_net))
        std  = float(np.std(x_net, ddof=1)) if n > 1 else float("nan")
        tstat = mean / (std / math.sqrt(n)) if std > 0 else 0.0
        wins   = x_net[x_net > 0]
        losses = x_net[x_net < 0]
        wr   = len(wins) / n if n else 0.0
        avgw = float(np.mean(wins))   if len(wins)   else 0.0
        avgl = float(np.mean(losses)) if len(losses) else 0.0
        rr   = (wr * avgw) / (max(1e-9, (1 - wr) * abs(avgl))) if avgl else 0.0
        by_horizon[hz_name] = {
            "n":      n,
            "mean":   mean,
            "stdev":  std,
            "tstat":  tstat,
            "winrate": wr,
            "avg_win":  avgw,
            "avg_loss": avgl,
            "rr_ratio": rr,
            "expectancy": mean,
        }
    out["per_horizon"] = by_horizon
    # Gate evaluation
    short_pass = []
    for hz in SHORT_HORIZONS:
        if hz not in by_horizon:
            continue
        h = by_horizon[hz]
        gate_a = h["tstat"] > GATE_TSTAT
        gate_b = h["rr_ratio"] > GATE_RR_RATIO
        short_pass.append((hz, gate_a, gate_b, h))
    gate_c = out["freq_per_day"] >= GATE_FREQ_PER_DY
    out["gates"] = {
        "freq_pass": gate_c,
        "freq_per_day": out["freq_per_day"],
        "short_horizons": short_pass,
    }
    out["any_short_horizon_passes_all_gates"] = any(
        a and b and gate_c for _, a, b, _ in short_pass
    )
    return out

# -----------------------------------------------------------------------------
# Reports
# -----------------------------------------------------------------------------
def fmt_horizon_row(name: str, h: dict, gates: dict) -> str:
    gate_a = "PASS" if h["tstat"] > GATE_TSTAT else "fail"
    gate_b = "PASS" if h["rr_ratio"] > GATE_RR_RATIO else "fail"
    return (
        f"| {name:>5s} | {h['n']:>6,} | {h['mean']:+7.3f} | {h['stdev']:6.3f}"
        f" | {h['tstat']:+6.2f} | {h['winrate']:5.1%} | {h['avg_win']:+6.3f}"
        f" | {h['avg_loss']:+6.3f} | {h['rr_ratio']:5.2f} | {gate_a:>4s}/{gate_b:<4s} |"
    )

def write_setup_catalog(results: dict) -> None:
    lines = []
    lines.append(f"# Signal Discovery -- Setup Catalog")
    lines.append("")
    lines.append(f"_Generated: {pd.Timestamp.utcnow().isoformat()}_")
    lines.append("")
    lines.append(f"**Corpus:** {BARS}")
    lines.append(f"**Span:** 2025-04-01 -> 2026-04-01 (~{DAYS_IN_CORPUS} days)")
    lines.append(f"**Cost model:** {COST_PT} pt round-trip (subtracted from every horizon return)")
    lines.append(f"**Gates:** t-stat > {GATE_TSTAT} / freq >= {GATE_FREQ_PER_DY}/day / "
                 f"(win-rate * avg-win) > {GATE_RR_RATIO} * (loss-rate * avg-loss)")
    lines.append("")
    lines.append("## Per-setup results")
    for name, res in results.items():
        lines.append(f"### {name}")
        g = res["gates"]
        lines.append(f"- entries total: **{res['n_total']:,}**  "
                     f"(freq/day: **{g['freq_per_day']:.2f}**, "
                     f"freq gate: **{'PASS' if g['freq_pass'] else 'FAIL'}**)")
        if not res["per_horizon"]:
            lines.append("- no entries (skipped)")
            lines.append("")
            continue
        lines.append("")
        lines.append("| horiz | n | mean (pt, net) | std | t-stat | winrate | avg win | avg loss | rr | t/rr gates |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|:---:|")
        for hz_name in HORIZONS:
            h = res["per_horizon"].get(hz_name)
            if h is None:
                continue
            lines.append(fmt_horizon_row(hz_name, h, g))
        verdict = "PASS (build engine)" if res["any_short_horizon_passes_all_gates"] else "fail"
        lines.append("")
        lines.append(f"**Setup verdict:** {verdict}")
        lines.append("")
    # Overall
    winners = [n for n, r in results.items() if r["any_short_horizon_passes_all_gates"]]
    lines.append("## Overall")
    if winners:
        lines.append(f"**{len(winners)} setup(s) clear all gates: {', '.join(winners)}**")
    else:
        lines.append("**No setup cleared all three gates.** See CHOSEN_SETUP.md for the "
                     "negative-result memo and recommended next steps.")
    with open(CAT_OUT, "w") as fh:
        fh.write("\n".join(lines))
    print(f"[+] wrote {CAT_OUT}", flush=True)

def write_chosen(results: dict) -> None:
    winners = [(n, r) for n, r in results.items() if r["any_short_horizon_passes_all_gates"]]
    if winners:
        # pick the one with highest t-stat in any short horizon
        def best_t(r):
            return max(
                (r["per_horizon"][hz]["tstat"] for hz in SHORT_HORIZONS if hz in r["per_horizon"]),
                default=0.0,
            )
        winners.sort(key=lambda nr: best_t(nr[1]), reverse=True)
        name, res = winners[0]
        lines = [
            f"# CHOSEN SETUP: {name}",
            "",
            f"_Generated: {pd.Timestamp.utcnow().isoformat()}_",
            "",
            f"This setup cleared all three signal-discovery gates "
            f"(t-stat > {GATE_TSTAT} on a short horizon, freq >= {GATE_FREQ_PER_DY}/day, "
            f"rr-ratio > {GATE_RR_RATIO} after a {COST_PT}pt cost).",
            "",
            f"**Entries:** {res['n_total']:,} ({res['gates']['freq_per_day']:.2f}/day)",
            "",
            "## Per-horizon edge (cost-adjusted)",
            "",
            "| horiz | n | mean (pt, net) | t-stat | winrate | rr |",
            "|---|---:|---:|---:|---:|---:|",
        ]
        for hz in HORIZONS:
            h = res["per_horizon"].get(hz)
            if not h:
                continue
            lines.append(f"| {hz} | {h['n']:,} | {h['mean']:+.3f} | {h['tstat']:+.2f}"
                         f" | {h['winrate']:.1%} | {h['rr_ratio']:.2f} |")
        lines += [
            "",
            "## Build directive",
            "",
            f"Build a C++ CRTP engine (model on HBG / AsianRange skeleton) that:",
            f"- detects this setup on the live 5-second bar feed",
            f"- enters bidirectionally (long+short)",
            f"- targets the strongest short horizon as the holding window",
            f"- uses an MFE-proportional trail (lock 80% of MFE, mirroring HBG)",
            f"- uses vol-adaptive SL (1.0-1.5 atr60)",
            f"- runs in shadow_mode=true initially",
            f"- includes all standard guardrails (spread gate, macro regime, session filter)",
            "",
            "Raw forward-return parquet at "
            f"`phase1/signal_discovery/forward_returns_{name}.parquet`.",
        ]
    else:
        lines = [
            "# Signal Discovery -- NEGATIVE RESULT",
            "",
            f"_Generated: {pd.Timestamp.utcnow().isoformat()}_",
            "",
            "## Verdict",
            "",
            f"None of the four candidate setups (compression_break, spike_reverse, "
            f"momentum_pullback, level_retest_reject) cleared all three gates "
            f"(t-stat > {GATE_TSTAT} on a 30s-5min horizon, freq >= {GATE_FREQ_PER_DY}/day, "
            f"rr-ratio > {GATE_RR_RATIO} after a {COST_PT}pt cost).",
            "",
            "## What this means",
            "",
            "Per the night-handoff and wrap-handoff Step 6 directive, the build "
            "directive is to **STOP, surface, and not build the engine on weak edge.**",
            "",
            "## Recommended next probes",
            "",
            "1. **Tighter setup definitions.** Try narrower thresholds (e.g. "
            "compress<0.30, spike z>4, mom24>1.5) -- accept lower frequency for higher t-stat.",
            "2. **Session restriction.** Filter to UTC 07:00-16:00 (London/NY overlap) "
            "and re-evaluate; off-hour noise may dilute the in-session edge.",
            "3. **OFI / order-book features.** L2 ticks were not present in this corpus "
            "(L1 only).  If a Level-2 feed becomes available the OFI setup is worth re-examining.",
            "4. **Multi-bar confirmation.** Require 2-3 bar pattern confirmation rather than "
            "single-bar entry; reduces false positives at small cost in frequency.",
            "5. **Ensemble.** Combine setups using weighted vote -- individual edges may not "
            "clear gates but a combined signal could.",
            "",
            "## Per-setup near-miss summary",
            "",
        ]
        lines.append("| setup | n | freq/day | best short-horiz t-stat | best rr |")
        lines.append("|---|---:|---:|---:|---:|")
        for name, r in results.items():
            best_t = max(
                (r["per_horizon"][hz]["tstat"] for hz in SHORT_HORIZONS if hz in r["per_horizon"]),
                default=0.0,
            )
            best_r = max(
                (r["per_horizon"][hz]["rr_ratio"] for hz in SHORT_HORIZONS if hz in r["per_horizon"]),
                default=0.0,
            )
            lines.append(f"| {name} | {r['n_total']:,} | {r['gates']['freq_per_day']:.2f}"
                         f" | {best_t:+.2f} | {best_r:.2f} |")
    with open(CHOSEN, "w") as fh:
        fh.write("\n".join(lines))
    print(f"[+] wrote {CHOSEN}", flush=True)

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
def main(argv) -> int:
    t0 = time.time()
    df = load_bars()
    df = add_features(df)
    results = {}
    for name, fn in SETUPS.items():
        sig = fn(df)
        n_sig = int((sig != 0).sum())
        if n_sig == 0:
            print(f"  [{name}] no entries -- skipping", flush=True)
            results[name] = {"n_total": 0, "per_horizon": {}, "gates": {
                "freq_pass": False, "freq_per_day": 0.0, "short_horizons": []},
                "any_short_horizon_passes_all_gates": False}
            continue
        df_fr = forward_returns(df, sig, name)
        out_pq = os.path.join(OUT_DIR, f"forward_returns_{name}.parquet")
        df_fr.to_parquet(out_pq, compression="zstd")
        print(f"    -> {out_pq} ({len(df_fr):,} rows)", flush=True)
        results[name] = summarise(df_fr)
    write_setup_catalog(results)
    write_chosen(results)
    print(f"[+] total elapsed: {time.time()-t0:.1f}s", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
