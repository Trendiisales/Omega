#!/usr/bin/env python3
"""
MAE/MFE Analysis for Omega trade CSV
Usage: python scripts/mae_mfe_analysis.py [path/to/omega_trade_closes.csv]
"""
import sys, csv, os
from collections import defaultdict

DEFAULT_PATHS = [
    "logs/trades/omega_trade_closes.csv",
    "C:/Omega/logs/trades/omega_trade_closes.csv",
]

def load(path):
    with open(path, newline='', encoding='utf-8-sig') as f:
        return list(csv.DictReader(f))

def f(v, default=0.0):
    try: return float(v or default)
    except: return default

def analyse(rows):
    # MFE/MAE in CSV are raw price points * size (not yet * tick_value).
    # XAUUSD: tick_value = $100/pt/lot. So MFE_usd = mfe_raw * 100.
    # But the CSV already stores pnl in USD (handle_closed_trade multiplies by mult).
    # Check: if mfe looks like pts (e.g. 0.33), multiply by 100 * size.
    # If mfe looks like dollars already (e.g. 33.0), use directly.
    # Heuristic: if avg |net_pnl| >> avg mfe*100, mfe is already in USD.

    full = [r for r in rows
            if r.get('exit_reason','') not in ('PARTIAL_1R','PARTIAL_2R','')
            and r.get('exit_reason','') != '']

    if not full:
        print("No full-close trades found.")
        return

    # Detect MFE scaling: sample a few trades
    sample_mfe = [f(r.get('mfe','0')) for r in full[:20] if f(r.get('mfe')) > 0]
    sample_pnl = [abs(f(r.get('net_pnl','0'))) for r in full[:20] if abs(f(r.get('net_pnl','0'))) > 0]
    # If median MFE < 5.0 it's in price points (e.g. 0.33 pts), needs *100 for XAUUSD
    # If median MFE > 10.0 it's already in USD
    med_mfe = sorted(sample_mfe)[len(sample_mfe)//2] if sample_mfe else 1.0
    mfe_scale = 100.0 if med_mfe < 5.0 else 1.0

    print(f"\n{'='*62}")
    print(f"  OMEGA MAE/MFE ANALYSIS")
    print(f"  {len(full)} full-close trades  (MFE scale: x{mfe_scale:.0f})")
    print(f"{'='*62}")

    engines = defaultdict(list)
    for r in full:
        engines[r.get('engine','?')].append(r)

    for eng, trades in sorted(engines.items()):
        wins   = [t for t in trades if f(t.get('net_pnl')) > 0]
        losses = [t for t in trades if f(t.get('net_pnl')) <= 0]
        if len(trades) < 3:
            continue

        mfe_wins    = [f(t['mfe']) * mfe_scale for t in wins   if f(t.get('mfe')) > 0]
        mfe_losses  = [f(t['mfe']) * mfe_scale for t in losses if f(t.get('mfe')) > 0]
        net_wins    = [f(t['net_pnl']) for t in wins]
        net_losses  = [f(t['net_pnl']) for t in losses]
        sl_losses   = [t for t in losses if 'SL' in t.get('exit_reason','')]

        print(f"\n  [{eng}]  {len(trades)} trades  WR={100*len(wins)/len(trades):.0f}%")
        print(f"  {'─'*57}")

        if wins:
            avg_net_w = sum(net_wins)/len(net_wins)
            print(f"  WINNERS ({len(wins)}):")
            print(f"    Avg net P&L:     ${avg_net_w:+.2f}")
            if mfe_wins:
                avg_mfe = sum(mfe_wins)/len(mfe_wins)
                cap = sum(net_wins)/sum(mfe_wins) if sum(mfe_wins) > 0 else 0
                mfe_s = sorted(mfe_wins)
                print(f"    Avg MFE (peak):  ${avg_mfe:.2f}   capture={cap*100:.0f}%")
                print(f"    MFE p25/p50/p75: ${mfe_s[len(mfe_s)//4]:.2f} / "
                      f"${mfe_s[len(mfe_s)//2]:.2f} / ${mfe_s[3*len(mfe_s)//4]:.2f}")

        if losses:
            avg_net_l = sum(net_losses)/len(net_losses)
            print(f"  LOSERS ({len(losses)}):")
            print(f"    Avg net P&L:     ${avg_net_l:.2f}")
            if mfe_losses:
                avg_mfe_l = sum(mfe_losses)/len(mfe_losses)
                print(f"    Avg MFE before loss: ${avg_mfe_l:.2f}  (was in profit this much)")
            if sl_losses:
                risk_amt  = [abs(f(t.get('net_pnl'))) for t in sl_losses]
                mfe_sl    = [f(t.get('mfe','0'))*mfe_scale for t in sl_losses]
                # How many went >0.5R into profit before SL
                went_pos  = sum(1 for m, r in zip(mfe_sl, risk_amt) if r > 0 and m > r*0.5)
                print(f"    SL hits that reached >0.5R profit first: "
                      f"{went_pos}/{len(sl_losses)} ({100*went_pos/max(1,len(sl_losses)):.0f}%)")
                if went_pos > 0:
                    print(f"    --> Staircase at 0.5R instead of 1R would have banked")
                    print(f"        partial profit on {went_pos} of these before reversal")

        if wins and losses:
            avg_w = sum(net_wins)/len(net_wins)
            avg_l = abs(sum(net_losses)/len(net_losses))
            wr    = len(wins)/len(trades)
            exp   = wr*avg_w - (1-wr)*avg_l
            print(f"\n  PAYOFF:      {avg_w/avg_l:.2f}x  avg_win=${avg_w:.0f}  avg_loss=${avg_l:.0f}")
            print(f"  EXPECTANCY:  ${exp:+.2f}/trade")
            if avg_w/avg_l < 1.0:
                be_wr = avg_l/(avg_w+avg_l)
                print(f"  BREAKEVEN:   need {be_wr*100:.0f}% WR (have {wr*100:.0f}%)")
                print(f"               OR avg winner needs to be ${avg_l:.0f} (currently ${avg_w:.0f})")

    # GoldFlow SL size distribution
    gf = [r for r in full if 'Flow' in r.get('engine','') or 'GoldFlow' in r.get('engine','')]
    sl_only = [r for r in gf if 'SL' in r.get('exit_reason','')]
    if sl_only:
        sl_sizes = sorted([abs(f(r.get('net_pnl'))) for r in sl_only])
        print(f"\n  {'='*57}")
        print(f"  GOLDFLOW SL LOSS DISTRIBUTION ({len(sl_sizes)} SL hits)")
        print(f"    Min: ${min(sl_sizes):.0f}   "
              f"p25: ${sl_sizes[len(sl_sizes)//4]:.0f}   "
              f"Median: ${sl_sizes[len(sl_sizes)//2]:.0f}   "
              f"p75: ${sl_sizes[3*len(sl_sizes)//4]:.0f}   "
              f"Max: ${max(sl_sizes):.0f}")
        over_100 = sum(1 for s in sl_sizes if s > 100)
        avg_sl   = sum(sl_sizes)/len(sl_sizes)
        print(f"    Avg loss: ${avg_sl:.0f}   Losses >$100: {over_100}/{len(sl_sizes)} ({100*over_100/len(sl_sizes):.0f}%)")
        # Show impact of hard cap
        for cap in [60, 70, 80, 90]:
            capped_avg = sum(min(s, cap) for s in sl_sizes)/len(sl_sizes)
            saved = (avg_sl - capped_avg) * len(sl_sizes)
            print(f"    Cap at ${cap}: avg loss ${capped_avg:.0f}  "
                  f"(saves ${avg_sl-capped_avg:.0f}/trade, ${saved:.0f} total on this sample)")

    # Session summary
    all_net = sum(f(r.get('net_pnl')) for r in full)
    all_wins = sum(1 for r in full if f(r.get('net_pnl')) > 0)
    print(f"\n  {'='*57}")
    print(f"  SESSION TOTAL: {len(full)} trades  "
          f"WR={100*all_wins/len(full):.0f}%  "
          f"Net=${all_net:+.2f}")
    print(f"{'='*62}\n")

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if not path:
        for p in DEFAULT_PATHS:
            if os.path.exists(p):
                path = p
                break
    if not path or not os.path.exists(path):
        print(f"Usage: python scripts/mae_mfe_analysis.py <omega_trade_closes.csv>")
        sys.exit(1)
    print(f"Loading: {path}")
    analyse(load(path))
