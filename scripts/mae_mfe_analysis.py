#!/usr/bin/env python3
"""
MAE/MFE Analysis for Omega trade CSV
Answers: are exits optimally timed? Is the staircase trigger at the right R?

Usage: python scripts/mae_mfe_analysis.py [path/to/omega_trade_closes.csv]
       python scripts/mae_mfe_analysis.py C:\Omega\logs\trades\omega_trade_closes.csv
"""
import sys, csv, os
from collections import defaultdict

DEFAULT_PATHS = [
    "logs/trades/omega_trade_closes.csv",
    "C:/Omega/logs/trades/omega_trade_closes.csv",
    r"C:\Omega\logs\trades\omega_trade_closes.csv",
]

def load(path):
    rows = []
    with open(path, newline='', encoding='utf-8-sig') as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)
    return rows

def f(v, default=0.0):
    try: return float(v or default)
    except: return default

def analyse(rows):
    # Filter: full closes only (no partials), XAUUSD GoldFlow
    full = [r for r in rows
            if r.get('exit_reason','') not in ('PARTIAL_1R','PARTIAL_2R','')
            and f(r.get('mfe','0')) >= 0]

    print(f"\n{'='*60}")
    print(f"  OMEGA MAE/MFE ANALYSIS")
    print(f"  {len(full)} full-close trades loaded")
    print(f"{'='*60}")

    engines = defaultdict(list)
    for r in full:
        engines[r.get('engine','?')].append(r)

    for eng, trades in sorted(engines.items()):
        wins   = [t for t in trades if f(t.get('net_pnl')) > 0]
        losses = [t for t in trades if f(t.get('net_pnl')) <= 0]
        if len(trades) < 5:
            continue

        mfe_wins   = [f(t['mfe']) * 100 for t in wins   if f(t.get('mfe')) > 0]
        mfe_losses = [f(t['mfe']) * 100 for t in losses if f(t.get('mfe')) > 0]
        net_wins   = [f(t['net_pnl']) for t in wins]
        net_losses = [f(t['net_pnl']) for t in losses]

        print(f"\n  [{eng}]  {len(trades)} trades  WR={100*len(wins)/len(trades):.0f}%")
        print(f"  {'─'*55}")

        if wins:
            print(f"  WINNERS ({len(wins)}):")
            print(f"    Avg net:         ${sum(net_wins)/len(net_wins):+.2f}")
            if mfe_wins:
                print(f"    Avg MFE:         ${sum(mfe_wins)/len(mfe_wins):.2f}  (peak excursion)")
                print(f"    Capture ratio:   {sum(net_wins)/sum(mfe_wins)*100:.0f}%  (net/MFE)")
                print(f"    MFE distribution: min=${min(mfe_wins):.0f}  p25=${sorted(mfe_wins)[len(mfe_wins)//4]:.0f}  "
                      f"p50=${sorted(mfe_wins)[len(mfe_wins)//2]:.0f}  max=${max(mfe_wins):.0f}")

        if losses:
            print(f"  LOSERS ({len(losses)}):")
            print(f"    Avg net:         ${sum(net_losses)/len(net_losses):.2f}")
            if mfe_losses:
                print(f"    Avg MFE before loss: ${sum(mfe_losses)/len(mfe_losses):.2f}  (was profitable this much before reversing)")
                # Trades that went positive then came back
                gave_back = [t for t in losses if f(t.get('mfe')) * 100 > 5.0]
                print(f"    Gave back >$5:   {len(gave_back)} trades  "
                      f"(avg MFE ${sum(f(t['mfe'])*100 for t in gave_back)/max(1,len(gave_back)):.0f} -> "
                      f"net ${sum(f(t['net_pnl']) for t in gave_back)/max(1,len(gave_back)):.0f})")
                # Key question: what fraction of losses hit >1R before SL?
                sl_losses = [t for t in losses if t.get('exit_reason') == 'SL_HIT']
                if sl_losses:
                    risk_dollars = [abs(f(t.get('net_pnl'))) for t in sl_losses]
                    mfe_before_sl = [f(t.get('mfe','0'))*100 for t in sl_losses]
                    one_r_before_sl = sum(1 for m, r in zip(mfe_before_sl, risk_dollars)
                                         if r > 0 and m > r * 0.5)
                    print(f"    SL hits that reached >0.5R profit first: "
                          f"{one_r_before_sl}/{len(sl_losses)}  "
                          f"({100*one_r_before_sl/max(1,len(sl_losses)):.0f}%)")
                    print(f"    --> If staircase triggered at 0.5R instead of 1R,")
                    print(f"        {one_r_before_sl} losses could have banked partial profit")

        # Payoff analysis
        if wins and losses:
            avg_w = sum(net_wins) / len(net_wins)
            avg_l = abs(sum(net_losses) / len(net_losses))
            wr = len(wins) / len(trades)
            exp = wr * avg_w - (1 - wr) * avg_l
            print(f"\n  PAYOFF RATIO: {avg_w/avg_l:.2f}x  (need >1.0 for positive expectancy)")
            print(f"  EXPECTANCY:   ${exp:+.2f}/trade")
            if avg_w / avg_l < 1.0:
                needed_wr = avg_l / (avg_w + avg_l)
                print(f"  --> At current payoff, need {needed_wr*100:.0f}% WR to break even (have {wr*100:.0f}%)")
                print(f"  --> OR need avg winner ${avg_l:.0f} to match avg loser (currently ${avg_w:.0f})")

    # SL size analysis across all gold flow
    gf = [r for r in full if 'GoldFlow' in r.get('engine','') or 'Flow' in r.get('engine','')]
    if gf:
        print(f"\n  {'='*55}")
        print(f"  GOLDFLOW SL SIZE DISTRIBUTION ({len(gf)} trades)")
        sl_sizes = sorted([abs(f(r.get('net_pnl','0'))) for r in gf
                          if r.get('exit_reason','') == 'SL_HIT'])
        if sl_sizes:
            print(f"    Min SL loss:  ${min(sl_sizes):.0f}")
            print(f"    p25:          ${sl_sizes[len(sl_sizes)//4]:.0f}")
            print(f"    Median:       ${sl_sizes[len(sl_sizes)//2]:.0f}")
            print(f"    p75:          ${sl_sizes[3*len(sl_sizes)//4]:.0f}")
            print(f"    Max SL loss:  ${max(sl_sizes):.0f}")
            over_100 = sum(1 for s in sl_sizes if s > 100)
            print(f"    Losses >$100: {over_100}/{len(sl_sizes)}  ({100*over_100/len(sl_sizes):.0f}%)")
            print(f"    --> These are the outliers killing the payoff ratio")
            print(f"    --> If capped at $80, avg loser = ${sum(min(s,80) for s in sl_sizes)/len(sl_sizes):.0f}")

    print(f"\n{'='*60}\n")

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if not path:
        for p in DEFAULT_PATHS:
            if os.path.exists(p):
                path = p
                break
    if not path or not os.path.exists(path):
        print(f"Usage: python scripts/mae_mfe_analysis.py <omega_trade_closes.csv>")
        print(f"Tried: {DEFAULT_PATHS}")
        sys.exit(1)
    print(f"Loading: {path}")
    rows = load(path)
    analyse(rows)
