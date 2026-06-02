#!/usr/bin/env python3
"""
x1_stage1.py
============
Stage-1 offline refinement of the X1 overlay (see SESSION_HANDOFF.md /
X1_OVERLAY_FINDINGS.md). Two cuts that turn the 71.9%-vs-51.5% confirm-filter
result from suggestive into something buildable:

  1. PER-ENGINE / PER-FAMILY confirm-gap breakdown.
     Hypothesis (findings §5.2): the momentum-confirm filter helps trend
     engines (Tsmom / Donchian / EmaPullback / XauTrendFollow) and is
     neutral-or-hurts scalp / mean-reversion engines. Tests it, and reports
     where the winner>loser gap actually lives instead of the pooled level.

  2. HORIZONS MATCHED TO HOLD TIME.
     The pooled forward test used 5/10/20-bar (5-20 min) horizons, but many
     trades hold hours (findings §4 caveat: M1 forward test UNDERSTATES edge).
     This re-runs the tag forward-return validation at horizons matched to each
     family's median hold_sec, so the tag's measured edge is evaluated over the
     window the trade is actually exposed to.

Imports the validated, fidelity-checked machinery from x1_validate.py — same
bars, same non-repainting tags, same forward-return engine. No re-implementation.

Usage
-----
  python3 x1_stage1.py --bars XAUUSD_2026-05_m1.csv --symbol XAUUSD \
      --trades ~/Downloads/omega_trade_closes.csv

Read-only research tooling. Touches no core/engine code.
"""

import argparse
import sys
import numpy as np
import pandas as pd

import x1_validate as X


# --------------------------------------------------------------------------- #
# Strategy-family map. Substring-matched against the engine column (engine
# names carry param suffixes like _H1_long / _Donchian_N20_sl2.0tp4.0).
# Buckets chosen by entry mechanic, the axis the confirm-filter hypothesis is
# about: does the engine already enter WITH momentum (trend) or against it
# (mean-rev), or is it timing-agnostic (scalp / straddle).
# --------------------------------------------------------------------------- #
# Order matters: first matching family wins. Mean-rev keys are listed before
# trend so an "RSI" / "Reversion" engine can't be captured by a looser trend key.
FAMILY = [
    # counter-momentum entries (fade) — confirm-filter expected NEUTRAL/NEGATIVE
    ("meanrev",   ["MeanReversion", "VWAPReversion", "RSI", "DynamicRange",
                   "AsianRange", "XauThreeBar", "XauusdFvg", "BBrev", "bband",
                   "BBScalp"]),
    # momentum-confirming entries (trend / breakout / session-momentum) —
    # confirm-filter expected POSITIVE
    ("trend",     ["Tsmom", "Donchian", "DonchN", "EmaPullback", "XauTrendFollow",
                   "UstecTrendFollow", "Ger40Turtle", "Ger40Keltner", "Keltner",
                   "_MA_", "LondonFixMomentum", "LondonOpen", "MacroCrash",
                   "IndexSwing", "Turtle"]),
    # scalp — fast, timing-agnostic
    ("scalp",     ["GoldScalpPyramid", "MidScalperGold", "GoldUltimate",
                   "FxScalpPyramid", "Scalp"]),
    # straddle / ORB — bidirectional breakout
    ("straddle",  ["XauStraddle", "IdxStraddle", "Straddle", "Orb", "ORB"]),
]

# hold_sec above this is treated as a bad/unclosed record and dropped from any
# hold-time statistic. The XauTrendFollow2h/4h/D1 rows carry ~6.9e7 s (~800 d),
# which is an unclosed-exit artifact, not a real hold. 14 days is already longer
# than any legitimate intraday-to-swing gold hold in this book.
MAX_SANE_HOLD_SEC = 14 * 24 * 3600


def family_of(engine: str) -> str:
    for fam, keys in FAMILY:
        if any(k in engine for k in keys):
            return fam
    return "other"


def wilson_lo_hi(k, n, z=1.96):
    """Wilson 95% CI for a proportion — honest small-n error bars."""
    if n == 0:
        return (float("nan"), float("nan"))
    p = k / n
    d = 1 + z * z / n
    c = p + z * z / (2 * n)
    half = z * np.sqrt(p * (1 - p) / n + z * z / (4 * n * n))
    return ((c - half) / d, (c + half) / d)


# --------------------------------------------------------------------------- #
# Cut 1: per-engine / per-family confirm-gap
# --------------------------------------------------------------------------- #
def confirm_breakdown(ctx: pd.DataFrame):
    ctx = ctx.copy()
    ctx["family"] = ctx["engine"].map(family_of)
    ctx["win"] = ctx["net_pnl"] > 0
    ctx["los"] = ctx["net_pnl"] < 0

    def block(df, label):
        w = df[df["win"]]
        l = df[df["los"]]
        nw, nl = len(w), len(l)
        cw = int(w["confirming_tag"].sum())
        cl = int(l["confirming_tag"].sum())
        fw = (cw / nw * 100) if nw else float("nan")
        fl = (cl / nl * 100) if nl else float("nan")
        gap = (fw - fl) if (nw and nl) else float("nan")
        wlo, whi = wilson_lo_hi(cw, nw)
        llo, lhi = wilson_lo_hi(cl, nl)
        return dict(group=label, n=len(df), win=nw, los=nl,
                    win_conf=fw, los_conf=fl, gap=gap,
                    win_ci=f"[{wlo*100:4.0f},{whi*100:4.0f}]" if nw else "",
                    los_ci=f"[{llo*100:4.0f},{lhi*100:4.0f}]" if nl else "")

    print("\n" + "=" * 78)
    print("CUT 1 — Confirm-gap by family  (confirming momentum tag in prior "
          "lookback bars)")
    print("=" * 78)
    print("  win_conf/los_conf = % of winning / losing trades with a confirming "
          "tag.\n  gap = win_conf - los_conf (the real signal). CI = Wilson 95%.\n")

    fam_rows = [block(ctx[ctx["family"] == f], f)
                for f in ["trend", "meanrev", "scalp", "straddle", "other"]]
    fam_rows = [r for r in fam_rows if r["n"] > 0]
    fam_rows.append(block(ctx, "ALL"))
    fam = pd.DataFrame(fam_rows)
    with pd.option_context("display.float_format", lambda v: f"{v:5.1f}"):
        print(fam[["group", "n", "win", "los", "win_conf", "win_ci",
                   "los_conf", "los_ci", "gap"]].to_string(index=False))

    # per-engine, only where both classes have >=3 so the gap means something
    print("\n  Per-engine (shown where wins>=3 AND losses>=3; else underpowered):")
    eng_rows = []
    for eng, df in ctx.groupby("engine"):
        r = block(df, eng)
        r["family"] = family_of(eng)
        eng_rows.append(r)
    eng = pd.DataFrame(eng_rows)
    show = eng[(eng["win"] >= 3) & (eng["los"] >= 3)].sort_values("gap",
                                                                  ascending=False)
    if show.empty:
        print("    (none reach the n>=3/3 bar — per-engine is underpowered at "
              "this sample; rely on the family rows)")
    else:
        with pd.option_context("display.float_format", lambda v: f"{v:5.1f}"):
            print(show[["group", "family", "win", "los", "win_conf",
                        "los_conf", "gap"]].to_string(index=False))
    return fam, eng


# --------------------------------------------------------------------------- #
# Cut 2: horizons matched to hold time
# --------------------------------------------------------------------------- #
def hold_matched_horizons(b, trades, tf_min):
    tr = trades.copy()
    tr["hold_sec"] = pd.to_numeric(tr["hold_sec"], errors="coerce")
    tr["family"] = tr["engine"].map(family_of)

    bad = tr[tr["hold_sec"] > MAX_SANE_HOLD_SEC]
    if len(bad):
        print("\n" + "=" * 78)
        print("CUT 2 — Hold-time-matched horizons")
        print("=" * 78)
        print(f"  DATA-QUALITY GUARD: dropped {len(bad)} trade(s) with "
              f"hold_sec > {MAX_SANE_HOLD_SEC} (~unclosed/exit artifact):")
        for eng, n in bad["engine"].value_counts().items():
            print(f"    {eng}: {n}")
    else:
        print("\n" + "=" * 78)
        print("CUT 2 — Hold-time-matched horizons")
        print("=" * 78)

    good = tr[(tr["hold_sec"] > 0) & (tr["hold_sec"] <= MAX_SANE_HOLD_SEC)]

    # per-family median hold -> bars
    print("\n  Median hold per family and the matched forward horizon (bars):")
    fam_h = {}
    rows = []
    for fam in ["trend", "meanrev", "scalp", "straddle", "other"]:
        d = good[good["family"] == fam]
        if d.empty:
            continue
        med = float(d["hold_sec"].median())
        bars = max(1, int(round(med / (tf_min * 60))))
        fam_h[fam] = bars
        rows.append(dict(family=fam, n=len(d), median_hold_sec=med,
                         median_hold_min=med / 60, matched_horizon_bars=bars))
    print(pd.DataFrame(rows).to_string(index=False))

    # collect the distinct horizons we actually need, plus the pooled reference
    horizons = sorted(set(list(fam_h.values()) + [5, 10, 20]))
    print(f"\n  Re-running tag forward-return validation at horizons (bars): "
          f"{horizons}")
    fv = X.forward_validation(b, horizons)

    # annotate which family each matched horizon belongs to
    h2fam = {}
    for fam, h in fam_h.items():
        h2fam.setdefault(h, []).append(fam)
    fv["matched_family"] = fv["horizon_bars"].map(
        lambda h: ",".join(h2fam.get(h, [])) )

    print("\n  Forward-return validation at all horizons "
          "(matched_family = which family holds ~this long):")
    with pd.option_context("display.float_format", lambda v: f"{v:7.3f}"):
        print(fv.to_string(index=False))

    # focused readout: each family's tags at ITS hold horizon
    print("\n  FOCUSED — each family's confirming tag evaluated at its own hold "
          "horizon:")
    print("  (trend confirms with momentum_up/down; mean-rev fades via "
          "retr_up/down)")
    foc = []
    fam_tag = {"trend": ["momentum_up", "momentum_down"],
               "meanrev": ["retr_up", "retr_down"],
               "scalp": ["momentum_up", "momentum_down"],
               "straddle": ["momentum_up", "momentum_down"],
               "other": ["momentum_up", "momentum_down"]}
    for fam, h in fam_h.items():
        for tag in fam_tag[fam]:
            r = fv[(fv["tag"] == tag) & (fv["horizon_bars"] == h)]
            if not r.empty:
                rr = r.iloc[0]
                foc.append(dict(family=fam, horizon_bars=h, tag=tag,
                                n=int(rr["n"]), median_bps=rr["median_bps"],
                                hit_rate_pct=rr["hit_rate_pct"]))
    with pd.option_context("display.float_format", lambda v: f"{v:7.3f}"):
        print(pd.DataFrame(foc).to_string(index=False))
    return fv


# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description="X1 overlay Stage-1 refinement")
    ap.add_argument("--bars", required=True)
    ap.add_argument("--symbol", required=True)
    ap.add_argument("--trades", required=True)
    ap.add_argument("--tf", type=int, default=X.DEFAULTS["tf"])
    ap.add_argument("--lookback", type=int, default=X.DEFAULTS["lookback"])
    args = ap.parse_args()

    cfg = dict(X.DEFAULTS)
    cfg["tf"] = args.tf
    cfg["lookback"] = args.lookback

    b = X.load_bars(args.bars)
    if len(b) < max(cfg["ema_slow"], cfg["wt_n2"]) + 5:
        raise SystemExit(f"Only {len(b)} bars — not enough.")
    b = X.compute_tags(b, cfg)

    trades = X.load_trades(args.trades, args.symbol)
    print(f"=== {args.symbol}  bars={len(b)}  "
          f"{b.index[0]} .. {b.index[-1]} ===")
    print(f"Trades for {args.symbol}: {len(trades)}  (lookback={cfg['lookback']} "
          f"bars, tf=M{cfg['tf']})")

    ctx = X.trade_context(b, trades, cfg["lookback"])
    ctx = ctx[ctx["dir"] != 0]
    print(f"In-window trades: {len(ctx)}  "
          f"(winners {(ctx['net_pnl']>0).sum()} / "
          f"losers {(ctx['net_pnl']<0).sum()})")

    confirm_breakdown(ctx)
    hold_matched_horizons(b, trades, cfg["tf"])


if __name__ == "__main__":
    main()
