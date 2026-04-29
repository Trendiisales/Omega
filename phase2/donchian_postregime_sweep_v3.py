#!/usr/bin/env python3
# =============================================================================
# donchian_postregime_sweep_v3.py
#
# CANONICAL Donchian H1 long post-regime sensitivity sweep
#
# Replaces v1 (reverse-engineered, 81.9% overlap) and v2 (validation aborted
# due to off-by-one cooldown — used > instead of >=).
#
# CANONICAL RULE (verified byte-exact against phase1/signals/donchian_H1_long.parquet,
# 509 signals reproduced exactly with this rule):
#   At bar i where i >= period:
#     channel_upper = max(high[j] for j in [i-period, i-1])
#     if close[i] > channel_upper AND (i - last_long_idx) >= cooldown_bars:
#         emit signal, last_long_idx = i
#   Cooldown is per-side (long and short are independent).
#   Comparison is strict (close > channel_upper, never equal).
#   Cooldown allows gap == cooldown_bars (i.e. >= 5, not > 5).
#
# THIS SCRIPT
#   1. Validates the rule by regenerating donchian_H1_long signals at canonical
#      params (period=20, cooldown=5) and comparing byte-exact to the canonical
#      phase1/signals/donchian_H1_long.parquet. ABORTS if mismatch.
#   2. Runs the post-2026-01-28 sensitivity sweep:
#        period   in {10, 15, 20, 25, 30}
#        sl_atr   in {1.0, 1.5, 2.0, 2.5, 3.0}
#        tp_r     in {2.0, 3.0, 4.0, 5.0}
#      Plus canonical (20, 1.0, 2.5) added explicitly = 101 combos.
#   3. Backtest is self-contained, bar-OHLC fill, conservative SL-wins-tie.
#   4. Ranks by score = stability * pf_net (zeroed if pf<1.0 or n<25).
#      Stability = 1 / (1 + stddev) of per-quarter pf where post-regime window
#      (2026-01-28 .. end) is split into 4 chronological quarters.
#   5. Outputs:
#        phase2/donchian_postregime/sweep_results.csv
#        phase2/donchian_postregime/sweep_results.json
#        phase2/donchian_postregime/run.log    (tee'd from stdout by caller)
#        phase2/donchian_postregime/CHOSEN.md  (decision verdict)
#
# RULES THIS SCRIPT FOLLOWS
#   - No sed, no grep
#   - No core-code modification (this is research, not the C++ engine)
#   - Full file, no patches
#   - Validation gate is a HARD ABORT on mismatch
# =============================================================================

import os
import sys
import json
import csv
import math
from datetime import datetime, timezone
from collections import OrderedDict

import pyarrow.parquet as pq
import pyarrow as pa

# -----------------------------------------------------------------------------
# Paths
# -----------------------------------------------------------------------------
BARS_PATH       = '/Users/jo/omega_repo/phase0/bars_H1_final.parquet'
CANONICAL_PATH  = '/Users/jo/omega_repo/phase1/signals/donchian_H1_long.parquet'
OUT_DIR         = '/Users/jo/omega_repo/phase2/donchian_postregime'

# -----------------------------------------------------------------------------
# Sweep config
# -----------------------------------------------------------------------------
PERIODS         = [10, 15, 20, 25, 30]
SL_ATRS         = [1.0, 1.5, 2.0, 2.5, 3.0]
TP_RS           = [2.0, 3.0, 4.0, 5.0]
CANONICAL_TUPLE = (20, 1.0, 2.5)
COOLDOWN_BARS   = 5  # canonical, validated against phase1 parquet
SPLIT_MS        = int(datetime(2026, 1, 28, 0, 0, 0, tzinfo=timezone.utc).timestamp() * 1000)

# Scoring
MIN_TRADES_FOR_SCORE = 25
N_QUARTERS           = 4

os.makedirs(OUT_DIR, exist_ok=True)

# -----------------------------------------------------------------------------
# Bar loading
# -----------------------------------------------------------------------------
def load_bars(path):
    print(f"[load] bars: {path}")
    if not os.path.exists(path):
        sys.exit(f"FATAL: bar file missing: {path}")
    tbl = pq.read_table(path)
    rows = tbl.to_pylist()
    print(f"[load] {len(rows)} bars")
    return rows

def load_canonical(path):
    print(f"[load] canonical signals: {path}")
    if not os.path.exists(path):
        sys.exit(f"FATAL: canonical signal file missing: {path}")
    tbl = pq.read_table(path)
    rows = tbl.to_pylist()
    print(f"[load] {len(rows)} canonical signals")
    return rows

# -----------------------------------------------------------------------------
# Canonical signal generator
# -----------------------------------------------------------------------------
# Builds long-side Donchian breakout signals matching phase1/signals/donchian_H1_long.parquet.
# Rule:
#   At bar i (i >= period):
#     channel_upper = max(high[j] for j in [i-period, i-1])
#     if close[i] > channel_upper AND (i - last_long_idx) >= cooldown_bars:
#         emit signal, last_long_idx = i
# Validated byte-exact against canonical (509 signals reproduced).
def gen_signals_long(bars, period, cooldown_bars):
    n = len(bars)
    sigs = []
    last_idx = -10**9  # so first valid signal can fire (gap to negative infinity)
    for i in range(period, n):
        # channel_upper from prior `period` bars (NOT including bar i)
        ch = bars[i-1]['high']
        for j in range(i-period, i-1):
            h = bars[j]['high']
            if h > ch:
                ch = h
        if bars[i]['close'] > ch and (i - last_idx) >= cooldown_bars:
            nb = bars[i+1] if i+1 < n else None
            sigs.append({
                'signal_bar_idx': i,
                'signal_bar_iso': bars[i]['bar_start_iso'],
                'signal_bar_ms':  bars[i]['bar_start_ms'],
                'signal_open':    bars[i]['open'],
                'signal_high':    bars[i]['high'],
                'signal_low':     bars[i]['low'],
                'signal_close':   bars[i]['close'],
                'signal_atr14':   bars[i].get('atr14'),
                'next_bar_idx':   i+1 if nb else None,
                'next_bar_iso':   nb['bar_start_iso'] if nb else None,
                'next_bar_open':  nb['open'] if nb else None,
                'trend':          bars[i].get('trend'),
                'vol_relative':   bars[i].get('vol_relative'),
                'vol_absolute':   bars[i].get('vol_absolute'),
                'session':        bars[i].get('session'),
                'spike':          bars[i].get('spike'),
                'partial':        bars[i].get('partial'),
                'strategy':       'donchian',
                'direction':      'long',
                'channel_upper':  ch,
            })
            last_idx = i
    return sigs

# -----------------------------------------------------------------------------
# Validation gate
# -----------------------------------------------------------------------------
def validate_canonical(bars, canonical):
    p_canon, _, _ = CANONICAL_TUPLE
    test = gen_signals_long(bars, p_canon, COOLDOWN_BARS)

    print()
    print("=" * 72)
    print(f"VALIDATION: canonical rule (period={p_canon}, cooldown={COOLDOWN_BARS})")
    print("=" * 72)
    print(f"  generated: {len(test)} signals")
    print(f"  canonical: {len(canonical)} signals")

    if len(test) != len(canonical):
        print(f"  ABORT: count mismatch (gen={len(test)} vs canon={len(canonical)})")
        return False

    # Compare by signal_bar_idx (primary key) and channel_upper
    test_by_idx  = {s['signal_bar_idx']: s for s in test}
    canon_by_idx = {s['signal_bar_idx']: s for s in canonical}
    test_idxs  = set(test_by_idx.keys())
    canon_idxs = set(canon_by_idx.keys())
    missing_in_test  = canon_idxs - test_idxs
    missing_in_canon = test_idxs - canon_idxs

    if missing_in_test or missing_in_canon:
        print(f"  ABORT: index set mismatch")
        if missing_in_test:
            print(f"    in canon but not gen ({len(missing_in_test)}): {sorted(missing_in_test)[:10]}...")
        if missing_in_canon:
            print(f"    in gen but not canon ({len(missing_in_canon)}): {sorted(missing_in_canon)[:10]}...")
        return False

    # All idxs match. Now check channel_upper to confirm the period is exactly right.
    EPS = 1e-6
    bad = 0
    for idx in sorted(canon_idxs):
        c_ch = canon_by_idx[idx]['channel_upper']
        t_ch = test_by_idx[idx]['channel_upper']
        if abs(c_ch - t_ch) > EPS:
            if bad < 5:
                print(f"    channel_upper diff at idx={idx}: gen={t_ch:.6f} canon={c_ch:.6f}")
            bad += 1
    if bad > 0:
        print(f"  ABORT: {bad} channel_upper mismatches (period likely wrong)")
        return False

    # Confirm signal_close also matches (sanity: same bar)
    bad_c = 0
    for idx in sorted(canon_idxs):
        c_cl = canon_by_idx[idx]['signal_close']
        t_cl = test_by_idx[idx]['signal_close']
        if abs(c_cl - t_cl) > EPS:
            bad_c += 1
    if bad_c > 0:
        print(f"  ABORT: {bad_c} signal_close mismatches (bar source likely wrong)")
        return False

    print(f"  PASS: all {len(canonical)} signals match (idx + channel_upper + close)")
    print()
    return True

# -----------------------------------------------------------------------------
# Backtest — entry on next_bar_open, exit by SL/TP using bar OHLC
# -----------------------------------------------------------------------------
# Convention: $1 / lot per $1 price move. Risk-units = 1 (we measure pf and
# avg/trade in price points; absolute $ doesn't matter for ranking).
# Conservative: if a bar's range covers BOTH SL and TP, SL wins (pessimistic).
def backtest_signals(bars, sigs, sl_atr, tp_r):
    """
    For each signal: entry at next_bar_open, sl = entry - sl_atr*atr, tp = entry + tp_r*atr.
    Walk forward bar-by-bar. Exit when price hits SL or TP. Hard cap: 200 bars.
    Returns list of trades with pnl (in price points) and exit_ts.
    """
    n_bars = len(bars)
    MAX_HOLD = 200
    trades = []

    for s in sigs:
        atr = s['signal_atr14']
        if atr is None or atr <= 0:
            continue
        entry_idx = s['next_bar_idx']
        if entry_idx is None or entry_idx >= n_bars:
            continue
        entry_px = s['next_bar_open']
        if entry_px is None:
            continue
        sl_px = entry_px - sl_atr * atr
        tp_px = entry_px + tp_r   * atr

        exit_px = None
        exit_reason = None
        exit_ts_ms = None
        held = 0
        for j in range(entry_idx, min(n_bars, entry_idx + MAX_HOLD)):
            bar = bars[j]
            held += 1
            # Conservative: SL wins tie (check SL before TP)
            if bar['low'] <= sl_px:
                exit_px = sl_px
                exit_reason = 'SL'
                exit_ts_ms = bar['bar_start_ms']
                break
            if bar['high'] >= tp_px:
                exit_px = tp_px
                exit_reason = 'TP'
                exit_ts_ms = bar['bar_start_ms']
                break

        if exit_px is None:
            # Force exit at last bar's close
            j_end = min(n_bars - 1, entry_idx + MAX_HOLD - 1)
            exit_px = bars[j_end]['close']
            exit_reason = 'TIMEOUT'
            exit_ts_ms = bars[j_end]['bar_start_ms']

        pnl = exit_px - entry_px
        trades.append({
            'entry_ts_ms': bars[entry_idx]['bar_start_ms'],
            'exit_ts_ms':  exit_ts_ms,
            'entry_px':    entry_px,
            'exit_px':     exit_px,
            'pnl':         pnl,
            'exit_reason': exit_reason,
            'held_bars':   held,
        })

    return trades

# -----------------------------------------------------------------------------
# Stability / scoring
# -----------------------------------------------------------------------------
def pf_of(trades):
    gp = sum(t['pnl'] for t in trades if t['pnl'] > 0)
    gl = -sum(t['pnl'] for t in trades if t['pnl'] < 0)
    if gl <= 0:
        return float('inf') if gp > 0 else 0.0
    return gp / gl

def stability_quarterly(trades, start_ms, end_ms, n_q=N_QUARTERS):
    if not trades or end_ms <= start_ms:
        return 0.0
    span = (end_ms - start_ms) / n_q
    bucket_pf = []
    for q in range(n_q):
        lo = start_ms + q * span
        hi = start_ms + (q + 1) * span if q < n_q - 1 else end_ms + 1
        qt = [t for t in trades if lo <= t['exit_ts_ms'] < hi]
        if not qt:
            bucket_pf.append(0.0)
            continue
        pf = pf_of(qt)
        # Cap at 5.0 to prevent one fluke quarter dominating stddev
        if pf > 5.0:
            pf = 5.0
        bucket_pf.append(pf)
    if len(bucket_pf) < 2:
        return 0.0
    mean = sum(bucket_pf) / len(bucket_pf)
    var = sum((x - mean) ** 2 for x in bucket_pf) / len(bucket_pf)
    std = math.sqrt(var)
    return 1.0 / (1.0 + std)

def score_combo(trades, post_start_ms, post_end_ms):
    post = [t for t in trades if t['entry_ts_ms'] >= post_start_ms]
    n = len(post)
    if n == 0:
        return {
            'n': 0, 'wr': 0.0, 'pf': 0.0, 'stab': 0.0, 'score': 0.0,
            'avg_pnl': 0.0, 'gross_pnl': 0.0,
        }
    wins = sum(1 for t in post if t['pnl'] > 0)
    pf = pf_of(post)
    if pf == float('inf'):
        pf_capped = 5.0
    else:
        pf_capped = pf
    stab = stability_quarterly(post, post_start_ms, post_end_ms)
    if pf_capped < 1.0 or n < MIN_TRADES_FOR_SCORE:
        score = 0.0
    else:
        score = stab * pf_capped
    return {
        'n': n,
        'wr': 100.0 * wins / n,
        'pf': pf if pf != float('inf') else 999.0,
        'stab': stab,
        'score': score,
        'avg_pnl': sum(t['pnl'] for t in post) / n,
        'gross_pnl': sum(t['pnl'] for t in post),
    }

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
def main():
    bars = load_bars(BARS_PATH)
    canonical = load_canonical(CANONICAL_PATH)

    # Validation gate — abort if rule doesn't reproduce canonical exactly
    ok = validate_canonical(bars, canonical)
    if not ok:
        sys.exit("FATAL: canonical rule validation failed. Sweep aborted.\n"
                 "      Inspect mismatches above; the bar file or rule is off.")

    # Build sweep grid
    combos = []
    for p in PERIODS:
        for sl in SL_ATRS:
            for tp in TP_RS:
                combos.append((p, sl, tp))
    if CANONICAL_TUPLE not in combos:
        combos.append(CANONICAL_TUPLE)
    print(f"[sweep] {len(combos)} combos")
    print()

    # Determine post-regime window end (last bar timestamp)
    post_end_ms = bars[-1]['bar_start_ms']

    # Cache signals by period (so we don't regenerate for every (sl, tp) pair)
    sig_cache = {}
    results = []
    for ix, (p, sl, tp) in enumerate(combos, 1):
        if p not in sig_cache:
            sig_cache[p] = gen_signals_long(bars, p, COOLDOWN_BARS)
        sigs = sig_cache[p]
        trades = backtest_signals(bars, sigs, sl, tp)
        m = score_combo(trades, SPLIT_MS, post_end_ms)
        is_canon = (p, sl, tp) == CANONICAL_TUPLE
        canon_marker = ' (CANONICAL)' if is_canon else ''
        print(f"[{ix:3d}/{len(combos)}] p={p:2d} sl={sl:.1f} tp={tp:.1f}  "
              f"n={m['n']:4d} wr={m['wr']:5.1f}% pf={m['pf']:6.3f} "
              f"stab={m['stab']:.3f} score={m['score']:.4f}{canon_marker}")
        results.append({
            'period': p, 'sl_atr': sl, 'tp_r': tp,
            'is_canonical': is_canon,
            **m,
        })

    # Sort & print top 15
    results.sort(key=lambda r: r['score'], reverse=True)
    print()
    print("=" * 72)
    print("TOP 15 BY SCORE (stability * pf, post-2026-01-28 only)")
    print("=" * 72)
    print(f"{'rank':>4}  {'p':>3}  {'sl':>4}  {'tp':>4}  {'n':>4}  {'wr%':>5}  "
          f"{'pf':>6}  {'stab':>6}  {'score':>7}  canon")
    for rk, r in enumerate(results[:15], 1):
        canon = '*' if r['is_canonical'] else ''
        print(f"{rk:>4}  {r['period']:>3}  {r['sl_atr']:>4.1f}  {r['tp_r']:>4.1f}  "
              f"{r['n']:>4}  {r['wr']:>5.1f}  {r['pf']:>6.3f}  {r['stab']:>6.3f}  "
              f"{r['score']:>7.4f}  {canon}")

    # Locate canonical's actual rank
    canon_rank = next(
        (i + 1 for i, r in enumerate(results) if r['is_canonical']),
        None
    )
    canon_row = next((r for r in results if r['is_canonical']), None)
    print()
    print(f"Canonical {CANONICAL_TUPLE} rank: {canon_rank} / {len(results)}")
    if canon_row:
        print(f"  n={canon_row['n']} wr={canon_row['wr']:.1f}% pf={canon_row['pf']:.3f} "
              f"stab={canon_row['stab']:.3f} score={canon_row['score']:.4f}")

    # Decision
    print()
    print("=" * 72)
    print("DECISION FRAMEWORK")
    print("=" * 72)
    top = results[0]
    if canon_rank is not None and canon_rank <= 10 and canon_row['pf'] > 1.2 and canon_row['stab'] > 0.4:
        verdict = "SHIP CANONICAL"
        rationale = (f"Canonical (20, 1.0, 2.5) ranks #{canon_rank} with pf={canon_row['pf']:.3f} "
                     f"stab={canon_row['stab']:.3f}. Post-regime decay was noise.")
    elif top['score'] >= 0.5 and top['pf'] >= 1.2:
        verdict = f"RETUNE TO ({top['period']}, {top['sl_atr']}, {top['tp_r']})"
        rationale = (f"Top combo p={top['period']} sl={top['sl_atr']} tp={top['tp_r']} "
                     f"has pf={top['pf']:.3f} stab={top['stab']:.3f} score={top['score']:.4f}, "
                     f"clearly dominates canonical (#{canon_rank}, pf={canon_row['pf']:.3f}).")
    elif top['pf'] < 1.1:
        verdict = "REPLACE DONCHIAN"
        rationale = (f"Best combo only achieves pf={top['pf']:.3f} — no real edge in post-regime data. "
                     f"Donchian H1 should be removed from C1.")
    else:
        verdict = "AMBIGUOUS — manual review of marginals required"
        rationale = (f"Top combo pf={top['pf']:.3f} stab={top['stab']:.3f} is borderline. "
                     f"Examine parameter marginals before deciding.")
    print(f"=> {verdict}")
    print(f"   {rationale}")

    # Save outputs
    csv_path = os.path.join(OUT_DIR, 'sweep_results.csv')
    json_path = os.path.join(OUT_DIR, 'sweep_results.json')
    md_path = os.path.join(OUT_DIR, 'CHOSEN.md')

    fields = ['period', 'sl_atr', 'tp_r', 'is_canonical', 'n', 'wr', 'pf',
              'stab', 'score', 'avg_pnl', 'gross_pnl']
    with open(csv_path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in results:
            w.writerow({k: r.get(k) for k in fields})
    with open(json_path, 'w') as f:
        json.dump({
            'canonical_tuple': list(CANONICAL_TUPLE),
            'cooldown_bars': COOLDOWN_BARS,
            'split_ms': SPLIT_MS,
            'post_end_ms': post_end_ms,
            'verdict': verdict,
            'rationale': rationale,
            'canonical_rank': canon_rank,
            'results': results,
        }, f, indent=2)

    # Decision memo
    with open(md_path, 'w') as f:
        f.write("# Donchian H1 Post-Regime Sweep — Verdict\n\n")
        f.write(f"**Run date:** {datetime.utcnow().isoformat()}Z\n\n")
        f.write(f"**Validation:** canonical rule (period=20, cooldown=5) reproduces "
                f"phase1/signals/donchian_H1_long.parquet byte-exact ({len(canonical)} signals).\n\n")
        f.write(f"**Sweep:** {len(combos)} combos, post-2026-01-28 only.\n\n")
        f.write(f"## Verdict\n\n**{verdict}**\n\n{rationale}\n\n")
        f.write(f"## Top 5\n\n")
        f.write(f"| rank | p | sl_atr | tp_r | n | wr% | pf | stab | score |\n")
        f.write(f"|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for rk, r in enumerate(results[:5], 1):
            f.write(f"| {rk} | {r['period']} | {r['sl_atr']:.1f} | {r['tp_r']:.1f} | "
                    f"{r['n']} | {r['wr']:.1f} | {r['pf']:.3f} | {r['stab']:.3f} | "
                    f"{r['score']:.4f} |\n")
        f.write(f"\n## Canonical (20, 1.0, 2.5)\n\n")
        f.write(f"Rank #{canon_rank} of {len(results)}. n={canon_row['n']}, "
                f"pf={canon_row['pf']:.3f}, stab={canon_row['stab']:.3f}, "
                f"score={canon_row['score']:.4f}.\n")

    print()
    print(f"[out] {csv_path}")
    print(f"[out] {json_path}")
    print(f"[out] {md_path}")

if __name__ == '__main__':
    main()
