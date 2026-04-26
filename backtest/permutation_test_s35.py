#!/usr/bin/env python3
"""
S37 — Permutation test of the three S35 findings.

Re-tests these three S35 candidates with:
  (1) Larger permutation sample (N=10,000 vs S35's 5,000)
  (2) Two test statistics: sum-PnL (S35 used this) and PF
  (3) Block bootstrap as a second null model that preserves local
      autocorrelation, in case trade outcomes are not i.i.d.
  (4) Family-wise correction acknowledging the full set of S35 candidates
      (Bonferroni and Benjamini-Hochberg FDR), not just these three.

The three findings under test:
  1. GER40 base, ATR-LOW (n=54, +$204,    PF 1.87, S35 p=0.031)
  2. GER40 long, ATR-LOW (n=26, +$192,    PF 3.35, S35 p=0.032)
  3. XAUUSD long, Mon    (n=26, +$73000,  PF 4.35, S35 p=0.0076)

Per S35, "ATR-LOW" = bottom tertile of atr_at_entry within combined OOS
trades for that (symbol, mode). "Mon" = entry_ts.weekday() == 0.

Inputs (read from --trades-dir, default ~/Omega/backtest):
  walkforward_b_<SYM>_trades.csv          — base mode
  walkforward_b_long_<SYM>_trades.csv     — long-only mode

Output: permutation_test_s37_results.txt in --output-dir (default cwd).

Usage:
    python3 permutation_test_s35.py
    python3 permutation_test_s35.py --trades-dir ~/Omega/backtest --n-perm 10000
    python3 permutation_test_s35.py --output-dir /tmp
"""

from __future__ import annotations

import argparse
import csv
import os
import random
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable


# ---------------------------------------------------------------------------
# data model
# ---------------------------------------------------------------------------

@dataclass
class Trade:
    symbol: str
    mode: str            # "base" or "long"
    window: str
    direction: str       # "long" or "short"
    entry_ts: datetime
    pnl: float
    bars_held: int
    atr_at_entry: float


def parse_ts(s: str) -> datetime:
    """walkforward_b emits ISO-like '2024-01-02 03:04:05' UTC timestamps."""
    s = s.strip()
    # tolerate trailing 'Z' or fractional seconds
    if s.endswith("Z"):
        s = s[:-1]
    if "." in s:
        s = s.split(".", 1)[0]
    if "T" in s:
        return datetime.strptime(s, "%Y-%m-%dT%H:%M:%S")
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S")


def load_trades(trades_dir: Path) -> list[Trade]:
    """Read all 10 walkforward_b[_long]_<SYM>_trades.csv files."""
    trades: list[Trade] = []
    if not trades_dir.is_dir():
        sys.exit(f"trades dir not found: {trades_dir}")

    for path in sorted(trades_dir.iterdir()):
        name = path.name
        if not name.endswith("_trades.csv"):
            continue
        if not name.startswith("walkforward_b"):
            continue

        # parse: walkforward_b_<SYM>_trades.csv          -> mode=base
        #        walkforward_b_long_<SYM>_trades.csv     -> mode=long
        stem = name[: -len("_trades.csv")]      # walkforward_b_<...>
        rest = stem[len("walkforward_b") :]     # _<SYM>  or  _long_<SYM>
        if rest.startswith("_long_"):
            mode = "long"
            sym = rest[len("_long_") :]
        elif rest.startswith("_"):
            mode = "base"
            sym = rest[1:]
        else:
            continue
        if not sym:
            continue

        with path.open() as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    trades.append(
                        Trade(
                            symbol=sym,
                            mode=mode,
                            window=row.get("window", ""),
                            direction=row["direction"].strip(),
                            entry_ts=parse_ts(row["entry_ts"]),
                            pnl=float(row["pnl"]),
                            bars_held=int(row["bars_held"]),
                            atr_at_entry=float(row.get("atr_at_entry", "0") or 0.0),
                        )
                    )
                except (KeyError, ValueError) as e:
                    # ignore the (rare) malformed row, but tell the user
                    print(f"WARN: skipping bad row in {name}: {e}", file=sys.stderr)
    return trades


# ---------------------------------------------------------------------------
# stats helpers
# ---------------------------------------------------------------------------

def pf(pnls: list[float]) -> float:
    """Profit factor: sum of wins / |sum of losses|. Inf if no losses."""
    wins = sum(p for p in pnls if p > 0)
    losses = -sum(p for p in pnls if p < 0)
    if losses == 0.0:
        return float("inf") if wins > 0 else 0.0
    return wins / losses


def winrate(pnls: list[float]) -> float:
    if not pnls:
        return 0.0
    return sum(1 for p in pnls if p > 0) / len(pnls)


# ---------------------------------------------------------------------------
# null distributions
# ---------------------------------------------------------------------------

def perm_iid(parent_pnls: list[float], n: int, n_perm: int, rng: random.Random
             ) -> tuple[list[float], list[float]]:
    """I.i.d. permutation: draw n parent_pnls without replacement, n_perm times.
    Returns (sum_pnl_samples, pf_samples)."""
    if n > len(parent_pnls):
        return [], []
    pnl_samples: list[float] = []
    pf_samples: list[float] = []
    parent = parent_pnls  # alias
    for _ in range(n_perm):
        sample = rng.sample(parent, n)
        pnl_samples.append(sum(sample))
        pf_samples.append(pf(sample))
    return pnl_samples, pf_samples


def perm_block(parent_pnls: list[float], n: int, n_perm: int, block: int,
               rng: random.Random) -> tuple[list[float], list[float]]:
    """Block bootstrap: resample contiguous blocks of length `block` (with
    replacement) until we have >= n trades, then truncate to n. Preserves
    any short-range autocorrelation that i.i.d. permutation destroys.

    Note this is sampling WITH replacement of blocks — it tests against a
    different null than perm_iid (closer to the bootstrap notion of
    'samples from the same distribution' rather than 'subset of the
    same population'). Reported alongside perm_iid for triangulation,
    not as a replacement for it."""
    L = len(parent_pnls)
    if n > L or block < 1 or block > L:
        return [], []
    n_blocks = (n + block - 1) // block
    pnl_samples: list[float] = []
    pf_samples: list[float] = []
    max_start = L - block
    for _ in range(n_perm):
        sample: list[float] = []
        for _ in range(n_blocks):
            start = rng.randint(0, max_start)
            sample.extend(parent_pnls[start : start + block])
        sample = sample[:n]
        pnl_samples.append(sum(sample))
        pf_samples.append(pf(sample))
    return pnl_samples, pf_samples


def one_sided_p(observed: float, null_samples: list[float]) -> float:
    """P(null >= observed) under the empirical null. One-sided, upper tail.
    Uses (k+1)/(N+1) to avoid p=0 when no null sample beats observed."""
    if not null_samples:
        return 1.0
    ge = sum(1 for x in null_samples if x >= observed)
    return (ge + 1) / (len(null_samples) + 1)


# ---------------------------------------------------------------------------
# candidate enumeration (must match find_edges.py exactly)
# ---------------------------------------------------------------------------

def enumerate_s35_candidates(trades: list[Trade]) -> list[tuple[str, list[Trade], list[Trade]]]:
    """Reproduce the exact candidate list S35 tested. Used to compute the
    family-wise correction denominator."""
    by_sm: dict[tuple[str, str], list[Trade]] = {}
    for t in trades:
        by_sm.setdefault((t.symbol, t.mode), []).append(t)

    candidates: list[tuple[str, list[Trade], list[Trade]]] = []

    # Direction (skipped for long-only because cohort==parent)
    for (sym, mode), ts in by_sm.items():
        if len(ts) < 30:
            continue
        for direction in ("long", "short"):
            cohort = [t for t in ts if t.direction == direction]
            if len(cohort) >= 20 and len(cohort) != len(ts):
                candidates.append((f"{sym} {mode} dir={direction}", cohort, ts))

    # ATR tertiles
    for (sym, mode), ts in by_sm.items():
        valid = [t for t in ts if t.atr_at_entry > 0]
        if len(valid) < 30:
            continue
        atrs = sorted(t.atr_at_entry for t in valid)
        n = len(atrs)
        lo_thr = atrs[n // 3]
        hi_thr = atrs[2 * n // 3]
        low = [t for t in valid if t.atr_at_entry < lo_thr]
        high = [t for t in valid if t.atr_at_entry >= hi_thr]
        if len(low) >= 20 and len(low) != len(valid):
            candidates.append((f"{sym} {mode} atr=LOW", low, valid))
        if len(high) >= 20 and len(high) != len(valid):
            candidates.append((f"{sym} {mode} atr=HIGH", high, valid))

    # bars_held cohorts
    for (sym, mode), ts in by_sm.items():
        if len(ts) < 30:
            continue
        fast = [t for t in ts if t.bars_held <= 3]
        slow = [t for t in ts if t.bars_held > 10]
        if len(fast) >= 20 and len(fast) != len(ts):
            candidates.append((f"{sym} {mode} bars=FAST(<=3)", fast, ts))
        if len(slow) >= 20 and len(slow) != len(ts):
            candidates.append((f"{sym} {mode} bars=SLOW(>10)", slow, ts))

    # day-of-week
    day_names = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
    for (sym, mode), ts in by_sm.items():
        if len(ts) < 30:
            continue
        for dow in range(7):
            day = [t for t in ts if t.entry_ts.weekday() == dow]
            if len(day) >= 20 and len(day) != len(ts):
                candidates.append((f"{sym} {mode} {day_names[dow]}", day, ts))

    return candidates


# ---------------------------------------------------------------------------
# the three S35 findings under test
# ---------------------------------------------------------------------------

def select_atr_low(trades: list[Trade], symbol: str, mode: str
                   ) -> tuple[list[Trade], list[Trade]]:
    """Return (cohort, parent) for ATR-LOW tertile within (symbol, mode)."""
    sm = [t for t in trades if t.symbol == symbol and t.mode == mode]
    valid = [t for t in sm if t.atr_at_entry > 0]
    if len(valid) < 30:
        return [], valid
    atrs = sorted(t.atr_at_entry for t in valid)
    lo_thr = atrs[len(atrs) // 3]
    cohort = [t for t in valid if t.atr_at_entry < lo_thr]
    return cohort, valid


def select_dow(trades: list[Trade], symbol: str, mode: str, dow: int
               ) -> tuple[list[Trade], list[Trade]]:
    """Return (cohort, parent) for day-of-week within (symbol, mode)."""
    sm = [t for t in trades if t.symbol == symbol and t.mode == mode]
    cohort = [t for t in sm if t.entry_ts.weekday() == dow]
    return cohort, sm


# ---------------------------------------------------------------------------
# multiple-comparisons correction
# ---------------------------------------------------------------------------

def benjamini_hochberg(pvals: list[float], alpha: float = 0.05) -> list[bool]:
    """Return reject-flags using BH FDR control at level alpha."""
    n = len(pvals)
    if n == 0:
        return []
    indexed = sorted(enumerate(pvals), key=lambda x: x[1])
    reject = [False] * n
    # find largest k s.t. p_(k) <= k/n * alpha; reject all up to that rank
    largest_k = -1
    for rank, (_, p) in enumerate(indexed, 1):
        if p <= rank / n * alpha:
            largest_k = rank
    if largest_k > 0:
        for rank, (orig_idx, _) in enumerate(indexed, 1):
            if rank <= largest_k:
                reject[orig_idx] = True
    return reject


# ---------------------------------------------------------------------------
# main test routine
# ---------------------------------------------------------------------------

def run_finding(label: str, cohort: list[Trade], parent: list[Trade],
                n_perm: int, block_size: int, rng: random.Random,
                n_candidates_total: int, out: list[str]) -> dict:
    cohort_pnls = [t.pnl for t in cohort]
    parent_pnls = [t.pnl for t in parent]
    obs_sum = sum(cohort_pnls)
    obs_pf = pf(cohort_pnls)
    obs_wr = winrate(cohort_pnls)
    n_cohort = len(cohort_pnls)
    n_parent = len(parent_pnls)

    out.append("")
    out.append("=" * 72)
    out.append(f"FINDING: {label}")
    out.append("=" * 72)
    out.append(f"  cohort n        : {n_cohort}")
    out.append(f"  parent n        : {n_parent}")
    out.append(f"  observed pnl    : {obs_sum:+.2f}")
    out.append(f"  observed pf     : {obs_pf:.3f}")
    out.append(f"  observed winrate: {obs_wr:.3f}")

    if n_cohort < 20 or n_cohort >= n_parent:
        out.append("  *** SKIP: cohort underpowered or equals parent ***")
        return {"label": label, "p_iid_pnl": 1.0, "p_iid_pf": 1.0,
                "p_block_pnl": 1.0, "obs_sum": obs_sum, "obs_pf": obs_pf}

    # i.i.d. permutation test
    iid_pnl, iid_pf = perm_iid(parent_pnls, n_cohort, n_perm, rng)
    p_iid_pnl = one_sided_p(obs_sum, iid_pnl)
    p_iid_pf = one_sided_p(obs_pf, iid_pf) if obs_pf != float("inf") else 0.0

    # block bootstrap
    blk_pnl, blk_pf = perm_block(parent_pnls, n_cohort, n_perm, block_size, rng)
    p_blk_pnl = one_sided_p(obs_sum, blk_pnl)
    p_blk_pf = one_sided_p(obs_pf, blk_pf) if obs_pf != float("inf") else 0.0

    # null-dist quantile context
    def pct(samples: list[float], q: float) -> float:
        s = sorted(samples)
        if not s:
            return float("nan")
        idx = max(0, min(len(s) - 1, int(q * len(s))))
        return s[idx]

    out.append("")
    out.append(f"  i.i.d. perm test (n_perm={n_perm}):")
    out.append(f"    null pnl  median={pct(iid_pnl, 0.50):+.1f}  "
               f"95th={pct(iid_pnl, 0.95):+.1f}  99th={pct(iid_pnl, 0.99):+.1f}")
    out.append(f"    null pf   median={pct(iid_pf, 0.50):.3f}    "
               f"95th={pct(iid_pf, 0.95):.3f}    99th={pct(iid_pf, 0.99):.3f}")
    out.append(f"    p (sum-pnl) = {p_iid_pnl:.4f}")
    out.append(f"    p (pf)      = {p_iid_pf:.4f}")
    out.append("")
    out.append(f"  block bootstrap (block={block_size}, n_perm={n_perm}):")
    out.append(f"    null pnl  median={pct(blk_pnl, 0.50):+.1f}  "
               f"95th={pct(blk_pnl, 0.95):+.1f}  99th={pct(blk_pnl, 0.99):+.1f}")
    out.append(f"    p (sum-pnl) = {p_blk_pnl:.4f}")
    out.append(f"    p (pf)      = {p_blk_pf:.4f}")

    # family-wise correction context
    bonf = 0.05 / n_candidates_total
    out.append("")
    out.append(f"  family-wise context (S35 tested {n_candidates_total} candidates total):")
    out.append(f"    Bonferroni alpha = 0.05 / {n_candidates_total} = {bonf:.5f}")
    out.append(f"    passes Bonferroni @ alpha=0.05 (i.i.d. sum-pnl)? "
               f"{'YES' if p_iid_pnl < bonf else 'NO'}")
    out.append(f"    passes Bonferroni @ alpha=0.05 (block sum-pnl) ? "
               f"{'YES' if p_blk_pnl < bonf else 'NO'}")

    return {
        "label": label,
        "n_cohort": n_cohort,
        "obs_sum": obs_sum,
        "obs_pf": obs_pf,
        "p_iid_pnl": p_iid_pnl,
        "p_iid_pf": p_iid_pf,
        "p_block_pnl": p_blk_pnl,
        "p_block_pf": p_blk_pf,
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trades-dir", default=str(Path.home() / "Omega" / "backtest"),
                    help="directory containing walkforward_b[_long]_<SYM>_trades.csv")
    ap.add_argument("--output-dir", default=".",
                    help="directory to write permutation_test_s37_results.txt")
    ap.add_argument("--n-perm", type=int, default=10000,
                    help="number of permutation samples per test (default 10000)")
    ap.add_argument("--block", type=int, default=10,
                    help="block size for block bootstrap (default 10 trades)")
    ap.add_argument("--seed", type=int, default=42,
                    help="rng seed (default 42, matches S35)")
    args = ap.parse_args()

    trades_dir = Path(args.trades_dir).expanduser()
    output_dir = Path(args.output_dir).expanduser()
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"loading trades from {trades_dir} ...")
    trades = load_trades(trades_dir)
    if not trades:
        sys.exit("no trades loaded — check --trades-dir")
    print(f"loaded {len(trades)} trades across "
          f"{len({(t.symbol, t.mode) for t in trades})} (symbol, mode) pairs")

    rng = random.Random(args.seed)

    # enumerate S35 candidates for family-wise correction denominator
    candidates = enumerate_s35_candidates(trades)
    n_candidates = len(candidates)

    out: list[str] = []
    out.append("S37 PERMUTATION TEST — re-test of S35 candidate findings")
    out.append("=" * 72)
    out.append(f"  trades dir         : {trades_dir}")
    out.append(f"  total trades       : {len(trades)}")
    out.append(f"  S35 candidates re-counted: {n_candidates}")
    out.append(f"  n_perm per test    : {args.n_perm}")
    out.append(f"  block size         : {args.block}")
    out.append(f"  rng seed           : {args.seed}")

    # the three findings
    findings_specs = [
        ("GER40 base ATR-LOW",  lambda: select_atr_low(trades, "GER40",  "base")),
        ("GER40 long ATR-LOW",  lambda: select_atr_low(trades, "GER40",  "long")),
        ("XAUUSD long Mon",     lambda: select_dow(trades, "XAUUSD", "long", 0)),
    ]

    results = []
    for label, sel in findings_specs:
        cohort, parent = sel()
        if not cohort:
            out.append("")
            out.append(f"FINDING: {label} — NO MATCHING TRADES "
                       "(check symbol/mode names in trades.csv filenames)")
            continue
        res = run_finding(label, cohort, parent,
                          n_perm=args.n_perm, block_size=args.block,
                          rng=rng, n_candidates_total=n_candidates, out=out)
        results.append(res)

    # multiple-comparisons summary across the three findings
    out.append("")
    out.append("=" * 72)
    out.append("MULTIPLE-COMPARISONS SUMMARY")
    out.append("=" * 72)
    out.append("")
    out.append(f"{'finding':<28} {'n':>4} {'pnl':>10} {'pf':>6} "
               f"{'p_iid':>7} {'p_blk':>7}")
    out.append("-" * 72)
    for r in results:
        out.append(f"{r['label']:<28} {r.get('n_cohort','?'):>4} "
                   f"{r['obs_sum']:>+10.1f} {r['obs_pf']:>6.2f} "
                   f"{r['p_iid_pnl']:>7.4f} {r['p_block_pnl']:>7.4f}")

    # BH-FDR across just the 3 findings (the user-facing decision set)
    p_iid_list = [r["p_iid_pnl"] for r in results]
    p_blk_list = [r["p_block_pnl"] for r in results]
    bh_iid = benjamini_hochberg(p_iid_list, alpha=0.05)
    bh_blk = benjamini_hochberg(p_blk_list, alpha=0.05)

    out.append("")
    out.append("Benjamini-Hochberg FDR control @ alpha=0.05 across the 3 findings:")
    for r, ok_iid, ok_blk in zip(results, bh_iid, bh_blk):
        out.append(f"  {r['label']:<28} BH(iid)={'PASS' if ok_iid else 'FAIL'}  "
                   f"BH(block)={'PASS' if ok_blk else 'FAIL'}")

    # Bonferroni against the FULL S35 candidate set (the actually-honest one)
    bonf = 0.05 / n_candidates if n_candidates else 1.0
    out.append("")
    out.append(f"Bonferroni @ alpha=0.05 across ALL {n_candidates} S35 candidates "
               f"(threshold {bonf:.5f}):")
    for r in results:
        out.append(f"  {r['label']:<28} "
                   f"Bonf(iid)={'PASS' if r['p_iid_pnl'] < bonf else 'FAIL'}  "
                   f"Bonf(block)={'PASS' if r['p_block_pnl'] < bonf else 'FAIL'}")

    out.append("")
    out.append("=" * 72)
    out.append("INTERPRETATION GUIDE")
    out.append("=" * 72)
    out.append("")
    out.append("- Bonferroni vs ALL S35 candidates is the strictest test. PASSING")
    out.append("  here means 'this finding survives the multiple-testing burden of")
    out.append("  the entire S35 search.' This is the bar for treating it as a")
    out.append("  prior to a real OOS-of-OOS validation.")
    out.append("- BH-FDR across the 3 findings is less strict but assumes you've")
    out.append("  already pre-selected to these three. Useful only if you trust the")
    out.append("  selection wasn't itself a multiple-comparisons step.")
    out.append("- Block bootstrap p-value being noticeably worse than i.i.d. is a")
    out.append("  sign the i.i.d. test was understating noise — autocorrelated")
    out.append("  trades cluster wins/losses, inflating apparent edge.")
    out.append("- Block bootstrap being roughly equal to i.i.d. means the trades")
    out.append("  behave i.i.d.-ish at the block scale tested; either p stands.")

    text = "\n".join(out)
    output_path = output_dir / "permutation_test_s37_results.txt"
    output_path.write_text(text)
    print(text)
    print(f"\n=> wrote {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
