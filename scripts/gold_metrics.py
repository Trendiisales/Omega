"""Performance metrics: Sharpe, Sortino, MDD, CAGR, Deflated Sharpe, hit/payoff."""
from __future__ import annotations
import math
import numpy as np
import pandas as pd


def _norm_cdf(x: float) -> float:
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def _norm_ppf(p: float) -> float:
    # Beasley-Springer-Moro approximation
    if p <= 0 or p >= 1:
        return float("nan")
    a = [-3.969683028665376e1, 2.209460984245205e2, -2.759285104469687e2,
         1.383577518672690e2, -3.066479806614716e1, 2.506628277459239]
    b = [-5.447609879822406e1, 1.615858368580409e2, -1.556989798598866e2,
         6.680131188771972e1, -1.328068155288572e1]
    c = [-7.784894002430293e-3, -3.223964580411365e-1, -2.400758277161838,
         -2.549732539343734, 4.374664141464968, 2.938163982698783]
    d = [7.784695709041462e-3, 3.224671290700398e-1, 2.445134137142996,
         3.754408661907416]
    plow = 0.02425
    phigh = 1 - plow
    if p < plow:
        q = math.sqrt(-2 * math.log(p))
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / \
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1)
    if p > phigh:
        q = math.sqrt(-2 * math.log(1 - p))
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / \
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1)
    q = p - 0.5
    r = q * q
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / \
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1)

BARS_PER_YEAR_5M = 252 * 24 * 12  # ~72576


def returns_from_equity(eq: pd.Series) -> pd.Series:
    return eq.pct_change().fillna(0.0)


def sharpe(returns: pd.Series, bars_per_year: float = BARS_PER_YEAR_5M) -> float:
    s = returns.std()
    if s == 0 or np.isnan(s):
        return 0.0
    return float(returns.mean() / s * np.sqrt(bars_per_year))


def sortino(returns: pd.Series, bars_per_year: float = BARS_PER_YEAR_5M) -> float:
    dn = returns[returns < 0]
    s = dn.std()
    if s == 0 or np.isnan(s):
        return 0.0
    return float(returns.mean() / s * np.sqrt(bars_per_year))


def max_drawdown(eq: pd.Series) -> float:
    peak = eq.cummax()
    dd = (eq - peak) / peak
    return float(dd.min())


def cagr(eq: pd.Series, bars_per_year: float = BARS_PER_YEAR_5M) -> float:
    n = len(eq)
    if n < 2 or eq.iloc[0] <= 0:
        return 0.0
    years = n / bars_per_year
    if years <= 0:
        return 0.0
    return float((eq.iloc[-1] / eq.iloc[0]) ** (1 / years) - 1)


def hit_rate(trades: pd.DataFrame) -> float:
    if len(trades) == 0:
        return 0.0
    return float((trades["pnl"] > 0).mean())


def payoff(trades: pd.DataFrame) -> float:
    if len(trades) == 0:
        return 0.0
    wins = trades.loc[trades["pnl"] > 0, "pnl"]
    losses = trades.loc[trades["pnl"] < 0, "pnl"]
    if len(losses) == 0 or losses.abs().mean() == 0:
        return float("inf")
    if len(wins) == 0:
        return 0.0
    return float(wins.mean() / losses.abs().mean())


def deflated_sharpe(sr: float, n_trials: int, n_obs: int,
                    skew: float = 0.0, kurt: float = 3.0) -> float:
    """Bailey & López de Prado Deflated Sharpe.
    Approximation: subtract expected-max SR under H0 from observed."""
    if n_trials < 2 or n_obs < 30:
        return float("nan")
    emc = 0.5772156649
    e_max = (1 - emc) * _norm_ppf(1 - 1 / n_trials) + emc * _norm_ppf(1 - 1 / (n_trials * np.e))
    sr_std = np.sqrt((1 - skew * sr + (kurt - 1) / 4 * sr ** 2) / (n_obs - 1))
    if sr_std == 0:
        return float("nan")
    z = (sr - e_max * sr_std) / sr_std
    return float(_norm_cdf(z))


def session_breakdown(bar_pnl: pd.Series, sessions: pd.Series) -> pd.DataFrame:
    df = pd.DataFrame({"pnl": bar_pnl, "session": sessions.reindex(bar_pnl.index)})
    g = df.groupby("session")["pnl"]
    return pd.DataFrame({
        "bars": g.count(),
        "sum_pnl": g.sum(),
        "mean_bar_pnl": g.mean(),
        "win_bars_pct": (df.groupby("session")["pnl"].apply(lambda x: (x > 0).mean()) * 100),
    })


def report(name: str, equity: pd.Series, bar_pnl: pd.Series,
           trades: pd.DataFrame | None = None, n_trials: int = 1,
           bars_per_year: float = BARS_PER_YEAR_5M,
           sessions: pd.Series | None = None) -> dict:
    rets = returns_from_equity(equity)
    sr = sharpe(rets, bars_per_year)
    so = sortino(rets, bars_per_year)
    mdd = max_drawdown(equity)
    cg = cagr(equity, bars_per_year)
    total_pnl = float(equity.iloc[-1] - equity.iloc[0])
    pct = float(total_pnl / equity.iloc[0])
    out = {
        "name": name,
        "total_pnl_usd": total_pnl,
        "total_return_pct": pct * 100,
        "cagr_pct": cg * 100,
        "sharpe": sr,
        "sortino": so,
        "max_dd_pct": mdd * 100,
        "bars": len(equity),
    }
    if trades is not None and len(trades) > 0:
        out["n_trades"] = int(len(trades))
        out["hit_rate_pct"] = hit_rate(trades) * 100
        out["payoff"] = payoff(trades)
        out["avg_trade_pnl"] = float(trades["pnl"].mean())
        out["mean_bars_held"] = float(trades["bars_held"].mean())
    else:
        out["n_trades"] = 0
    if n_trials > 1:
        out["deflated_sr"] = deflated_sharpe(sr, n_trials, len(equity),
                                             rets.skew(), rets.kurt() + 3)
    if sessions is not None:
        out["session_breakdown"] = session_breakdown(bar_pnl, sessions)
    return out
