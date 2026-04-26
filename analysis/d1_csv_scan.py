#!/usr/bin/env python3
"""
d1_csv_scan.py  --  S40/S41 D1 audit pass on omega_trade_closes.csv.
Re-evaluate engine kills with FORCE_CLOSE / DOLLAR_STOP / WEEKEND_CLOSE excluded,
verify culled engines actually stopped firing when their cull commit landed,
and (S41 Phase 1) produce per-engine x exit-reason pivot + exit-asymmetry summary.
Usage: python d1_csv_scan.py <path-to-omega_trade_closes.csv>
"""

import csv, sys, os
from collections import defaultdict
from datetime import datetime

CULL_MAP = {
    'CandleFlow':            ('CULLED',   '2026-04-24', 'S19 Stage 1B  -- header removed'),
    'BBMeanReversion':       ('CULLED',   '2026-04-24', 'S19  -- header removed'),
    'CompressionBreakout':   ('CULLED',   '2026-04-23', 'S16'),
    'DomPersist':            ('CULLED',   '2026-04-23', 'S15'),
    'MicroMomentum':         ('CULLED',   '2026-04-20', 'Batch 5V'),
    'GoldFlow':              ('CULLED',   '2026-04-24', 'S19 Stage 1B'),
    'TickScalp':             ('CULLED',   '2026-04-20', 'Batch 5V'),
    'SilverTurtle':          ('CULLED',   '2026-04-19', 'Batch 5V'),
    'LatencyEdge':           ('CULLED',   '2026-04-24', 'S13'),
    'MacroCrash':            ('DISABLED', '2026-04-24', 'S17 demote -- code preserved'),
    'TrendPullbackEngine_gold':  ('DISABLED', '?',     'Comment-disabled in init'),
    'VWAPReversion':         ('DISABLED', '?',         'enabled=false on all 4 instances'),
}

POLLUTION_REASONS = {'FORCE_CLOSE', 'DOLLAR_STOP', 'WEEKEND_CLOSE'}

# S41 Section 5 reason groupings for the asymmetry summary.
# SL_HIT and bare 'SL' are grouped because they both denote hard-stop exits.
TRAIL_REASONS      = {'TRAIL_HIT', 'TRAIL_SL'}
STOP_REASONS       = {'SL_HIT', 'SL'}
STAGNATION_REASONS = {'STAGNATION'}
PARTIAL_REASONS    = {'PARTIAL_TP', 'PARTIAL_1R', 'PARTIAL_2R'}

# Cap the per-engine x exit-reason matrix at this many reason columns;
# remaining reasons aggregate into a final OTHER column.
TOP_REASONS_CAP = 12

# Cell symbol used in the matrix for empty (engine, reason) combinations.
EMPTY_CELL = '\u00b7'  # centred dot

def parse_ts(s):
    if s is None: return None
    s = str(s).strip()
    if not s: return None
    try:
        v = float(s)
        if v > 1e12:    return datetime.utcfromtimestamp(v / 1000.0)
        if v > 1e9:     return datetime.utcfromtimestamp(v)
    except ValueError:
        pass
    for fmt in ('%Y-%m-%d %H:%M:%S', '%Y-%m-%dT%H:%M:%S',
                '%Y-%m-%d %H:%M:%S.%f', '%Y-%m-%dT%H:%M:%S.%f',
                '%Y-%m-%d', '%m/%d/%Y %H:%M:%S',
                '%Y-%m-%dT%H:%M:%SZ', '%Y-%m-%d %H:%M:%S.%fZ',
                '%Y-%m-%dT%H:%M:%S.%fZ'):
        try: return datetime.strptime(s, fmt)
        except ValueError: continue
    return None

def fmt_money(x):
    return f"${x:>+10.2f}"

def fmt_money_short(x):
    """Compact dollar formatting for matrix cells. Width 10 incl. sign."""
    return f"${x:>+9.2f}"

def fmt_cell(n, pnl):
    """Matrix cell: 'N / $PnL' on one line, fixed width 16."""
    return f"{n:>3}/{fmt_money_short(pnl)}"

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    path = sys.argv[1]
    if not os.path.exists(path):
        print(f"ERROR: {path} not found"); sys.exit(2)

    with open(path, newline='') as f:
        sample = f.read(2048); f.seek(0)
        try:
            dialect = csv.Sniffer().sniff(sample)
        except Exception:
            dialect = csv.excel
        reader = csv.DictReader(f, dialect=dialect)
        cols = reader.fieldnames or []
        rows = list(reader)

    print("=" * 78)
    print(" D1 CSV SCAN  --  pollution-aware engine audit")
    print("=" * 78)
    print(f" File:  {path}")
    print(f" Cols:  {cols}")
    print(f" Rows:  {len(rows)}")
    print()

    def pick(*names):
        for n in names:
            for c in cols:
                if c.lower() == n.lower(): return c
        return None
    f_engine  = pick('engine','engine_name','strategy')
    f_reason  = pick('exit_reason','reason','exitReason','exit')
    f_pnl     = pick('net_pnl','pnl','net','net_p_l','total','profit')
    f_close   = pick('exit_ts_utc','exit_ts_unix','close_ts','exit_ts','close_time','exitTime','timestamp','ts','time')
    if not (f_engine and f_reason and f_pnl and f_close):
        print(f"ERROR: missing required fields. engine={f_engine} reason={f_reason} "
              f"pnl={f_pnl} close={f_close}")
        sys.exit(2)
    print(f" Field map: engine={f_engine}  reason={f_reason}  pnl={f_pnl}  close={f_close}")
    print()

    by_engine = defaultdict(lambda: {'trades': [], 'first_ts': None, 'last_ts':  None})
    parse_failures = 0
    for r in rows:
        eng = (r.get(f_engine) or '').strip()
        rsn = (r.get(f_reason) or '').strip().upper()
        try: pnl = float(r.get(f_pnl) or 0.0)
        except ValueError: pnl = 0.0
        ts = parse_ts(r.get(f_close) or '')
        if ts is None and (r.get(f_close) or '').strip():
            parse_failures += 1
        d = by_engine[eng]
        d['trades'].append((ts, rsn, pnl))
        if ts:
            if d['first_ts'] is None or ts < d['first_ts']: d['first_ts'] = ts
            if d['last_ts']  is None or ts > d['last_ts']:  d['last_ts']  = ts
    if parse_failures:
        print(f" WARN: {parse_failures} rows had unparseable timestamps in {f_close}")
        print()

    print("-" * 78)
    print(" SECTION 1  --  Per-engine: ALL exits vs CLEAN (FORCE_CLOSE/DOLLAR_STOP/WEEKEND_CLOSE excluded)")
    print("-" * 78)
    hdr = f" {'Engine':<32}  {'N_all':>5}  {'PnL_all':>11}  {'WR_all':>6}    {'N_clean':>7}  {'PnL_clean':>11}  {'WR_clean':>8}    {'%poll':>5}"
    print(hdr)
    print(" " + "-" * (len(hdr)-1))
    summary = []
    for eng in sorted(by_engine.keys()):
        d = by_engine[eng]
        all_trades = d['trades']
        n_all = len(all_trades)
        if n_all == 0: continue
        pnl_all = sum(p for _,_,p in all_trades)
        w_all   = sum(1 for _,_,p in all_trades if p > 0)
        wr_all  = (100.0 * w_all / n_all) if n_all else 0.0
        clean = [(ts,rs,p) for (ts,rs,p) in all_trades if rs not in POLLUTION_REASONS]
        n_clean = len(clean)
        pnl_clean = sum(p for _,_,p in clean)
        w_clean = sum(1 for _,_,p in clean if p > 0)
        wr_clean = (100.0 * w_clean / n_clean) if n_clean else 0.0
        pct_poll = 100.0 * (n_all - n_clean) / n_all if n_all else 0.0
        summary.append((eng, n_all, pnl_all, wr_all, n_clean, pnl_clean, wr_clean, pct_poll))
    summary.sort(key=lambda x: x[2])
    for s in summary:
        eng, n_all, pnl_all, wr_all, n_clean, pnl_clean, wr_clean, pct_poll = s
        print(f" {eng[:31]:<32}  {n_all:>5}  {fmt_money(pnl_all)}  {wr_all:>5.1f}%   {n_clean:>7}  {fmt_money(pnl_clean)}  {wr_clean:>7.1f}%   {pct_poll:>4.1f}%")
    print()

    print("-" * 78)
    print(" SECTION 2  --  Did culled / disabled engines actually stop firing?")
    print("-" * 78)
    print(f" {'Engine match':<28} {'State':<9} {'Cull date':<11} {'Last trade':<19} {'After cull':<10} {'Status'}")
    print(" " + "-" * 95)
    for key, (action, date, _note) in CULL_MAP.items():
        matching = [(e, d) for e, d in by_engine.items() if key.lower() in e.lower()]
        if not matching:
            print(f" {key:<28} {action:<9} {date:<11} {'(no trades found)':<19} {'-':<10} {'no record'}")
            continue
        for eng, d in matching:
            last = d['last_ts'].strftime('%Y-%m-%d %H:%M:%S') if d['last_ts'] else '?'
            n_after = 0
            if date != '?' and d['trades']:
                cull_dt = parse_ts(date)
                if cull_dt:
                    n_after = sum(1 for ts,_,_ in d['trades'] if ts and ts > cull_dt)
            status = 'CLEAN' if n_after == 0 else 'STILL FIRING'
            print(f" {eng[:27]:<28} {action:<9} {date:<11} {last:<19} {n_after:<10} {status}")
    print()

    print("-" * 78)
    print(" SECTION 3  --  Pollution distribution overall")
    print("-" * 78)
    by_reason = defaultdict(lambda: {'n':0, 'pnl':0.0})
    for r in rows:
        rsn = (r.get(f_reason) or '').strip().upper()
        try: pnl = float(r.get(f_pnl) or 0.0)
        except ValueError: pnl = 0.0
        by_reason[rsn]['n']   += 1
        by_reason[rsn]['pnl'] += pnl
    total_n = sum(v['n'] for v in by_reason.values())
    total_p = sum(v['pnl'] for v in by_reason.values())
    print(f" {'Reason':<22} {'N':>6} {'%N':>6} {'PnL':>13} {'%PnL':>7}")
    for rsn in sorted(by_reason.keys(), key=lambda k: -abs(by_reason[k]['pnl'])):
        d = by_reason[rsn]
        pct_n = 100.0 * d['n'] / total_n if total_n else 0.0
        pct_p = 100.0 * d['pnl'] / total_p if abs(total_p) > 1e-9 else 0.0
        marker = '  <-- pollution' if rsn in POLLUTION_REASONS else ''
        print(f" {rsn:<22} {d['n']:>6} {pct_n:>5.1f}% {fmt_money(d['pnl'])}  {pct_p:>6.1f}%{marker}")
    print()

    print("-" * 78)
    print(" SECTION 4  --  Engines ranked by CLEAN P&L")
    print("-" * 78)
    summary.sort(key=lambda x: x[5])
    print(f" {'Engine':<32}  {'N_clean':>7}  {'PnL_clean':>11}  {'WR_clean':>8}")
    for s in summary:
        eng, _, _, _, n_clean, pnl_clean, wr_clean, _ = s
        if n_clean == 0: continue
        print(f" {eng[:31]:<32}  {n_clean:>7}  {fmt_money(pnl_clean)}  {wr_clean:>7.1f}%")
    print()

    print_section_5(by_engine, summary, by_reason)

    print(" Pollution reasons treated as restart/operator noise:")
    print(" ", ', '.join(sorted(POLLUTION_REASONS)))


def print_section_5(by_engine, summary, by_reason):
    """
    S41 Phase 1: per-engine x exit-reason pivot + exit-asymmetry summary.

    5a: Matrix of (engine, reason) -> 'N / $PnL'. Engines sorted by clean PnL
        ascending (matches Section 4). Reason columns are top TOP_REASONS_CAP
        by absolute total PnL impact (from Section 3 totals); everything else
        aggregates into a final OTHER column. Pollution columns flagged with *.

    5b: Per-engine exit-asymmetry summary highlighting hard-stop bleed vs
        trail/partial profits. Asymmetry = $TRAIL + $PARTIAL + $SL_HIT + $STAGNATION
        (signed sum -- SL/STAG PnLs are already negative when bleeding).
        Negative = engine bleeds through stops/stagnation faster than it earns
        through trails/partials. Positive = trails/partials cover the bleed.
    """
    # Engine ordering: clean PnL ascending (worst bleeders first).
    # summary tuples: (eng, n_all, pnl_all, wr_all, n_clean, pnl_clean, wr_clean, pct_poll)
    engines_ordered = [s[0] for s in sorted(summary, key=lambda x: x[5]) if s[1] > 0]

    # Reason ordering: top N by absolute total PnL impact (matches Section 3 sort).
    reasons_sorted = sorted(by_reason.keys(), key=lambda k: -abs(by_reason[k]['pnl']))
    top_reasons = reasons_sorted[:TOP_REASONS_CAP]
    other_reasons = set(reasons_sorted[TOP_REASONS_CAP:])

    # Build (engine, reason) -> (n, pnl) pivot from raw trades.
    pivot = defaultdict(lambda: {'n': 0, 'pnl': 0.0})
    for eng, d in by_engine.items():
        for ts, rsn, pnl in d['trades']:
            pivot[(eng, rsn)]['n']   += 1
            pivot[(eng, rsn)]['pnl'] += pnl

    print("-" * 78)
    print(" SECTION 5a  --  Per-engine x exit-reason matrix (N / $PnL)")
    print(f"             Top {TOP_REASONS_CAP} reasons by |PnL| shown; rest aggregated as OTHER.")
    print(f"             Empty cell = '{EMPTY_CELL}'.  Pollution columns marked with '*'.")
    print("-" * 78)

    cell_w = 16
    eng_w  = 28
    col_labels = []
    for r in top_reasons:
        label = r if r else '(blank)'
        if r in POLLUTION_REASONS:
            label = label + '*'
        col_labels.append(label[:cell_w])
    col_labels.append('OTHER')
    col_labels.append('TOTAL')

    # Header rows (split long reason names across two lines if needed for width).
    hdr_line = ' ' + 'Engine'.ljust(eng_w)
    for lbl in col_labels:
        hdr_line += lbl.rjust(cell_w)
    print(hdr_line)
    print(' ' + '-' * (eng_w + cell_w * len(col_labels)))

    # Body rows.
    col_totals = {r: {'n': 0, 'pnl': 0.0} for r in top_reasons}
    col_totals['OTHER'] = {'n': 0, 'pnl': 0.0}
    col_totals['TOTAL'] = {'n': 0, 'pnl': 0.0}

    for eng in engines_ordered:
        row_n = 0
        row_pnl = 0.0
        cells = []
        for r in top_reasons:
            cell = pivot.get((eng, r))
            if cell and cell['n'] > 0:
                cells.append(fmt_cell(cell['n'], cell['pnl']))
                row_n   += cell['n']
                row_pnl += cell['pnl']
                col_totals[r]['n']   += cell['n']
                col_totals[r]['pnl'] += cell['pnl']
            else:
                cells.append(EMPTY_CELL.center(cell_w))
        # OTHER aggregate
        other_n = 0
        other_pnl = 0.0
        for r in other_reasons:
            cell = pivot.get((eng, r))
            if cell and cell['n'] > 0:
                other_n   += cell['n']
                other_pnl += cell['pnl']
        if other_n > 0:
            cells.append(fmt_cell(other_n, other_pnl))
            row_n   += other_n
            row_pnl += other_pnl
            col_totals['OTHER']['n']   += other_n
            col_totals['OTHER']['pnl'] += other_pnl
        else:
            cells.append(EMPTY_CELL.center(cell_w))
        # TOTAL
        cells.append(fmt_cell(row_n, row_pnl))
        col_totals['TOTAL']['n']   += row_n
        col_totals['TOTAL']['pnl'] += row_pnl

        line = ' ' + eng[:eng_w].ljust(eng_w) + ''.join(c.rjust(cell_w) for c in cells)
        print(line)

    # Footer totals row.
    print(' ' + '-' * (eng_w + cell_w * len(col_labels)))
    foot = ' ' + 'TOTAL'.ljust(eng_w)
    for r in top_reasons:
        t = col_totals[r]
        foot += fmt_cell(t['n'], t['pnl']).rjust(cell_w)
    foot += fmt_cell(col_totals['OTHER']['n'], col_totals['OTHER']['pnl']).rjust(cell_w)
    foot += fmt_cell(col_totals['TOTAL']['n'], col_totals['TOTAL']['pnl']).rjust(cell_w)
    print(foot)
    print()

    # 5b -- exit-asymmetry summary.
    print("-" * 78)
    print(" SECTION 5b  --  Per-engine exit asymmetry  (TRAIL+/PARTIAL income vs SL_HIT/STAGNATION bleed)")
    print("                 Asymmetry = $TRAIL + $PARTIAL + $SL_HIT + $STAGNATION  (signed sum)")
    print("                 Negative = bleeds through stops faster than trails/partials cover.")
    print("-" * 78)

    asym_hdr = (
        f" {'Engine':<28}"
        f" {'N':>4}"
        f"   {'%SL':>4} {'$SL':>10}"
        f"   {'%STAG':>5} {'$STAG':>10}"
        f"   {'%TRAIL':>6} {'$TRAIL':>10}"
        f"   {'%PART':>5} {'$PART':>10}"
        f"   {'Asymmetry':>11}"
    )
    print(asym_hdr)
    print(' ' + '-' * (len(asym_hdr) - 1))

    asym_rows = []
    for eng in engines_ordered:
        trades = by_engine[eng]['trades']
        n = len(trades)
        if n == 0: continue
        sl_n   = sum(1 for _,r,_ in trades if r in STOP_REASONS)
        sl_p   = sum(p for _,r,p in trades if r in STOP_REASONS)
        st_n   = sum(1 for _,r,_ in trades if r in STAGNATION_REASONS)
        st_p   = sum(p for _,r,p in trades if r in STAGNATION_REASONS)
        tr_n   = sum(1 for _,r,_ in trades if r in TRAIL_REASONS)
        tr_p   = sum(p for _,r,p in trades if r in TRAIL_REASONS)
        pa_n   = sum(1 for _,r,_ in trades if r in PARTIAL_REASONS)
        pa_p   = sum(p for _,r,p in trades if r in PARTIAL_REASONS)
        asym = tr_p + pa_p + sl_p + st_p
        asym_rows.append((
            eng, n,
            sl_n, sl_p,
            st_n, st_p,
            tr_n, tr_p,
            pa_n, pa_p,
            asym
        ))

    # Sort by asymmetry ascending (worst exit-asymmetry first).
    asym_rows.sort(key=lambda r: r[10])

    for row in asym_rows:
        eng, n, sl_n, sl_p, st_n, st_p, tr_n, tr_p, pa_n, pa_p, asym = row
        pct_sl   = 100.0 * sl_n / n if n else 0.0
        pct_st   = 100.0 * st_n / n if n else 0.0
        pct_tr   = 100.0 * tr_n / n if n else 0.0
        pct_pa   = 100.0 * pa_n / n if n else 0.0
        print(
            f" {eng[:27]:<28}"
            f" {n:>4}"
            f"   {pct_sl:>3.0f}% {fmt_money(sl_p):>10}"
            f"   {pct_st:>4.0f}% {fmt_money(st_p):>10}"
            f"   {pct_tr:>5.0f}% {fmt_money(tr_p):>10}"
            f"   {pct_pa:>4.0f}% {fmt_money(pa_p):>10}"
            f"   {fmt_money(asym):>11}"
        )
    print()


if __name__ == '__main__':
    main()
