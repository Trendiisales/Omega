#!/usr/bin/env python3
"""
Promote gate: does an RD-Agent signal survive Omega bigcap trading costs?

Takes a finished qlib run's prediction, forms the daily top-K equal-weight
basket on the Omega universe, applies a realistic IBKR bigcap round-trip cost
to turnover, and reports NET metrics + a PASS/FAIL verdict.

This is the cost-aware portfolio BT for a DAILY-rebalance equity strategy
(fills at close ± slippage). It is the honest cost gate short of Omega's C++
tick replay — for a daily strategy there is no intraday tick fill to simulate.
A PASS here means "worth wiring into Omega as a shadow sleeve"; it is NOT a
deploy authorization (deploy stays gated, operator-driven).

    python promote_faithful_bt.py --mlruns <dir> --provider ~/.qlib/qlib_data/omega_data \
        [--topk 5] [--cost-bps 10] [--out ~/Omega/data/rdagent/verdict.json]
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
import qlib
from qlib.data import D


def _latest_pred(mlruns: str) -> str:
    preds = glob.glob(os.path.join(mlruns, "**", "pred.pkl"), recursive=True)
    if not preds:
        raise SystemExit(f"no pred.pkl under {mlruns}")
    return max(preds, key=os.path.getmtime)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mlruns", required=True)
    ap.add_argument("--provider", default=str(Path.home() / ".qlib" / "qlib_data" / "omega_data"))
    ap.add_argument("--region", default="us")
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--cost-bps", type=float, default=10.0, help="round-trip cost per unit turnover, bps (IBKR bigcap ~10)")
    ap.add_argument("--sharpe-pass", type=float, default=0.5)
    ap.add_argument("--out", default=str(Path.home() / "Omega" / "data" / "rdagent" / "verdict.json"))
    a = ap.parse_args()

    pred = pd.read_pickle(_latest_pred(a.mlruns))
    col = pred.columns[0]
    pred = pred[col].unstack("instrument")  # rows=date, cols=instrument

    qlib.init(provider_uri=str(Path(a.provider).expanduser()), region=a.region, kernels=1)
    names = list(pred.columns)
    px = D.features(names, ["$close"], freq="day")["$close"].unstack("instrument")
    fwd = px.shift(-1) / px - 1.0  # next-day close-to-close return realized by a position held from t to t+1

    dates = [d for d in pred.index if d in fwd.index]
    prev: set[str] = set()
    rets, turns = [], []
    for d in dates:
        row = pred.loc[d].dropna()
        if row.empty:
            continue
        picks = list(row.sort_values(ascending=False).head(a.topk).index)
        r = fwd.loc[d, picks].mean()
        if pd.isna(r):
            continue
        turnover = len(set(picks) ^ prev) / max(len(picks), 1)  # symmetric diff / K
        rets.append(float(r))
        turns.append(float(turnover))
        prev = set(picks)

    rets = np.array(rets)
    turns = np.array(turns)
    cost = turns * (a.cost_bps / 1e4)
    net = rets - cost

    def _metrics(x: np.ndarray) -> dict:
        if len(x) < 5 or x.std() == 0:
            return {"ann_return": None, "sharpe": None, "max_drawdown": None}
        eq = np.cumprod(1 + x)
        dd = (eq / np.maximum.accumulate(eq) - 1).min()
        return {
            "ann_return": round(float(x.mean() * 252), 4),
            "sharpe": round(float(x.mean() / x.std() * np.sqrt(252)), 3),
            "max_drawdown": round(float(dd), 4),
        }

    gross_m, net_m = _metrics(rets), _metrics(net)
    passed = bool(net_m["sharpe"] is not None and net_m["sharpe"] >= a.sharpe_pass and (net_m["ann_return"] or 0) > 0)

    verdict = {
        "generated_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "universe": "OMEGA-BIGCAP",
        "topk": a.topk,
        "cost_bps_roundtrip": a.cost_bps,
        "avg_daily_turnover": round(float(turns.mean()), 3) if len(turns) else None,
        "n_days": len(net),
        "gross": gross_m,
        "net": net_m,
        "verdict": "PASS" if passed else "FAIL",
        "note": "cost-aware daily portfolio BT; PASS => wire as Omega shadow sleeve, NOT a deploy authorization",
    }
    out = Path(a.out).expanduser()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(verdict, indent=2))
    print(json.dumps(verdict, indent=2))


if __name__ == "__main__":
    main()
