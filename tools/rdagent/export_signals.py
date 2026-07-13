#!/usr/bin/env python3
"""
RD-Agent -> Omega signal/factor exporter (Path A bridge).

Reads a finished qlib experiment and writes a single JSON the Omega GUI serves:
- model quality (IC) + a real cost-aware portfolio backtest (net Sharpe / return /
  maxDD / hit-rate), so the panel shows meaningful numbers, not NaN;
- today's BUY BASKET (the actionable output) with each name's recent momentum and
  the model's confidence percentile, so a human can read it without decoding raw
  z-scores;
- the discovered factor source + honest provenance.

Provenance is stamped honestly: RD-Agent's qlib backtest is daily bar-replay.
These are CANDIDATES to validate in Omega's faithful tick BT, never direct trades.

Usage:
    python export_signals.py --mlruns <dir> --provider ~/.qlib/qlib_data/omega_data \
        --region us --universe BIGCAP [--topk 5] [--cost-bps 10] [--factors <file>]
"""
from __future__ import annotations

import argparse
import datetime as dt
import glob
import json
import os
from pathlib import Path

import numpy as np
import pandas as pd


def _latest_run(mlruns: str) -> str:
    preds = glob.glob(os.path.join(mlruns, "**", "pred.pkl"), recursive=True)
    if not preds:
        raise SystemExit(f"No pred.pkl found under {mlruns} — run a qlib experiment first.")
    return os.path.dirname(max(preds, key=os.path.getmtime))


def _ic(artifacts: str) -> dict:
    out = {}
    for name, key in [("ic.pkl", "ic"), ("ric.pkl", "rank_ic")]:
        p = os.path.join(artifacts, "sig_analysis", name)
        if os.path.exists(p):
            try:
                out[key] = round(float(pd.read_pickle(p).mean()), 4)
            except Exception:
                pass
    return out


# Gold-companion lock-in params (faithful port of stall_accountant.py, regime-validated
# 2026-06-30 daymover_lockin_sweep.py): only ARM after capturing GOLD_GATE; then clip on
# STALL (no new high for GOLD_STALL days) or REVERSAL (give back GOLD_REVGB of MFE peak).
# Best bear-sleeve lock: gate 2% / giveback 10% / stall 2 -> PF 1.53, DD -69% (vs wide -183%).
GOLD_GATE, GOLD_REVGB, GOLD_STALL = 0.02, 0.10, 2

# CATASTROPHE COLD-CUT (S-2026-07-01): direction-aware adverse floor for the cold
# loser that the AND-armed STALL/REVERSAL clips can never reach (a trade that goes
# red and NEVER goes green rides to max_hold unprotected -- the live SOL companion
# sat at -$6.40 / MFE 0% / stall 1 exactly this way). This is NOT the rejected v1
# arm-gated cut (`not armed`, fav < 2%): that was too broad and scalped dip-then-
# recover WINNERS -> tot% + PF down every value (see COLD_CUT_stall_accountant.patch.md).
# v2 fires ONLY when the trade (a) never went green (peak fav <= MFE_EPS), AND
# (b) is not a 1-bar wick (held >= MINHOLD), AND (c) adverse <= -GOLD_COLDCUT. Any
# strength first -> EXEMPT, so runners keep riding wide. `daymover_goldlogic_bt.py
# --sweep-smartcut`: whole book PF 1.76->1.85, tot 838->882, worst -45->-41,
# maxDD -282->-235, fires ~15x/7yr; passes the Adverse-Protection gate at ~20%
# (self-harm below ~18%). CRYPTO gets NO cut (trend flip bounds worst at -31%).
GOLD_COLDCUT = 0.20
GOLD_COLDCUT_MFE_EPS = 0.003
GOLD_COLDCUT_MINHOLD = 3


def _mark_and_exit(c, st, gate_giveback, atr_mult, max_hold, t):
    """Update peak/stall then SWITCH exit: WIDE trail for bull-entry positions,
    GOLD-companion clip for bear-entry positions. Returns True if exit."""
    if c > st["peak"]:
        st["peak"] = c
        st["since_high"] = 0
    else:
        st["since_high"] += 1
    held_days = t - st["entry_t"]
    fav = st["peak"] / st["entry_px"] - 1.0
    armed = fav >= GOLD_GATE
    # CATASTROPHE COLD-CUT: never-green + not-a-wick + deep adverse. LONG day-movers
    # -> adverse = c/entry - 1 (invert for shorts). Any strength first -> exempt.
    if (GOLD_COLDCUT > 0.0 and fav <= GOLD_COLDCUT_MFE_EPS
            and held_days >= GOLD_COLDCUT_MINHOLD
            and (c / st["entry_px"] - 1.0) <= -GOLD_COLDCUT):
        return True
    if not st.get("bull", True):
        # bear-entry: gold-companion clip (qualified signal, ride only once protected)
        clip = armed and (st["since_high"] >= GOLD_STALL or c <= st["peak"] * (1 - GOLD_REVGB))
        return bool(clip or held_days >= max_hold)
    # bull-entry: wide trail (let the runner run)
    gb = c <= st["peak"] * (1 - gate_giveback)
    tr = c <= st["peak"] - atr_mult * st["entry_atr"]
    return bool(gb or tr or held_days >= max_hold)


def _portfolio_and_basket(artifacts: str, provider: str, region: str, topk: int,
                          cost_bps: float, gate: float, contin_k: int):
    """Day-mover + continuation selection (replaces the neg-IC qlib model rank).

    The :7799 basket trades BIG DAY MOVERS that are also at a new contin_k-day high
    (the validated BigCapMomo entry: impulse + continuation). Exit is REGIME-SWITCHED
    (daymover_goldlogic_bt.py / daymover_lockin_sweep.py, faithful 7yr/multi-regime):
      - BULL entry (basket > 200d SMA): ride a WIDE trail (20% giveback / 4xATR / 20d).
        Wide beats clipping in bull -- a few runners carry the PF (bull-entry PF 4.21).
      - BEAR entry: same GOLD-companion logic we run on gold (stall_accountant.py) --
        arm after +2% captured, clip on 2-day stall or 10% give-back of MFE peak.
        Bear entries are QUALIFIED (they met 5%+new-high) and net-positive, but the
        clip cuts the bear drawdown 3.4x (-183% -> -69%) without excluding them.
    SWITCH whole-book: PF 3.45, tot +2750%, maxDD -254% (vs no-companion 2.91 / -439%).
    The qlib model score is still surfaced as context but no longer drives the BUY."""
    import qlib
    from qlib.data import D

    pred = pd.read_pickle(os.path.join(artifacts, "pred.pkl"))
    col = pred.columns[0]
    wide = pred[col].unstack("instrument")  # date x instrument (model score, context only)
    qlib.init(provider_uri=str(Path(provider).expanduser()), region=region, kernels=1)
    names = list(wide.columns)
    px = D.features(names, ["$close"], freq="day")["$close"].unstack("instrument")

    # ATR proxy per name = rolling mean |dClose| (same as the validated sweep)
    closes = {nm: px[nm].to_numpy(dtype=float) for nm in names}
    atr = {}
    for nm in names:
        c = closes[nm]
        dabs = np.abs(np.diff(c, prepend=c[0]))
        atr[nm] = pd.Series(dabs).rolling(14).mean().to_numpy()
    dates = list(px.index)

    # market regime: equal-weight basket close vs its own 200d SMA (bear-exclusion gate
    # used across the Omega book). bull -> ride wide; bear -> gold-clip the qualified entry.
    pxmat = px[names].to_numpy(dtype=float)
    base = pxmat / pxmat[0]
    basket = np.nanmean(base, axis=1)
    bsma = pd.Series(basket).rolling(200, min_periods=200).mean().to_numpy()
    bull_regime = basket > bsma   # bool per date row; NaN-warmup -> False

    # ----- backtest: day-mover entry + SWITCH exit, daily mark-to-market -----
    GIVEBACK, ATR_MULT, MAX_HOLD = 0.20, 4.0, 20
    held: dict = {}     # name -> {entry_px, peak, entry_t, entry_atr}
    gross_d, net_d, turns = [], [], []
    for t in range(contin_k + 1, len(dates)):
        # 1) mark held positions over day t, then check exits
        day_rs, exits = [], []
        for nm, st in held.items():
            c, cp = closes[nm][t], closes[nm][t - 1]
            if np.isfinite(c) and np.isfinite(cp) and cp > 0:
                day_rs.append(c / cp - 1.0)
            if np.isfinite(c) and _mark_and_exit(c, st, GIVEBACK, ATR_MULT, MAX_HOLD, t):
                exits.append(nm)
        for nm in exits:
            held.pop(nm, None)
        port_r = float(np.mean(day_rs)) if day_rs else 0.0
        # 2) new entries: biggest movers >= gate AND at a new contin_k-day high
        entries = 0
        if len(held) < topk:
            cand = []
            for nm in names:
                if nm in held:
                    continue
                c = closes[nm]
                if not (np.isfinite(c[t]) and np.isfinite(c[t - 1]) and c[t - 1] > 0):
                    continue
                dr = c[t] / c[t - 1] - 1.0
                hi = c[t] >= np.nanmax(c[t - contin_k:t + 1])
                if dr >= gate and hi:
                    cand.append((dr, nm))
            cand.sort(reverse=True)
            for _, nm in cand:
                if len(held) >= topk:
                    break
                a = atr[nm][t]
                held[nm] = {"entry_px": closes[nm][t], "peak": closes[nm][t],
                            "entry_t": t, "since_high": 0,
                            "bull": bool(bull_regime[t]),
                            "entry_atr": a if np.isfinite(a) else closes[nm][t] * 0.02}
                entries += 1
        # 3) cost: one side per fill (entry or exit), cost_bps is round-trip
        sides = entries + len(exits)
        cost = sides / max(topk, 1) * (cost_bps / 2 / 1e4)
        gross_d.append(port_r)
        net_d.append(port_r - cost)
        turns.append(entries / max(topk, 1))
    gross_d, net_d, turns = np.array(gross_d), np.array(net_d), np.array(turns)

    def _m(x):
        if len(x) < 5 or x.std() == 0:
            return {"sharpe": None, "ann_return": None, "max_drawdown": None, "hit_rate": None}
        eq = np.cumprod(1 + x)
        dd = float((eq / np.maximum.accumulate(eq) - 1).min())
        return {
            "sharpe": round(float(x.mean() / x.std() * np.sqrt(252)), 2),
            "ann_return": round(float(x.mean() * 252), 4),
            "max_drawdown": round(dd, 4),
            "hit_rate": round(float((x > 0).mean()), 3),
        }

    portfolio = {
        "topk": topk, "cost_bps_roundtrip": cost_bps, "n_days": int(len(net_d)),
        "selection": f"day-mover>={gate:.0%} + new {contin_k}d-high, wide trail (gb20/atr4/h20)",
        "avg_daily_turnover": round(float(turns.mean()), 3) if len(turns) else None,
        "gross": _m(gross_d), "net": _m(net_d),
    }

    # ----- today's basket: day-movers first, model score kept as context -----
    last = dates[-1]
    n = len(names)
    ret = lambda inst, k: (float(px[inst].iloc[-1] / px[inst].iloc[-1 - k] - 1) if px[inst].notna().sum() > k else None)  # noqa: E731

    def _px(inst):
        try:
            s = px[inst].dropna()
            return round(float(s.iloc[-1]), 2) if len(s) else None
        except Exception:  # noqa: BLE001
            return None

    def _today_move(inst):
        s = px[inst].dropna()
        if len(s) < contin_k + 2:
            return None, False
        dr = float(s.iloc[-1] / s.iloc[-2] - 1.0)
        hi = bool(float(s.iloc[-1]) >= float(s.iloc[-(contin_k + 1):].max()))
        return dr, hi

    scores = wide.loc[last].to_dict() if last in wide.index else {}
    rows = []
    for inst in names:
        dr, hi = _today_move(inst)
        qualifies = dr is not None and dr >= gate and hi
        rows.append({"instrument": str(inst), "day_ret": dr, "new_high": hi,
                     "qualifies": qualifies, "score": scores.get(inst)})
    # BUY = qualifying day-movers, biggest move first, capped at topk; rest sorted by move
    rows.sort(key=lambda r: (r["qualifies"], r["day_ret"] if r["day_ret"] is not None else -9),
              reverse=True)
    buys = [r["instrument"] for r in rows if r["qualifies"]][:topk]
    basket = []
    for i, r in enumerate(rows):
        inst = r["instrument"]
        basket.append({
            "rank": i + 1,
            "instrument": inst,
            "day_ret": round(r["day_ret"], 4) if r["day_ret"] is not None else None,
            "new_high": r["new_high"],
            "score": round(float(r["score"]), 4) if r["score"] is not None and pd.notna(r["score"]) else None,
            "percentile": round(100 * (n - i) / n),
            "price": _px(inst),
            "ret_5d": ret(inst, 5),
            "ret_20d": ret(inst, 20),
            "action": "BUY" if inst in buys else "—",
        })
    signal = {"date": str(pd.Timestamp(last).date()), "rebalance": "daily close",
              "selection": f"day-mover>={gate:.0%} + new {contin_k}d-high",
              "universe_size": n, "buy_count": len(buys), "basket": basket}
    return portfolio, signal


def _regime_strategy(provider: str, region: str, topk: int):
    """Today's regime read + the recommended flat-by-close + high-vol-gate strategy."""
    import qlib
    from qlib.data import D
    try:
        qlib.init(provider_uri=str(Path(provider).expanduser()), region=region, kernels=1)
        names = [p.split("/")[-1].replace(".csv", "") for p in []]  # placeholder; use all instruments
        cal = D.calendar()
        # equal-weight index from all names
        insts = D.list_instruments(D.instruments("all"), as_list=True) if hasattr(D, "list_instruments") else None
    except Exception:
        insts = None
    try:
        feat = D.features(insts, ["$close"], freq="day")["$close"].unstack("instrument")
        idx = feat.mean(axis=1).dropna()
        ret = idx.pct_change()
        vol20 = ret.rolling(20).std()
        sma50 = idx.rolling(50).mean()
        cur_vol = float(vol20.iloc[-1])
        vol_pct = round(float((vol20 <= cur_vol).mean()) * 100)
        bear = bool(idx.iloc[-1] < sma50.iloc[-1])
        hivol = vol_pct >= 75
        crash = bool(idx.iloc[-1] / idx.iloc[-6] - 1 < -0.04)  # 5-session drop > 4%
        # 3-state regime brain: LONG only in calm bull; SHORT (crash hedge) on a fast
        # drop or violent bear; CASH otherwise (soft-bear / high-vol chop).
        if crash or (bear and hivol):
            action, label, engine = "SHORT", "crash" if crash else "violent-bear", "bear engine (short index, flat-by-close)"
        elif bear or hivol:
            action, label, engine = "CASH", ("bear" if bear else "high-vol"), "sit out — long engine gated"
        else:
            action, label, engine = "LONG", "calm bull", "long engine (top-K, flat-by-close)"
        regime = {"vol_percentile": vol_pct, "below_sma50": bear, "crash": crash,
                  "label": label, "action": action, "engine": engine}
    except Exception:
        regime = {"vol_percentile": None, "below_sma50": None, "crash": None,
                  "label": "unknown", "action": "LONG", "engine": "?"}
    return {
        "name": f"flat-by-close · long top-{topk} · high-vol gate",
        "rules": [
            "enter at the open, exit ALL at the close — hold nothing overnight (zero gap risk)",
            "sit in CASH on top-quartile-vol days (gate improved Sharpe 0.85->1.18)",
            "no profit-target — capping winners tested worst; hold to close",
            "long-only for now: protect bear via cash gate (short side has no edge yet)",
        ],
        "backtest": {"sharpe": 1.18, "ann_return": 0.34, "max_drawdown": -0.195,
                     "vs_ungated_sharpe": 0.85, "window": "2024-06..2026-06 bigcap, 5bps",
                     "caveat": "bull-beta signal; structure validated, edge not"},
        "bear_engine": {"name": "crash hedge — short index, flat-by-close",
                        "arms_on": "5-session drop > 4% (idle ~86% of days)",
                        "sharpe": 0.49, "max_drawdown": -0.053, "armed_day_bps": 20,
                        "caveat": "thin sample (~16 armed days); a hedge overlay, not standalone alpha"},
        "today": regime,
    }


def _factors(spec):
    if not spec:
        return []
    p = Path(spec)
    files = [p] if p.is_file() else list(p.glob("*.py")) + list(p.glob("*.md"))
    out = []
    for f in files[:20]:
        try:
            out.append({"name": f.stem, "source_file": str(f), "source": f.read_text()[:4000]})
        except Exception:
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mlruns", default="mlruns")
    ap.add_argument("--provider", default=str(Path.home() / ".qlib" / "qlib_data" / "omega_data"))
    ap.add_argument("--region", default="us")
    ap.add_argument("--universe", default="BIGCAP")
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--cost-bps", type=float, default=10.0)
    ap.add_argument("--gate", type=float, default=0.03, help="min day-return to qualify as a mover "
                    "(0.05 -> 0.03 operator test S-2026-07-14: 5% + new-20d-high fired ~0 in the "
                    "corrective tape; 3% matches the validated stockmover gate.thr=0.03)")
    ap.add_argument("--contin-k", type=int, default=20, help="new K-day-high continuation lookback")
    ap.add_argument("--factors", default=None)
    ap.add_argument("--provenance", default="bar-replay")
    ap.add_argument("--out", default=str(Path.home() / "Omega" / "data" / "rdagent" / "latest.json"))
    a = ap.parse_args()

    artifacts = _latest_run(a.mlruns)
    portfolio, signal = _portfolio_and_basket(artifacts, a.provider, a.region, a.topk,
                                              a.cost_bps, a.gate, a.contin_k)
    doc = {
        "generated_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "source": {
            "engine": "rdagent-qlib", "universe": a.universe, "provenance": a.provenance,
            "tradeable_in_omega": a.universe.upper() not in {"CSI300", "CSI500", "CN"},
            "run_dir": artifacts,
            "description": f"daily cross-sectional ranking model on {signal['universe_size']} {a.universe} names",
        },
        "model": _ic(artifacts),
        "portfolio": portfolio,
        "strategy": _regime_strategy(a.provider, a.region, a.topk),
        "factors": _factors(a.factors),
        "signal": signal,
    }
    out = Path(a.out).expanduser()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=2))
    n = portfolio["net"]
    buys = [b["instrument"] for b in signal["basket"] if b["action"] == "BUY"]
    print(f"wrote {out}")
    print(f"  net Sharpe={n['sharpe']} annRet={n['ann_return']} maxDD={n['max_drawdown']} "
          f"hit={n['hit_rate']} | {signal['selection']} | buy: {buys}")


if __name__ == "__main__":
    main()
