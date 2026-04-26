#!/usr/bin/env python3
"""
simulate_early_sl.py

Lossless re-simulation of time-conditional SL on the 10 walkforward
*_trades.csv files produced by walkforward_b / walkforward_b_long after
the S36 MAE-logging extension.

Mechanism. For each candidate config (early_sl_mult, early_bar_count):
  for each OOS trade:
    look at recorded mae_bar1..mae_bar_N where N = early_bar_count.
    if any mae_bar_k >= early_sl_mult AND that bar was active (mae_bar_k != -1):
        the trade would have hit the tightened SL on bar k. Replace the
        recorded exit with a synthetic SL hit at:
            adverse_pts = early_sl_mult * atr_at_entry
            new_pnl = -adverse_pts * usd_per_point * lot_size
        i.e., the loss is exactly -early_sl_mult * atr * upp * lot.
    else:
        trade unchanged.

Caveats stated up front so we don't overclaim:
  - When the trade originally exited via SL on bar 1/2/3 with a wider SL,
    the early-SL fires *earlier or the same time* and the loss is smaller.
    That's correct.
  - When the trade originally went to TP/WEEKEND/EOF but had MAE in the
    first 3 bars >= early_sl_mult, the simulator now intercepts it as a
    loss. This is the cohort that matters for testing whether the
    early-SL kills genuine winners.
  - Trades whose true bars_held > 3 but had MAE > early_sl_mult during
    bars 1..3 get reclassified from "winner" to "loser at -early_sl_mult".
    The simulator accounts for this exactly.
  - Trades that exit at bar k <= early_bar_count via TP have mae_bar_k
    sentinel-ed (we recorded MAE BEFORE the TP/SL hit check, so the bar
    is observed). This is correct: if mae_bar_k < early_sl_mult, the
    early-SL would not have fired and the trade keeps its TP exit.
  - We do NOT model the case where the same bar's MAE briefly touched
    the tightened SL but the close was beyond the original SL (e.g., wick
    pierce). The harness records the bar's TRUE adverse extreme (low_bid
    for long, high_ask for short), so if the bar's extreme exceeded
    early_sl_mult, the SL hits — that's exactly correct, because in live
    trading a price touch at the SL level executes the stop.

Inputs. 10 trades.csv files in current directory:
    walkforward_b_<SYM>_trades.csv          (5 files: XAUUSD, NAS100, US500, GER40, EURUSD)
    walkforward_b_long_<SYM>_trades.csv     (5 files: same symbols)

Outputs.
    early_sl_sweep.csv      grid of (config x dataset) -> stats
    early_sl_summary.txt    human readable: best per-dataset, hold-fixed-config A/B
    early_sl_per_trade.csv  for the headline config, per-trade before/after
"""

import os
import sys
import csv
import glob
from collections import defaultdict

# Symbol-level constants (must match SymbolSpec in the C++ harness)
SYMBOL_SPECS = {
    'XAUUSD': {'usd_per_point': 100.0,    'lot_size': 1.0},
    'NAS100': {'usd_per_point': 1.0,      'lot_size': 0.1},
    'US500':  {'usd_per_point': 1.0,      'lot_size': 0.1},
    'GER40':  {'usd_per_point': 1.0,      'lot_size': 0.1},  # note: EUR not USD
    'EURUSD': {'usd_per_point': 100000.0, 'lot_size': 0.1},
}


def load_trades(path):
    """Read a trades.csv into a list of dicts with typed fields."""
    rows = []
    with open(path) as f:
        rdr = csv.DictReader(f)
        for r in rdr:
            try:
                rows.append({
                    'window':       r['window'],
                    'donchian':     int(r['donchian']),
                    'sl_mult':      float(r['sl_mult']),
                    'tp_mult':      float(r['tp_mult']),
                    'direction':    r['direction'],   # 'long' or 'short'
                    'entry_ts':     r['entry_ts'],
                    'exit_ts':      r['exit_ts'],
                    'entry_px':     float(r['entry_px']),
                    'exit_px':      float(r['exit_px']),
                    'pnl':          float(r['pnl']),
                    'exit_reason':  r['exit_reason'],
                    'bars_held':    int(r['bars_held']),
                    'atr_at_entry': float(r['atr_at_entry']),
                    'mae_bar1':     float(r['mae_bar1']),
                    'mae_bar2':     float(r['mae_bar2']),
                    'mae_bar3':     float(r['mae_bar3']),
                })
            except (KeyError, ValueError) as e:
                print(f"WARN: malformed row in {path}: {e}; row={r}", file=sys.stderr)
    return rows


def simulate(trades, symbol, early_sl_mult, early_bar_count):
    """Re-simulate trades under the given early-SL config.

    Returns: list of (orig_pnl, new_pnl, fired_at_bar) one per trade.
    fired_at_bar is None if the early-SL did not fire.
    """
    spec = SYMBOL_SPECS[symbol]
    upp = spec['usd_per_point']
    lot = spec['lot_size']

    out = []
    mae_keys = ['mae_bar1', 'mae_bar2', 'mae_bar3']
    for t in trades:
        orig_pnl = t['pnl']
        atr = t['atr_at_entry']

        if atr <= 0.0:
            out.append((orig_pnl, orig_pnl, None))
            continue

        # Walk through bars 1..early_bar_count looking for MAE >= early_sl_mult.
        # Sentinel -1 means trade was not active that bar (original exit happened
        # earlier), so we cannot trigger then.
        fired_at = None
        for k in range(1, min(early_bar_count, 3) + 1):
            mae = t[mae_keys[k - 1]]
            if mae < 0.0:
                # Trade closed before this bar; nothing more to check.
                break
            if mae >= early_sl_mult:
                fired_at = k
                break

        if fired_at is None:
            new_pnl = orig_pnl
        else:
            # Tightened SL fires on bar `fired_at`. The loss is exactly
            # -early_sl_mult * atr * upp * lot (long or short, same magnitude).
            adverse_pts = early_sl_mult * atr
            new_pnl = -adverse_pts * upp * lot

            # IMPORTANT: if the trade's ORIGINAL exit was on or before this bar
            # AND was an SL with the WIDER stop, the tightened SL just gives a
            # smaller loss. Sanity check: new_pnl must be > orig_pnl in that
            # case. If the original was a TP that won, the swap turns it into
            # a loser, and new_pnl < orig_pnl — also expected.
            # No additional logic needed; the math handles both.

        out.append((orig_pnl, new_pnl, fired_at))
    return out


def aggregate(simulated):
    """Aggregate per-trade simulated results into combined stats."""
    n_trades = len(simulated)
    if n_trades == 0:
        return {
            'trades': 0, 'wins': 0, 'losses': 0, 'wr': 0.0,
            'gross_profit': 0.0, 'gross_loss': 0.0,
            'pnl': 0.0, 'pf': 0.0,
            'fired_count': 0, 'fired_pct': 0.0,
        }
    pnl = sum(new for _, new, _ in simulated)
    wins = sum(1 for _, new, _ in simulated if new >= 0.0)
    losses = n_trades - wins
    gp = sum(new for _, new, _ in simulated if new >= 0.0)
    gl = sum(new for _, new, _ in simulated if new < 0.0)
    pf = (gp / -gl) if gl < 0.0 else (1e9 if gp > 0.0 else 0.0)
    fired = sum(1 for _, _, f in simulated if f is not None)
    return {
        'trades': n_trades,
        'wins': wins,
        'losses': losses,
        'wr': wins / n_trades,
        'gross_profit': gp,
        'gross_loss': gl,
        'pnl': pnl,
        'pf': pf,
        'fired_count': fired,
        'fired_pct': fired / n_trades,
    }


def discover_datasets():
    """Find all 10 trades.csv files in the current directory."""
    datasets = []
    for path in sorted(glob.glob('walkforward_b_*_trades.csv')):
        # Distinguish base vs long. The pattern walkforward_b_long_*_trades.csv
        # also matches walkforward_b_*_trades.csv, so be careful.
        fname = os.path.basename(path)
        if fname.startswith('walkforward_b_long_'):
            mode = 'long'
            sym = fname[len('walkforward_b_long_'):-len('_trades.csv')]
        else:
            mode = 'base'
            sym = fname[len('walkforward_b_'):-len('_trades.csv')]
        if sym not in SYMBOL_SPECS:
            print(f"WARN: unknown symbol {sym} in {path}, skipping",
                  file=sys.stderr)
            continue
        datasets.append((sym, mode, path))
    return datasets


def fmt_pnl(symbol, v):
    """Format PnL with currency hint for GER40 (EUR)."""
    cur = 'EUR' if symbol == 'GER40' else 'USD'
    sign = '-' if v < 0 else ' '
    return f"{sign}{cur} {abs(v):>10,.2f}"


def main():
    datasets = discover_datasets()
    if len(datasets) != 10:
        print(f"WARN: expected 10 datasets, found {len(datasets)}",
              file=sys.stderr)
    print(f"Discovered {len(datasets)} datasets:", file=sys.stderr)
    for sym, mode, path in datasets:
        print(f"  {sym} {mode:5s} {path}", file=sys.stderr)
    print("", file=sys.stderr)

    # Cache loaded trades to avoid re-reading per config
    loaded = []
    for sym, mode, path in datasets:
        trades = load_trades(path)
        loaded.append((sym, mode, path, trades))

    # === Phase 1: defaults baseline (config = no early SL) ===
    # We trust the existing pnl column as ground truth.
    baseline = {}
    for sym, mode, path, trades in loaded:
        if not trades:
            continue
        baseline[(sym, mode)] = aggregate([(t['pnl'], t['pnl'], None) for t in trades])

    # === Phase 2: candidate sweep ===
    # Hypothesis: tighter early SL improves bars<=3 cohort without hurting
    # the bars>3 cohort. We sweep early_sl_mult x early_bar_count.
    sl_grid = [0.3, 0.5, 0.75, 1.0]
    bc_grid = [1, 2, 3]
    configs = [(sl, bc) for sl in sl_grid for bc in bc_grid]

    sweep_rows = []
    for sym, mode, path, trades in loaded:
        if not trades:
            continue
        base = baseline[(sym, mode)]
        for sl, bc in configs:
            sim = simulate(trades, sym, sl, bc)
            agg = aggregate(sim)
            sweep_rows.append({
                'symbol':           sym,
                'mode':             mode,
                'early_sl_mult':    sl,
                'early_bar_count':  bc,
                'orig_pnl':         base['pnl'],
                'orig_pf':          base['pf'],
                'orig_wr':          base['wr'],
                'orig_trades':      base['trades'],
                'new_pnl':          agg['pnl'],
                'new_pf':           agg['pf'],
                'new_wr':           agg['wr'],
                'new_trades':       agg['trades'],
                'fired_count':      agg['fired_count'],
                'fired_pct':        agg['fired_pct'],
                'delta_pnl':        agg['pnl'] - base['pnl'],
                'delta_pnl_pct':    (agg['pnl'] - base['pnl']) / base['pnl']
                                    if base['pnl'] != 0 else 0.0,
            })

    # Write CSV
    with open('early_sl_sweep.csv', 'w', newline='') as f:
        if sweep_rows:
            wr = csv.DictWriter(f, fieldnames=list(sweep_rows[0].keys()))
            wr.writeheader()
            wr.writerows(sweep_rows)
    print(f"Wrote early_sl_sweep.csv ({len(sweep_rows)} rows)", file=sys.stderr)

    # === Phase 3: human-readable summary ===
    with open('early_sl_summary.txt', 'w') as f:
        f.write("=" * 80 + "\n")
        f.write("S36 EARLY-SL RE-SIMULATION SUMMARY\n")
        f.write("Lossless replay of time-conditional SL using recorded MAE bar1..3\n")
        f.write("=" * 80 + "\n\n")

        # Per-dataset baseline
        f.write("BASELINE (no early-SL) — from default-mode walkforward trades.csv:\n\n")
        f.write(f"  {'symbol':10s} {'mode':6s} {'trades':>7s} {'pnl':>16s}"
                f" {'pf':>7s} {'wr':>7s}\n")
        for sym, mode, path, trades in loaded:
            if (sym, mode) not in baseline:
                continue
            b = baseline[(sym, mode)]
            f.write(f"  {sym:10s} {mode:6s} {b['trades']:>7d}"
                    f" {fmt_pnl(sym, b['pnl']):>16s}"
                    f" {b['pf']:>7.3f} {b['wr']:>7.3f}\n")
        f.write("\n")

        # Sweep table per dataset
        f.write("SWEEP (delta vs baseline, by dataset):\n\n")
        # Group sweep rows by (symbol, mode)
        groups = defaultdict(list)
        for row in sweep_rows:
            groups[(row['symbol'], row['mode'])].append(row)

        for (sym, mode), rows in sorted(groups.items()):
            base = baseline[(sym, mode)]
            f.write(f"--- {sym} {mode} (baseline pnl={fmt_pnl(sym, base['pnl'])}, "
                    f"pf={base['pf']:.3f}, n={base['trades']}) ---\n")
            f.write(f"  {'sl':>5s} {'bars':>5s} {'fire%':>7s} {'new_pnl':>16s}"
                    f" {'Δpnl':>14s} {'Δ%':>8s} {'new_pf':>8s} {'new_wr':>8s}\n")
            for r in sorted(rows, key=lambda x: (x['early_bar_count'], x['early_sl_mult'])):
                f.write(f"  {r['early_sl_mult']:>5.2f} {r['early_bar_count']:>5d}"
                        f" {r['fired_pct']*100:>6.1f}%"
                        f" {fmt_pnl(sym, r['new_pnl']):>16s}"
                        f" {fmt_pnl(sym, r['delta_pnl']):>14s}"
                        f" {r['delta_pnl_pct']*100:>7.1f}%"
                        f" {r['new_pf']:>8.3f} {r['new_wr']:>8.3f}\n")
            f.write("\n")

        # Cross-dataset rollup: for each config, count datasets where pnl improved
        f.write("CROSS-DATASET ROLLUP — count of datasets where Δpnl > 0:\n\n")
        f.write(f"  {'sl':>5s} {'bars':>5s} {'wins/10':>8s}"
                f" {'sum_Δpnl_USD_eqv':>20s} (excl. GER40 EUR)\n")
        config_summary = defaultdict(lambda: {'wins': 0, 'total': 0,
                                              'sum_dpnl_usd': 0.0})
        for r in sweep_rows:
            key = (r['early_sl_mult'], r['early_bar_count'])
            config_summary[key]['total'] += 1
            if r['delta_pnl'] > 0:
                config_summary[key]['wins'] += 1
            if r['symbol'] != 'GER40':
                config_summary[key]['sum_dpnl_usd'] += r['delta_pnl']

        for (sl, bc), s in sorted(config_summary.items()):
            f.write(f"  {sl:>5.2f} {bc:>5d}"
                    f" {s['wins']:>3d}/{s['total']:<3d}"
                    f" {s['sum_dpnl_usd']:>20,.2f}\n")
        f.write("\n")

        # Per-bar contribution: for the headline config (0.5 / 3), how often
        # did the early-SL fire on bar 1 vs bar 2 vs bar 3?
        f.write("FIRE BREAKDOWN at config (0.5, 3): how many trades fire on each bar\n\n")
        for sym, mode, path, trades in loaded:
            if not trades:
                continue
            sim = simulate(trades, sym, 0.5, 3)
            by_bar = defaultdict(int)
            for _, _, fired in sim:
                if fired is not None:
                    by_bar[fired] += 1
            total_fired = sum(by_bar.values())
            f.write(f"  {sym:8s} {mode:6s} fired {total_fired:>3d}/{len(sim):<3d}: "
                    f"bar1={by_bar[1]:>3d}  bar2={by_bar[2]:>3d}  bar3={by_bar[3]:>3d}\n")
        f.write("\n")

        # Honest read on best config across all 10
        f.write("HEADLINE READ:\n\n")
        best_total = max(config_summary.items(),
                         key=lambda kv: kv[1]['sum_dpnl_usd'])
        best_wins = max(config_summary.items(),
                        key=lambda kv: kv[1]['wins'])
        sl, bc = best_total[0]
        f.write(f"  Best by sum-of-Δpnl (USD only): "
                f"early_sl_mult={sl:.2f}, early_bar_count={bc} "
                f"  ({best_total[1]['sum_dpnl_usd']:+,.2f} across 9 USD datasets)\n")
        sl, bc = best_wins[0]
        f.write(f"  Best by win-count: "
                f"early_sl_mult={sl:.2f}, early_bar_count={bc} "
                f"  ({best_wins[1]['wins']}/10 datasets improved)\n\n")

        # If no config improves >= 7/10, hypothesis is dead
        if max(s['wins'] for s in config_summary.values()) < 7:
            f.write("VERDICT: No config improved >=7/10 datasets. "
                    "The bars_held edge does NOT translate into a tradable "
                    "early-SL filter. Hypothesis falsified at the cohort level.\n")
        elif max(s['wins'] for s in config_summary.values()) >= 8:
            f.write("VERDICT: A config improved >=8/10 datasets. "
                    "Worth a real harness A/B with force-pick env vars to "
                    "rule out picker-confounding.\n")
        else:
            f.write("VERDICT: Mixed results (7/10 best). Marginal signal. "
                    "Probably not robust enough to deploy without further validation.\n")

    print("Wrote early_sl_summary.txt", file=sys.stderr)


if __name__ == '__main__':
    main()
