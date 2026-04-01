#!/usr/bin/env python3
"""
MAE/MFE Analysis for Omega trade CSV
Reads omega_trade_closes.csv and produces actionable exit optimisation report.

CSV columns (confirmed from header):
  trade_id, trade_ref, entry_ts_unix, entry_ts_utc, entry_utc_weekday,
  exit_ts_unix, exit_ts_utc, exit_utc_weekday, symbol, engine, side,
  entry_px, exit_px, tp, sl, size, gross_pnl, net_pnl,
  slippage_entry, slippage_exit, commission, ...
  mfe, mae, hold_sec, spread_at_entry, latency_ms, regime, exit_reason

MFE/MAE are in raw price POINTS. Convert to USD:
  XAUUSD/XAGUSD: pts * size * 100
  USTEC.F/US500.F/DJ30.F/NAS100: pts * size * tick_value_per_lot
  EURUSD/GBPUSD/etc: pts * size * 100000 (not useful here)

Usage:
  python scripts/mae_mfe_analysis.py
  python scripts/mae_mfe_analysis.py C:\\Omega\\logs\\trades\\omega_trade_closes.csv
"""
import sys, csv, os
from collections import defaultdict

DEFAULT_PATHS = [
    "C:/Omega/logs/trades/omega_trade_closes.csv",
    "logs/trades/omega_trade_closes.csv",
    "../logs/trades/omega_trade_closes.csv",
]

def f(v, default=0.0):
    try: return float(v or default)
    except: return default

def tick_mult(symbol):
    """Dollar value per 1pt move per 1 lot."""
    s = symbol.upper()
    if s in ("XAUUSD", "XAGUSD"):        return 100.0
    if s in ("USTEC.F", "NAS100"):       return 20.0
    if s in ("US500.F",):                return 50.0
    if s in ("DJ30.F",):                 return 5.0
    if s in ("GER40", "UK100", "ESTX50"):return 1.0
    if s in ("USOIL.F", "BRENT"):        return 1000.0
    return 100.0  # default

def mfe_usd(row):
    """MFE in USD = raw_pts * size * tick_mult."""
    pts  = f(row.get('mfe', 0))
    size = f(row.get('size', 0))
    mult = tick_mult(row.get('symbol', ''))
    return pts * size * mult

def mae_usd(row):
    pts  = f(row.get('mae', 0))
    size = f(row.get('size', 0))
    mult = tick_mult(row.get('symbol', ''))
    return pts * size * mult

def load(path):
    with open(path, newline='', encoding='utf-8-sig') as fh:
        return list(csv.DictReader(fh))

def analyse(rows):
    # Full closes only — exclude partials and open positions
    full = [r for r in rows
            if r.get('exit_reason','') not in ('PARTIAL_1R','PARTIAL_2R','')
            and r.get('exit_reason','') != '']

    if not full:
        print("No full-close trades found in CSV.")
        return

    print(f"\n{'='*64}")
    print(f"  OMEGA MAE/MFE EXIT OPTIMISATION REPORT")
    print(f"  {len(full)} full-close trades analysed")
    print(f"{'='*64}")

    # Group by engine
    engines = defaultdict(list)
    for r in full:
        engines[r.get('engine','?')].append(r)

    for eng, trades in sorted(engines.items()):
        wins   = [t for t in trades if f(t.get('net_pnl')) > 0]
        losses = [t for t in trades if f(t.get('net_pnl')) <= 0]
        if len(trades) < 3:
            continue

        net_wins  = [f(t['net_pnl']) for t in wins]
        net_loss  = [f(t['net_pnl']) for t in losses]
        mfe_wins  = [mfe_usd(t) for t in wins  if mfe_usd(t) > 0]
        mfe_loss  = [mfe_usd(t) for t in losses if mfe_usd(t) > 0]

        wr = len(wins)/len(trades)
        print(f"\n  [{eng}]  {len(trades)} trades  WR={wr*100:.0f}%  "
              f"net=${sum(f(t['net_pnl']) for t in trades):+.2f}")
        print(f"  {'─'*60}")

        if wins:
            avg_w = sum(net_wins)/len(net_wins)
            print(f"  WINNERS ({len(wins)})  avg=${avg_w:+.2f}")
            if mfe_wins:
                avg_mfe = sum(mfe_wins)/len(mfe_wins)
                cap_pct  = sum(net_wins)/sum(mfe_wins)*100 if sum(mfe_wins) > 0 else 0
                s = sorted(mfe_wins)
                print(f"    MFE avg=${avg_mfe:.2f}  "
                      f"p25=${s[len(s)//4]:.2f}  p50=${s[len(s)//2]:.2f}  p75=${s[3*len(s)//4]:.2f}")
                print(f"    Capture ratio: {cap_pct:.0f}%  (net P&L / peak MFE)")
                if cap_pct < 50:
                    print(f"    ⚠  Low capture — trail exiting too early or giving back too much")

        if losses:
            avg_l = sum(net_loss)/len(net_loss)
            print(f"  LOSERS  ({len(losses)})  avg=${avg_l:.2f}")
            if mfe_loss:
                avg_mfe_l = sum(mfe_loss)/len(mfe_loss)
                print(f"    Avg MFE before loss: ${avg_mfe_l:.2f}  "
                      f"(was in profit this much before reversing)")

            # SL hits that had meaningful profit first
            sl_hits = [t for t in losses if 'SL' in t.get('exit_reason','')]
            if sl_hits:
                risk_usd = [abs(f(t['net_pnl'])) for t in sl_hits]
                mfe_sl   = [mfe_usd(t) for t in sl_hits]
                # Reached half the risk amount in profit before SL
                half_r   = sum(1 for m,r in zip(mfe_sl,risk_usd) if r>0 and m > r*0.5)
                one_r    = sum(1 for m,r in zip(mfe_sl,risk_usd) if r>0 and m > r)
                print(f"    SL hits reaching >0.5R profit first: {half_r}/{len(sl_hits)}"
                      f"  ({100*half_r/max(1,len(sl_hits)):.0f}%)")
                print(f"    SL hits reaching >1.0R profit first: {one_r}/{len(sl_hits)}"
                      f"  ({100*one_r/max(1,len(sl_hits)):.0f}%)")
                if half_r >= 2:
                    print(f"    ⚠  Partial trigger too late — {half_r} trades banked nothing "
                          f"before reversing through full SL")

        # Payoff analysis
        if wins and losses:
            avg_w = sum(net_wins)/len(net_wins)
            avg_l = abs(sum(net_loss)/len(net_loss))
            exp   = wr*avg_w - (1-wr)*avg_l
            be_wr = avg_l/(avg_w+avg_l)
            print(f"\n  Payoff ratio: {avg_w/avg_l:.2f}x  "
                  f"Expectancy: ${exp:+.2f}/trade  "
                  f"Break-even WR: {be_wr*100:.0f}%")
            if avg_w/avg_l < 1.0:
                print(f"  ⚠  Negative payoff — need avg winner ${avg_l:.0f} "
                      f"(currently ${avg_w:.0f}) OR {be_wr*100:.0f}% WR (have {wr*100:.0f}%)")

    # ── SL size distribution for GoldFlow ──────────────────────────────────
    gf = [r for r in full if 'Flow' in r.get('engine','')]
    sl_gf = [r for r in gf if 'SL' in r.get('exit_reason','')]
    if sl_gf:
        sizes = sorted([abs(f(r['net_pnl'])) for r in sl_gf])
        avg_s  = sum(sizes)/len(sizes)
        print(f"\n  {'='*60}")
        print(f"  GOLDFLOW SL DISTRIBUTION  ({len(sizes)} SL hits)")
        print(f"    min=${min(sizes):.0f}  "
              f"p25=${sizes[len(sizes)//4]:.0f}  "
              f"median=${sizes[len(sizes)//2]:.0f}  "
              f"p75=${sizes[3*len(sizes)//4]:.0f}  "
              f"max=${max(sizes):.0f}  "
              f"avg=${avg_s:.0f}")
        over = {80:0, 100:0, 120:0}
        for s in sizes:
            for cap in over: over[cap] += (s > cap)
        for cap, n in over.items():
            capped_avg = sum(min(s,cap) for s in sizes)/len(sizes)
            saved_total = (avg_s - capped_avg) * len(sizes)
            print(f"    Cap ${cap}: avg=${capped_avg:.0f}  "
                  f"({n}/{len(sizes)} capped)  "
                  f"saves ${avg_s-capped_avg:.0f}/trade  "
                  f"${saved_total:.0f} total this sample")

    # ── Session totals ──────────────────────────────────────────────────────
    all_net  = sum(f(r['net_pnl']) for r in full)
    all_wins = sum(1 for r in full if f(r['net_pnl']) > 0)
    all_avg_w = sum(f(r['net_pnl']) for r in full if f(r['net_pnl'])>0) / max(1,all_wins)
    all_loss  = len(full) - all_wins
    all_avg_l = abs(sum(f(r['net_pnl']) for r in full if f(r['net_pnl'])<=0)) / max(1,all_loss)
    print(f"\n  {'='*60}")
    print(f"  ALL ENGINES COMBINED")
    print(f"    {len(full)} trades  WR={100*all_wins/len(full):.0f}%  "
          f"Net=${all_net:+.2f}")
    print(f"    Avg winner: ${all_avg_w:.2f}  Avg loser: ${all_avg_l:.2f}  "
          f"Payoff: {all_avg_w/all_avg_l:.2f}x")
    print(f"{'='*64}\n")

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if not path:
        for p in DEFAULT_PATHS:
            if os.path.exists(p): path = p; break
    if not path or not os.path.exists(path):
        print(f"Usage: python scripts/mae_mfe_analysis.py <omega_trade_closes.csv>")
        print(f"Tried: {DEFAULT_PATHS}")
        sys.exit(1)
    print(f"Loading: {path}")
    analyse(load(path))
