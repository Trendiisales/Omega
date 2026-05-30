#!/usr/bin/env python3
"""
deflated_sharpe_gate.py  --  S39 2026-05-30
============================================
Statistically-honest engine-promotion gate for Omega.

Computes the Deflated Sharpe Ratio (DSR; Bailey & Lopez de Prado 2014) per
engine from the shadow ledger. The DSR corrects an observed Sharpe ratio for:
  (1) selection under MULTIPLE TESTING -- we ran N engines, so the best-looking
      Sharpe is inflated by the max-of-N effect;
  (2) non-normal returns (skew + excess kurtosis);
  (3) finite sample length.

Why this and not raw PF/WR: with ~38 engines on the ledger, picking the
top-Sharpe engine and promoting it is exactly the overfit trap the research
flags. The DSR asks: "is this engine's edge real once we account for having
fished across 38 of them?" Only DSR > 0.95 (95% confidence true SR > the
deflated benchmark) AND n >= MIN_N earns a PROMOTE.

Per-trade Sharpe is used as the return series. It is SCALE-INVARIANT
(mean/std cancels each engine's pnl_scale), so the per-engine pnl_scale
differences in the shadow ledger do not distort the comparison.

Usage:
  python3 tools/deflated_sharpe_gate.py <omega_trade_closes.csv> [--min-n 15]
"""
import csv, math, sys, argparse
from collections import defaultdict

EULER_MASCHERONI = 0.5772156649015329
E = math.e

# ---------------------------------------------------------------------------
# Standard normal CDF / inverse-CDF (no scipy dependency).
# ---------------------------------------------------------------------------
def norm_cdf(x):
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))

def norm_ppf(p):
    """Inverse standard-normal CDF -- Acklam's rational approximation."""
    if p <= 0.0: return -float('inf')
    if p >= 1.0: return float('inf')
    a = [-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
          1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00]
    b = [-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
          6.680131188771972e+01, -1.328068155288572e+01]
    c = [-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
         -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00]
    d = [7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
         3.754408661907416e+00]
    plow, phigh = 0.02425, 1 - 0.02425
    if p < plow:
        q = math.sqrt(-2 * math.log(p))
        return (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / \
               ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)
    if p > phigh:
        q = math.sqrt(-2 * math.log(1 - p))
        return -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / \
                ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)
    q = p - 0.5; r = q*q
    return (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q / \
           (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1)

# ---------------------------------------------------------------------------
def moments(xs):
    n = len(xs)
    m = sum(xs) / n
    var = sum((x-m)**2 for x in xs) / n
    sd = math.sqrt(var) if var > 0 else 0.0
    if sd == 0:
        return m, sd, 0.0, 3.0
    skew = (sum((x-m)**3 for x in xs) / n) / sd**3
    kurt = (sum((x-m)**4 for x in xs) / n) / sd**4   # raw (normal = 3)
    return m, sd, skew, kurt

def sharpe(xs):
    m, sd, _, _ = moments(xs)
    return (m / sd) if sd > 0 else 0.0

def expected_max_sharpe(var_sr, N):
    """SR0: expected max Sharpe across N independent trials under H0 (true SR=0)."""
    if N <= 1 or var_sr <= 0:
        return 0.0
    sd_sr = math.sqrt(var_sr)
    return sd_sr * ((1 - EULER_MASCHERONI) * norm_ppf(1 - 1.0/N)
                    + EULER_MASCHERONI * norm_ppf(1 - 1.0/(N*E)))

def deflated_sharpe(sr_hat, T, skew, kurt, sr0):
    """DSR = P(true SR > sr0) given observed sr_hat, sample T, skew, kurt."""
    if T < 2:
        return 0.0
    denom = 1 - skew*sr_hat + ((kurt - 1.0)/4.0)*sr_hat**2
    if denom <= 0:
        denom = 1e-9
    z = (sr_hat - sr0) * math.sqrt(T - 1) / math.sqrt(denom)
    return norm_cdf(z)

# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--min-n", type=int, default=15,
                    help="min trades for an engine to be eligible for promotion")
    ap.add_argument("--promote-dsr", type=float, default=0.95)
    ap.add_argument("--promote-n", type=int, default=30)
    args = ap.parse_args()

    rows = list(csv.DictReader(open(args.csv, encoding="utf-8-sig")))
    by = defaultdict(list)
    for r in rows:
        try:
            by[r["engine"]].append(float(r["net_pnl"]))
        except (ValueError, KeyError):
            pass

    # Trial set = engines with >= min_n trades. N trials => DSR deflation.
    eligible = {e: xs for e, xs in by.items() if len(xs) >= args.min_n}
    if not eligible:
        print(f"No engine has >= {args.min_n} trades. Cannot run DSR gate honestly.")
        # still show per-engine SR for context
        print("\nPer-engine Sharpe (sample too small for DSR):")
        for e, xs in sorted(by.items(), key=lambda kv: -sharpe(kv[1])):
            print(f"  {e:<40} n={len(xs):>3}  SR/trade={sharpe(xs):+.3f}")
        sys.exit(0)

    sr_trials = [sharpe(xs) for xs in eligible.values()]
    N = len(sr_trials)
    mean_sr = sum(sr_trials)/N
    var_sr = sum((s-mean_sr)**2 for s in sr_trials)/N if N > 1 else 0.0
    sr0 = expected_max_sharpe(var_sr, N)

    print("="*92)
    print(f"DEFLATED SHARPE PROMOTION GATE  (Bailey & Lopez de Prado)")
    print(f"  trial set N = {N} engines with n>=({args.min_n})   "
          f"Var(SR_trials)={var_sr:.4f}   deflated benchmark SR0={sr0:+.4f}/trade")
    print(f"  PROMOTE rule: DSR >= {args.promote_dsr} AND n >= {args.promote_n}")
    print("="*92)
    print(f"{'engine':<40}{'n':>4}{'SR/trd':>8}{'skew':>7}{'kurt':>7}{'DSR':>7}  verdict")
    print("-"*92)

    results = []
    for e, xs in eligible.items():
        m, sd, sk, ku = moments(xs)
        sr = (m/sd) if sd > 0 else 0.0
        dsr = deflated_sharpe(sr, len(xs), sk, ku, sr0)
        results.append((e, len(xs), sr, sk, ku, dsr))

    for e, n, sr, sk, ku, dsr in sorted(results, key=lambda r: -r[5]):
        if sr <= 0:
            v = "KILL (SR<=0)"
        elif dsr >= args.promote_dsr and n >= args.promote_n:
            v = ">>> PROMOTE"
        elif dsr >= args.promote_dsr:
            v = f"PASS but n<{args.promote_n} -- keep shadow"
        elif dsr >= 0.90:
            v = "MARGINAL -- more shadow"
        else:
            v = "HOLD shadow (not significant)"
        print(f"{e:<40}{n:>4}{sr:>+8.3f}{sk:>7.2f}{ku:>7.2f}{dsr:>7.3f}  {v}")

    print("-"*92)
    print("Note: engines below n>=%d are excluded from the trial set and shown"
          " by raw Sharpe only:" % args.min_n)
    for e, xs in sorted(((e,xs) for e,xs in by.items() if len(xs) < args.min_n),
                        key=lambda kv: -sharpe(kv[1])):
        print(f"  {e:<40} n={len(xs):>3}  SR/trade={sharpe(xs):+.3f}  (INSUFFICIENT)")

if __name__ == "__main__":
    main()
