#!/usr/bin/env python3
"""
S37 step (B + C) — robustness checks for the XAUUSD long Mon finding.

(B) Cross-symbol Monday robustness:
    For each (symbol, mode) pair, compute Mon-entry vs all-other-days
    stats and a one-sided permutation p-value (Mon >= random same-size
    cohort). Then combine the 10 p-values via Fisher's method to ask
    "is there a cross-symbol Monday pattern at all?" If only XAUUSD
    long shows it, the finding is a single-instance fluke. If half
    the symbol-modes lean the same way, there's a real market-
    structure story (weekend gap, week-open positioning, etc.).

(C) Trade-level forensics on XAUUSD long Mon:
    Dump all Mon trades sorted by PnL. Compute concentration (top-3
    fraction of total), median, mean, trimmed mean (10% each tail),
    and verify the PF/winrate/PnL math reconciles. If the +$97k is
    carried by 2-3 huge winners, the effect is fat-tailed and
    fragile. If it's spread across the cohort, it's a structural
    edge.

Inputs: same trades.csv files as permutation_test_s35.py.
Output: cross_symbol_mon_results.txt in --output-dir.

Usage:
    python3 cross_symbol_mon_test.py
    python3 cross_symbol_mon_test.py --trades-dir ~/Omega/backtest \\
            --n-perm 10000 --output-dir .
"""

from __future__ import annotations

import argparse
import csv
import math
import random
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


# ---------------------------------------------------------------------------
# data model (matches permutation_test_s35.py)
# ---------------------------------------------------------------------------

@dataclass
class Trade:
    symbol: str
    mode: str
    window: str
    direction: str
    entry_ts: datetime
    pnl: float
    bars_held: int
    atr_at_entry: float


def parse_ts(s: str) -> datetime:
    s = s.strip()
    if s.endswith("Z"):
        s = s[:-1]
    if "." in s:
        s = s.split(".", 1)[0]
    if "T" in s:
        return datetime.strptime(s, "%Y-%m-%dT%H:%M:%S")
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S")


def load_trades(trades_dir: Path) -> list[Trade]:
    trades: list[Trade] = []
    if not trades_dir.is_dir():
        sys.exit(f"trades dir not found: {trades_dir}")

    for path in sorted(trades_dir.iterdir()):
        name = path.name
        if not name.endswith("_trades.csv"):
            continue
        if not name.startswith("walkforward_b"):
            continue
        stem = name[: -len("_trades.csv")]
        rest = stem[len("walkforward_b"):]
        if rest.startswith("_long_"):
            mode = "long"
            sym = rest[len("_long_"):]
        elif rest.startswith("_"):
            mode = "base"
            sym = rest[1:]
        else:
            continue
        if not sym:
            continue

        with path.open() as f:
            for row in csv.DictReader(f):
                try:
                    trades.append(Trade(
                        symbol=sym,
                        mode=mode,
                        window=row.get("window", ""),
                        direction=row["direction"].strip(),
                        entry_ts=parse_ts(row["entry_ts"]),
                        pnl=float(row["pnl"]),
                        bars_held=int(row["bars_held"]),
                        atr_at_entry=float(row.get("atr_at_entry", "0") or 0.0),
                    ))
                except (KeyError, ValueError) as e:
                    print(f"WARN: skipping bad row in {name}: {e}", file=sys.stderr)
    return trades


# ---------------------------------------------------------------------------
# stats helpers
# ---------------------------------------------------------------------------

def pf(pnls: list[float]) -> float:
    wins = sum(p for p in pnls if p > 0)
    losses = -sum(p for p in pnls if p < 0)
    if losses == 0.0:
        return float("inf") if wins > 0 else 0.0
    return wins / losses


def winrate(pnls: list[float]) -> float:
    if not pnls:
        return 0.0
    return sum(1 for p in pnls if p > 0) / len(pnls)


def mean(xs: list[float]) -> float:
    return sum(xs) / len(xs) if xs else 0.0


def median(xs: list[float]) -> float:
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return 0.0
    if n % 2 == 1:
        return s[n // 2]
    return 0.5 * (s[n // 2 - 1] + s[n // 2])


def trimmed_mean(xs: list[float], trim_frac: float) -> float:
    """Symmetric-trim mean: drop trim_frac from each tail."""
    if not xs:
        return 0.0
    s = sorted(xs)
    k = int(len(s) * trim_frac)
    if 2 * k >= len(s):
        return median(s)
    trimmed = s[k: len(s) - k]
    return sum(trimmed) / len(trimmed)


# ---------------------------------------------------------------------------
# (B) per-symbol Monday permutation
# ---------------------------------------------------------------------------

def perm_p_mon(parent_pnls: list[float], mon_pnl_sum: float, mon_n: int,
               n_perm: int, rng: random.Random) -> float:
    """One-sided p: P(random n-sized draw from parent has sum >= mon_pnl_sum)."""
    if mon_n > len(parent_pnls) or mon_n == 0:
        return 1.0
    ge = 0
    for _ in range(n_perm):
        sample = rng.sample(parent_pnls, mon_n)
        if sum(sample) >= mon_pnl_sum:
            ge += 1
    return (ge + 1) / (n_perm + 1)


def fisher_combined_p(pvals: list[float]) -> tuple[float, float]:
    """Fisher's method for combining independent p-values. Returns
    (chi2_statistic, combined_p). Falls back gracefully if any p=0
    by clipping to 1/(n_perm+1) range — caller should pass in
    already-clipped p-values from the (k+1)/(N+1) estimator."""
    if not pvals:
        return (0.0, 1.0)
    chi2 = -2.0 * sum(math.log(max(p, 1e-300)) for p in pvals)
    # df = 2k
    k = len(pvals)
    # survival function of chi-squared with 2k df at chi2
    # for 2k df: P(X >= x) = e^(-x/2) * sum_{i=0}^{k-1} (x/2)^i / i!
    half = chi2 / 2.0
    if half <= 0:
        return (chi2, 1.0)
    log_sum_term = -half
    sum_terms = 0.0
    log_fact = 0.0
    half_pow = 1.0
    for i in range(k):
        if i > 0:
            log_fact += math.log(i)
            half_pow *= half
        # term = half_pow / i!
        # to avoid underflow, work in log space then exp at end
        sum_terms += math.exp(math.log(max(half_pow, 1e-300)) - log_fact)
    p = math.exp(log_sum_term) * sum_terms
    return (chi2, max(0.0, min(1.0, p)))


def cross_symbol_monday(trades: list[Trade], n_perm: int, rng: random.Random,
                        out: list[str]) -> list[dict]:
    out.append("")
    out.append("=" * 78)
    out.append("(B) CROSS-SYMBOL MONDAY ROBUSTNESS")
    out.append("=" * 78)
    out.append("")
    out.append("For each (symbol, mode) pair, compares Monday-entry trades vs all")
    out.append("other days and computes one-sided permutation p (Mon sum-PnL >=")
    out.append("random same-size draw from full pair pool, n_perm={0}).".format(n_perm))
    out.append("")
    out.append(f"{'symbol':<8} {'mode':<5} {'mon_n':>5} {'mon_pnl':>11} "
               f"{'mon_mean':>9} {'mon_pf':>6} {'mon_wr':>6} "
               f"{'rest_mean':>9} {'rest_pf':>6} {'p_perm':>7}")
    out.append("-" * 78)

    by_sm: dict[tuple[str, str], list[Trade]] = {}
    for t in trades:
        by_sm.setdefault((t.symbol, t.mode), []).append(t)

    rows: list[dict] = []
    for (sym, mode), ts in sorted(by_sm.items()):
        if len(ts) < 30:
            continue
        mon = [t for t in ts if t.entry_ts.weekday() == 0]
        rest = [t for t in ts if t.entry_ts.weekday() != 0]
        if not mon:
            out.append(f"{sym:<8} {mode:<5} {0:>5} (no Monday entries)")
            continue

        mon_pnls = [t.pnl for t in mon]
        rest_pnls = [t.pnl for t in rest]
        all_pnls = [t.pnl for t in ts]
        mon_sum = sum(mon_pnls)
        p_perm = perm_p_mon(all_pnls, mon_sum, len(mon), n_perm, rng) \
                 if len(mon) < len(ts) else 1.0

        rows.append({
            "symbol": sym, "mode": mode, "mon_n": len(mon),
            "mon_pnl_sum": mon_sum,
            "mon_mean": mean(mon_pnls),
            "mon_pf": pf(mon_pnls),
            "mon_wr": winrate(mon_pnls),
            "rest_n": len(rest),
            "rest_mean": mean(rest_pnls),
            "rest_pf": pf(rest_pnls),
            "p_perm": p_perm,
        })
        pf_str = f"{pf(mon_pnls):>6.2f}" if pf(mon_pnls) != float("inf") else "  inf "
        rest_pf_str = f"{pf(rest_pnls):>6.2f}" if pf(rest_pnls) != float("inf") else "  inf "
        out.append(f"{sym:<8} {mode:<5} {len(mon):>5} {mon_sum:>+11.1f} "
                   f"{mean(mon_pnls):>+9.1f} {pf_str} {winrate(mon_pnls):>6.3f} "
                   f"{mean(rest_pnls):>+9.1f} {rest_pf_str} {p_perm:>7.4f}")

    # cross-symbol meta-analysis
    out.append("")
    out.append("CROSS-SYMBOL META-ANALYSIS")
    out.append("-" * 78)

    # how many lean Mon-positive (Mon mean > rest mean)?
    lean_pos = sum(1 for r in rows if r["mon_mean"] > r["rest_mean"])
    lean_neg = sum(1 for r in rows if r["mon_mean"] < r["rest_mean"])
    out.append(f"  symbol-modes with Mon mean > rest mean: {lean_pos} / {len(rows)}")
    out.append(f"  symbol-modes with Mon mean < rest mean: {lean_neg} / {len(rows)}")
    out.append("  (under H0 of no Mon effect, expected ~50/50)")
    # binomial-ish sanity: P(>= lean_pos out of n by chance, p=0.5)
    n_total = len(rows)
    if n_total > 0:
        # exact one-sided binomial tail
        from math import comb
        binom_p = sum(comb(n_total, k) for k in range(lean_pos, n_total + 1)) / (2 ** n_total)
        out.append(f"  one-sided binomial p (lean_pos >= {lean_pos} | p=0.5): {binom_p:.4f}")

    # Fisher's combined p across the 10 permutation p-values
    pvals = [r["p_perm"] for r in rows]
    chi2, combined = fisher_combined_p(pvals)
    out.append(f"  Fisher's combined p (across {len(pvals)} symbol-modes): "
               f"{combined:.4f}  (chi2={chi2:.2f}, df={2*len(pvals)})")
    out.append("")
    out.append("  Interpretation:")
    out.append("    - If combined p << 0.05: there's a cross-symbol Monday tilt.")
    out.append("      XAUUSD long Mon is one expression of a real market structure.")
    out.append("    - If combined p ~ 0.5: the 10 p-values look uniformly random.")
    out.append("      XAUUSD long Mon is a single fluke amid otherwise-null results.")
    out.append("    - The binomial sign test is a non-parametric backup: does the")
    out.append("      DIRECTION of the Mon effect cluster across symbols?")

    return rows


# ---------------------------------------------------------------------------
# (C) XAUUSD long Mon trade-level forensics
# ---------------------------------------------------------------------------

def xauusd_mon_forensics(trades: list[Trade], out: list[str]) -> None:
    out.append("")
    out.append("=" * 78)
    out.append("(C) XAUUSD LONG MON — TRADE-LEVEL FORENSICS")
    out.append("=" * 78)
    out.append("")

    cohort = [t for t in trades
              if t.symbol == "XAUUSD" and t.mode == "long"
              and t.entry_ts.weekday() == 0]
    if not cohort:
        out.append("No XAUUSD long Mon trades found — abort.")
        return

    pnls = sorted((t.pnl for t in cohort), reverse=True)
    total = sum(pnls)
    n = len(pnls)
    wins = [p for p in pnls if p > 0]
    losses = [p for p in pnls if p <= 0]

    out.append(f"  cohort size      : {n}")
    out.append(f"  total pnl        : {total:+.2f}")
    out.append(f"  mean             : {mean(pnls):+.2f}")
    out.append(f"  median           : {median(pnls):+.2f}")
    out.append(f"  trimmed mean(10%): {trimmed_mean(pnls, 0.10):+.2f}")
    out.append(f"  trimmed mean(20%): {trimmed_mean(pnls, 0.20):+.2f}")
    out.append(f"  winners          : {len(wins)} / {n} = {len(wins)/n:.3f}")
    out.append(f"  losers           : {len(losses)} / {n} = {len(losses)/n:.3f}")
    out.append(f"  pf               : {pf(pnls):.3f}")
    out.append(f"  win sum / loss sum : {sum(wins):+.1f} / {sum(losses):+.1f}")

    out.append("")
    out.append("CONCENTRATION ANALYSIS")
    out.append("-" * 78)
    cum = 0.0
    out.append(f"  top 1: {pnls[0]:>+11.1f}  ({pnls[0]/total*100 if total else 0:>5.1f}% of total)")
    cum = pnls[0]
    out.append(f"  top 3 cum: {sum(pnls[:3]):>+11.1f}  "
               f"({sum(pnls[:3])/total*100 if total else 0:>5.1f}% of total)")
    out.append(f"  top 5 cum: {sum(pnls[:5]):>+11.1f}  "
               f"({sum(pnls[:5])/total*100 if total else 0:>5.1f}% of total)")
    out.append(f"  median trade: {median(pnls):>+11.1f}")

    # Recompute total without top-1, top-3 to see how fragile
    if n > 1:
        without_top1 = total - pnls[0]
        without_top3 = total - sum(pnls[:3])
        out.append(f"  total without top-1: {without_top1:+.1f}")
        out.append(f"  total without top-3: {without_top3:+.1f}")

    # PF/WR/PnL math reconciliation
    out.append("")
    out.append("PF / WR / PnL MATH RECONCILIATION")
    out.append("-" * 78)
    avg_win = mean(wins) if wins else 0.0
    avg_loss = mean(losses) if losses else 0.0
    out.append(f"  avg win  : {avg_win:+.2f}")
    out.append(f"  avg loss : {avg_loss:+.2f}")
    out.append(f"  win/loss ratio: "
               f"{abs(avg_win/avg_loss) if avg_loss else float('inf'):.2f}")
    out.append(f"  expected pf from wr+ratio: "
               f"{(len(wins)*avg_win) / (-len(losses)*avg_loss) if (losses and avg_loss) else float('inf'):.3f}")
    out.append(f"  computed pf: {pf(pnls):.3f}")
    out.append("  (these should match within rounding)")

    # Full sorted dump
    out.append("")
    out.append("ALL TRADES, SORTED BY PnL DESCENDING")
    out.append("-" * 78)
    out.append(f"  {'rank':>4}  {'pnl':>11}  {'entry_ts':<19}  "
               f"{'bars':>4}  {'window':<6}")
    cohort_sorted = sorted(cohort, key=lambda t: -t.pnl)
    for rank, t in enumerate(cohort_sorted, 1):
        out.append(f"  {rank:>4}  {t.pnl:>+11.1f}  "
                   f"{t.entry_ts.strftime('%Y-%m-%d %H:%M:%S')}  "
                   f"{t.bars_held:>4}  {t.window:<6}")

    out.append("")
    out.append("FORENSICS VERDICT GUIDE")
    out.append("-" * 78)
    out.append("  - top-3 share > 60%: edge is fat-tail, fragile to a single bad")
    out.append("    week. Cannot deploy as-is.")
    out.append("  - top-3 share 30-60%: typical for trend-following, edge plausibly")
    out.append("    structural but needs sample-size confirmation.")
    out.append("  - top-3 share < 30%: edge is broad-based, the strongest possible")
    out.append("    sign of a real Mon effect on XAUUSD long.")
    out.append("  - trimmed mean(10%) flipping sign vs raw mean: the effect lives")
    out.append("    entirely in the tails. Treat as zero edge.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trades-dir",
                    default=str(Path.home() / "Omega" / "backtest"))
    ap.add_argument("--output-dir", default=".")
    ap.add_argument("--n-perm", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    trades_dir = Path(args.trades_dir).expanduser()
    output_dir = Path(args.output_dir).expanduser()
    output_dir.mkdir(parents=True, exist_ok=True)

    trades = load_trades(trades_dir)
    if not trades:
        sys.exit("no trades loaded")
    print(f"loaded {len(trades)} trades across "
          f"{len({(t.symbol, t.mode) for t in trades})} (symbol, mode) pairs")

    rng = random.Random(args.seed)
    out: list[str] = []
    out.append("S37 step (B+C) — cross-symbol Monday robustness + XAUUSD forensics")
    out.append("=" * 78)
    out.append(f"  trades dir : {trades_dir}")
    out.append(f"  total trades: {len(trades)}")
    out.append(f"  n_perm     : {args.n_perm}")
    out.append(f"  seed       : {args.seed}")

    cross_symbol_monday(trades, args.n_perm, rng, out)
    xauusd_mon_forensics(trades, out)

    text = "\n".join(out)
    output_path = output_dir / "cross_symbol_mon_results.txt"
    output_path.write_text(text)
    print(text)
    print(f"\n=> wrote {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
