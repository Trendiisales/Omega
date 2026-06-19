#!/usr/bin/env python3
import gold_intraday_edgehunt as E

DS = E.datasets()

def half_split(bars):
    m = len(bars)//2
    return bars[:m], bars[m:]

def eval_cell(tf, signal, params, exit_mode, sizing):
    be, bu = DS[tf]
    tbe = E.run(be, signal, params, exit_mode, sizing)
    tbu = E.run(bu, signal, params, exit_mode, sizing)
    sbe = E.stats(tbe); sbu = E.stats(tbu)
    # time-halves within each regime, combined
    be1,be2 = half_split(be); bu1,bu2 = half_split(bu)
    h1 = E.stats(E.run(be1,signal,params,exit_mode,sizing))
    h2 = E.stats(E.run(be2,signal,params,exit_mode,sizing))
    h3 = E.stats(E.run(bu1,signal,params,exit_mode,sizing))
    h4 = E.stats(E.run(bu2,signal,params,exit_mode,sizing))
    all_trades = tbe+tbu
    ft = E.fattail(all_trades)
    sall = E.stats(all_trades)
    return dict(
        tf=tf, signal=signal, params=params, exit=exit_mode, sizing=sizing,
        be_pf=sbe['pf'], be_net=sbe['net'], be_n=sbe['n'],
        bu_pf=sbu['pf'], bu_net=sbu['net'], bu_n=sbu['n'],
        all_pf=sall['pf'], all_net=sall['net'], all_n=sall['n'],
        halves_pf=(h1['pf'],h2['pf'],h3['pf'],h4['pf']),
        fattail_pf=ft,
    )

def main():
    cells = []
    # exits
    trail_exits = [('trail',2.0),('trail',3.0),('trail',4.0)]
    fixedR_exits = [('fixedR',2.0,2.0),('fixedR',3.0,2.0)]  # (R, stop_atr)

    grid = []
    # Donchian
    for N in (20,40,55):
        for ex in trail_exits + fixedR_exits:
            for sz in ('fixed','voltarget'):
                grid.append(('donchian', {'N':N}, ex, sz))
    # Keltner
    for k in (1.5,2.0,2.5):
        for ex in trail_exits + fixedR_exits:
            for sz in ('fixed','voltarget'):
                grid.append(('keltner', {'k':k,'N':20}, ex, sz))
    # ATR-expansion
    for x in (1.5,2.0,2.5):
        for ex in trail_exits:
            for sz in ('fixed','voltarget'):
                grid.append(('atrexp', {'x':x,'N':20}, ex, sz))

    for tf in ('M15','M30','H1'):
        for sig,p,ex,sz in grid:
            cells.append(eval_cell(tf, sig, p, ex, sz))

    # ranking: both-regime positive net AND both-regime PF>1
    def both_pos(c):
        return c['be_net']>0 and c['bu_net']>0 and c['be_pf']>1.0 and c['bu_pf']>1.0
    survivors = [c for c in cells if both_pos(c) and c['be_n']>=15 and c['bu_n']>=15]
    survivors.sort(key=lambda c: min(c['be_pf'],c['bu_pf']), reverse=True)

    def fmt_ex(ex):
        if ex[0]=='trail': return f"trail{ex[1]:.0f}N"
        return f"R{ex[1]:.0f}/s{ex[2]:.0f}"
    def fmt_p(sig,p):
        if sig=='donchian': return f"N{p['N']}"
        if sig=='keltner': return f"k{p['k']}"
        return f"x{p['x']}"

    print("="*150)
    print("BOTH-REGIME SURVIVORS (be_net>0, bu_net>0, both PF>1, n>=15/regime) — ranked by min(be_pf,bu_pf)")
    print("="*150)
    print(f"{'TF':4} {'signal':9} {'param':6} {'exit':9} {'size':9} | {'BE_pf':>6} {'BE_net':>9} {'BE_n':>5} | {'BU_pf':>6} {'BU_net':>9} {'BU_n':>5} | {'ALL_pf':>6} {'ALL_net':>10} | {'minHalfPF':>9} {'FT_pf':>6} robust")
    print("-"*150)
    for c in survivors:
        mh = min(h for h in c['halves_pf'])
        ftr = 'Y' if (c['fattail_pf'] is not None and c['fattail_pf']>1.0) else 'N'
        print(f"{c['tf']:4} {c['signal']:9} {fmt_p(c['signal'],c['params']):6} {fmt_ex(c['exit']):9} {c['sizing']:9} | "
              f"{c['be_pf']:6.2f} {c['be_net']:9.0f} {c['be_n']:5d} | "
              f"{c['bu_pf']:6.2f} {c['bu_net']:9.0f} {c['bu_n']:5d} | "
              f"{c['all_pf']:6.2f} {c['all_net']:10.0f} | "
              f"{mh:9.2f} {(c['fattail_pf'] or 0):6.2f} {ftr}")

    print(f"\nTotal cells: {len(cells)}  |  both-regime survivors: {len(survivors)}")

    # Also dump top M15/M30 cost-survivable specifically
    print("\n" + "="*150)
    print("TOP M15/M30 ONLY (best cross-regime, fat-tail-robust preferred)")
    print("="*150)
    m = [c for c in survivors if c['tf'] in ('M15','M30')]
    for c in m[:12]:
        mh = min(h for h in c['halves_pf'])
        ftr = 'Y' if (c['fattail_pf'] is not None and c['fattail_pf']>1.0) else 'N'
        print(f"{c['tf']:4} {c['signal']:9} {fmt_p(c['signal'],c['params']):6} {fmt_ex(c['exit']):9} {c['sizing']:9} | "
              f"BE_pf {c['be_pf']:5.2f} net {c['be_net']:8.0f} n{c['be_n']:<4d} | "
              f"BU_pf {c['bu_pf']:5.2f} net {c['bu_net']:8.0f} n{c['bu_n']:<4d} | "
              f"minHalfPF {mh:4.2f} FT {(c['fattail_pf'] or 0):4.2f} {ftr}")

if __name__ == '__main__':
    main()
