#!/usr/bin/env python3
"""
CRYPTO BEAR-BOUNCE — long-only BTC/ETH engine for DOWNTREND regimes (S-2026-07-02).

WHY: NZ regulation => long-only crypto. The existing book has no downtrend answer:
  - Luke daily system: regime gate BTC>200MA MANDATORY => sits OUT the whole bear.
  - Entry A (dip-buy pullback) proven DEAD on crypto ("crypto dips keep dipping").
  - Companion book (KELT/EMAX) is two-sided; its long side alone bleeds in bear.
This harness tests the one long edge a crypto bear offers: VIOLENT BEAR RALLIES
(+15..+40% bounces every few weeks). Three candidate entry families, all gated
to BEAR regime (daily close < SMA200 evaluated on COMPLETED daily bar):

  C1 FLUSH-RECLAIM  capitulation snapback: N-day flush <= -X%, then first 4h
                    reclaim bar (close > prior 4h high). Mean-reversion long.
  C2 RALLY-RIDER    4h Donchian breakout long inside bear; rides the rally,
                    exits on 4h Donchian-low break / trend flip.
  C3 EMA9-RECLAIM   daily close crosses above EMA9 after >=K days below
                    (the Luke crypto exit, inverted into a bear-bounce entry);
                    exits on first daily close < EMA9 (ride-wide semantics).
  C4 FLUSH-ARMED BREAKOUT (hybrid): a recent capitulation flush (5d ret <= -X%
                    within the last arm_days) ARMS the setup; a fast 4h Donchian
                    breakout TRIGGERS the long. Kills the mid-range whipsaw
                    entries that bleed C2 dry in grinding bears (2022, 2025-26):
                    bear rallies start from capitulation, not mid-range strength.

PROTECTION STACK (operator semantics, all sweepable, hourly marks):
  - hard initial stop (structure/ATR) -> risk-based sizing (risk_pct of equity)
  - BE-AND-RIDE floor: after capture >= be_arm, floor = entry*(1+be_lock); trade
    keeps riding, exits only through the floor ("covered a little")
  - GIVEBACK clip: after fav >= gate_pct, exit when fav <= mfe*(1-gb)
  - COLD CUT (never-green catastrophe): peak fav <= mfe_eps AND held >= minhold
    bars AND adverse <= -cut  (v2 design from the equity day-mover sweep)
  - TIME stop (hours; 0 = off)

COSTS: 6 bps/side taker (IBKR MBT/MET measured 2-8bps ladder) + 10 bps extra
slip on stop-outs. Stress runs at 2x. Fills: signal on COMPLETED bar, fill at
NEXT hourly open. No lookahead: daily/4h indicators use only completed bars.

Data: <SYM>_1h.csv  ts,open,high,low,close,volume (epoch s, UTC, ascending).
Usage: python3 bear_bounce_bt.py --data <dir> [--phase entries|protect|size]
"""
from __future__ import annotations
import argparse, csv, math, os, sys
from collections import defaultdict

# ---------------- data ----------------

def load_1h(path):
    rows = []
    with open(path) as f:
        r = csv.reader(f); next(r)
        for c in r:
            ts = int(c[0]); o, h, l, cl = map(float, c[1:5])
            if min(o, h, l, cl) <= 0: continue
            rows.append((ts, o, h, l, cl))
    rows.sort()
    return rows

def integrity(rows, name):
    """Data gate: gaps, spikes. Reject-worthy issues -> print + return False."""
    bad_spikes = 0; gaps = 0; maxgap = 0
    for i in range(1, len(rows)):
        dt = rows[i][0] - rows[i-1][0]
        if dt > 3600:
            gaps += 1; maxgap = max(maxgap, dt)
        r = rows[i][4] / rows[i-1][4]
        if r > 1.5 or r < 0.66:  # >50% hourly move = suspect (x1000-class glitch)
            bad_spikes += 1
    span_d = (rows[-1][0] - rows[0][0]) / 86400
    print(f"[GATE] {name}: {len(rows)} bars, {span_d:.0f}d, gaps={gaps} (max {maxgap/3600:.0f}h), spikes={bad_spikes}")
    return bad_spikes == 0

def build_frames(rows):
    """hourly rows -> dict with hourly arrays + completed 4h/daily indicator series
    keyed by hourly index (values = state as of the last COMPLETED 4h/daily bar)."""
    n = len(rows)
    ts  = [r[0] for r in rows]
    o   = [r[1] for r in rows]; h = [r[2] for r in rows]
    l   = [r[3] for r in rows]; c = [r[4] for r in rows]

    # ---- completed daily bars (UTC midnight) ----
    daily = []            # (day_start_ts, o,h,l,c)
    cur = None
    for i in range(n):
        d0 = ts[i] // 86400 * 86400
        if cur is None or d0 != cur[0]:
            if cur is not None: daily.append(tuple(cur))
            cur = [d0, o[i], h[i], l[i], c[i]]
        else:
            cur[2] = max(cur[2], h[i]); cur[3] = min(cur[3], l[i]); cur[4] = c[i]
    # NOTE: trailing partial day intentionally dropped (only completed bars)
    dts = [d[0] for d in daily]
    dcl = [d[4] for d in daily]
    dhi = [d[2] for d in daily]; dlo = [d[3] for d in daily]

    def sma(x, w):
        out = [None]*len(x); s = 0.0
        for i, v in enumerate(x):
            s += v
            if i >= w: s -= x[i-w]
            if i >= w-1: out[i] = s/w
        return out
    def ema(x, span):
        out = [None]*len(x); k = 2.0/(span+1); e = None
        for i, v in enumerate(x):
            e = v if e is None else v*k + e*(1-k)
            out[i] = e
        return out
    def atr(hi, lo, cl, w=14):
        out = [None]*len(cl); trs = []
        for i in range(len(cl)):
            tr = hi[i]-lo[i] if i == 0 else max(hi[i]-lo[i], abs(hi[i]-cl[i-1]), abs(lo[i]-cl[i-1]))
            trs.append(tr)
            if i >= w-1:
                out[i] = sum(trs[i-w+1:i+1])/w
        return out

    dsma200 = sma(dcl, 200); dsma50 = sma(dcl, 50); dsma30 = sma(dcl, 30)
    dsma20 = sma(dcl, 20); dema9 = ema(dcl, 9); dema21 = ema(dcl, 21)
    datr = atr(dhi, dlo, dcl, 14)
    # N-day return
    def nd_ret(nd):
        return [None if i < nd else dcl[i]/dcl[i-nd]-1 for i in range(len(dcl))]
    dret3 = nd_ret(3); dret5 = nd_ret(5)
    # days below ema9 counter
    below9 = [0]*len(dcl)
    for i in range(len(dcl)):
        if dema9[i] is not None and dcl[i] < dema9[i]:
            below9[i] = (below9[i-1] if i else 0) + 1

    # ---- completed 4h bars ----
    q = []                # (q_start_ts, o,h,l,c)
    cur = None
    for i in range(n):
        q0 = ts[i] // 14400 * 14400
        if cur is None or q0 != cur[0]:
            if cur is not None: q.append(tuple(cur))
            cur = [q0, o[i], h[i], l[i], c[i]]
        else:
            cur[2] = max(cur[2], h[i]); cur[3] = min(cur[3], l[i]); cur[4] = c[i]
    qts = [x[0] for x in q]; qhi = [x[2] for x in q]; qlo = [x[3] for x in q]; qcl = [x[4] for x in q]
    qatr = atr(qhi, qlo, qcl, 14)

    # rolling donchian on completed 4h bars: value at index i = max/min of bars (i-w+1..i)
    def roll_max(x, w):
        out = [None]*len(x)
        from collections import deque
        dq = deque()
        for i, v in enumerate(x):
            while dq and x[dq[-1]] <= v: dq.pop()
            dq.append(i)
            if dq[0] <= i-w: dq.popleft()
            if i >= w-1: out[i] = x[dq[0]]
        return out
    def roll_min(x, w):
        out = [None]*len(x)
        from collections import deque
        dq = deque()
        for i, v in enumerate(x):
            while dq and x[dq[-1]] >= v: dq.pop()
            dq.append(i)
            if dq[0] <= i-w: dq.popleft()
            if i >= w-1: out[i] = x[dq[0]]
        return out

    # map hourly index -> index of last COMPLETED daily / 4h bar
    di = [None]*n; qi = [None]*n
    j = -1
    for i in range(n):
        d0 = ts[i] // 86400 * 86400
        while j+1 < len(dts) and dts[j+1] < d0: j += 1
        di[i] = j if j >= 0 else None
    j = -1
    for i in range(n):
        q0 = ts[i] // 14400 * 14400
        while j+1 < len(qts) and qts[j+1] < q0: j += 1
        qi[i] = j if j >= 0 else None

    return {
        'ts': ts, 'o': o, 'h': h, 'l': l, 'c': c, 'n': n,
        'daily': {'ts': dts, 'c': dcl, 'h': dhi, 'l': dlo, 'sma200': dsma200,
                  'sma50': dsma50, 'sma30': dsma30, 'sma20': dsma20,
                  'ema9': dema9, 'ema21': dema21, 'atr': datr,
                  'ret3': dret3, 'ret5': dret5, 'below9': below9},
        'q4': {'ts': qts, 'h': qhi, 'l': qlo, 'c': qcl, 'atr': qatr,
               'roll_max': roll_max, 'roll_min': roll_min},
        'di': di, 'qi': qi,
    }

# ---------------- engine ----------------

DEF = dict(
    candidate='C2',        # C1 | C2 | C3 | C4 | C5
    regime='bear',         # bear | bull | any   (bear = daily close < sma200)
    subgate='none',        # none | sma50 | sma50up | both -- bear SUB-REGIME:
                           # 'sma50'   = close > sma50 (intermediate turn: recovery leg)
                           # 'sma50up' = sma50 rising vs 5d ago
                           # 'both'    = close > rising sma50 (basing/recovery confirmed)
    # C1 flush-reclaim
    flush_nd=3, flush_pct=0.10, arm_hours=96,
    # C2 rally-rider donchian (in completed 4h bars)
    don_in=30, don_out=15,
    # C3 ema9 reclaim
    below_min=3, c3_stop_atr=0.5,
    # C4 flush-armed breakout
    c4_flush=0.12, c4_arm_days=7, c4_don_fast=12,
    # C5 snapback: N-day-low flush within last c5_recent days, trigger = hourly
    # close reclaims prior daily HIGH; exit INTO STRENGTH at tp (bounce-sized)
    c5_lowdays=20, c5_recent=3, tp=0.08,
    # C6 panic snapback (liquidation-cascade overshoot): hourly close <= -panic
    # vs max close of prior 24h -> buy the overshoot, exit at +tp or time stop
    c6_panic=0.08, c6_stop=0.08,
    # C7 W-bottom: pivot low (20d low) in last c7_look days, then a HIGHER low
    # held >= 2 days, then daily close > prior daily high => second-leg entry
    c7_look=12,
    # dual-symbol confirmation: entry allowed only if the OTHER symbol's daily
    # close is also above its EMA9 (both majors turning together)
    dual_confirm=False,
    # protection
    stop_atr=2.0,          # initial hard stop = entry - stop_atr * ATR(4h) (0=off -> uses 25% disaster stop)
    be_arm=0.0, be_lock=0.0,      # BE-and-ride floor (0=off)
    gate_pct=0.015, gb=0.0,       # giveback clip (gb=0 -> off)
    cold_cut=0.0, cut_mfe_eps=0.003, cut_minhold=12,   # v2 catastrophe cut (hours)
    time_stop=0,           # hours, 0=off
    exit_ema9=False,       # trend exit: first daily close < ema9
    # sizing / costs
    risk_pct=0.01, max_pos_pct=0.5, equity0=100_000.0,
    cost_bps=6.0, stop_slip_bps=10.0,
)

def simulate(frames_by_sym, P):
    cost = P['cost_bps']/1e4; slip = P['stop_slip_bps']/1e4
    equity = P['equity0']
    trades = []
    # unified hourly timeline
    all_ts = sorted(set().union(*[set(f['ts']) for f in frames_by_sym.values()]))
    idx = {s: {t: i for i, t in enumerate(f['ts'])} for s, f in frames_by_sym.items()}
    pos = {}            # sym -> dict
    pending = {}        # sym -> signal dict to fill at next hourly open
    eq_curve = []

    # precompute donchian arrays per symbol (C2/C4)
    don = {}
    for s, f in frames_by_sym.items():
        q = f['q4']
        don[s] = {'hi': q['roll_max'](q['h'], P['don_in']),
                  'lo': q['roll_min'](q['l'], P['don_out']),
                  'fast_hi': q['roll_max'](q['h'], P['c4_don_fast'])}

    for t in all_ts:
        for s, f in frames_by_sym.items():
            i = idx[s].get(t)
            if i is None: continue
            ts, o, h, l, c = f['ts'][i], f['o'][i], f['h'][i], f['l'][i], f['c'][i]

            # ---- fill pending entry at this hour's open ----
            if s in pending and s not in pos:
                sig = pending.pop(s)
                entry = o * (1 + cost)
                stop = sig['stop']
                if stop >= entry:          # degenerate
                    pass
                else:
                    riskps = entry - stop
                    qty = (equity * P['risk_pct']) / riskps
                    qty = min(qty, equity * P['max_pos_pct'] / entry)
                    pos[s] = dict(entry=entry, qty=qty, stop=stop, t_in=ts,
                                  mfe=0.0, floor=None, kind=sig['kind'], bars=0,
                                  tp_px=entry * (1 + P['tp']) if sig.get('use_tp') else None)
            elif s in pending:
                pending.pop(s)

            # ---- manage open position (hourly marks) ----
            if s in pos:
                p = pos[s]; p['bars'] += 1
                fav_hi = h / p['entry'] - 1.0
                p['mfe'] = max(p['mfe'], fav_hi)
                exit_px = None; reason = None
                # 1. hard stop (intrabar) -- stop checked BEFORE tp (worst-case same-bar)
                if l <= p['stop']:
                    exit_px = p['stop'] * (1 - slip); reason = 'STOP'
                # 1b. profit target intrabar (exit into strength)
                if exit_px is None and p.get('tp_px') and h >= p['tp_px']:
                    exit_px = p['tp_px']; reason = 'TP'
                # 2. BE floor (intrabar, only after armed)
                if exit_px is None and p['floor'] is not None and l <= p['floor']:
                    exit_px = p['floor'] * (1 - slip); reason = 'BE_FLOOR'
                if exit_px is None:
                    fav_c = c / p['entry'] - 1.0
                    adverse = 1.0 - c / p['entry']
                    # arm BE floor
                    if P['be_arm'] > 0 and p['floor'] is None and p['mfe'] >= P['be_arm']:
                        p['floor'] = p['entry'] * (1 + P['be_lock'])
                    # 3. giveback clip (close-based)
                    if P['gb'] > 0 and p['mfe'] >= P['gate_pct'] and fav_c <= p['mfe'] * (1 - P['gb']):
                        exit_px = c; reason = 'GB_CLIP'
                    # 4. cold cut v2 (never-green catastrophe)
                    elif (P['cold_cut'] > 0 and p['mfe'] <= P['cut_mfe_eps']
                          and p['bars'] >= P['cut_minhold'] and adverse >= P['cold_cut']):
                        exit_px = c; reason = 'COLD_CUT'
                    # 5. time stop
                    elif P['time_stop'] > 0 and p['bars'] >= P['time_stop']:
                        exit_px = c; reason = 'TIME'
                    # 6. trend exit at completed-daily boundary
                    elif P['exit_ema9']:
                        dj = f['di'][i]
                        if dj is not None and dj > 0:
                            D = f['daily']
                            if (ts % 86400) < 3600 and D['ema9'][dj] is not None and D['c'][dj] < D['ema9'][dj] \
                               and D['ts'][dj] > p['t_in'] - 86400:
                                exit_px = o; reason = 'EMA9_FLIP'
                    # 6b. C2/C4 donchian-low exit at completed-4h boundary
                    if exit_px is None and P['candidate'] in ('C2', 'C4'):
                        qj = f['qi'][i]
                        if qj is not None and qj >= 1 and (ts % 14400) < 3600:
                            dlo = don[s]['lo'][qj-1]
                            if dlo is not None and f['q4']['c'][qj] < dlo:
                                exit_px = o; reason = 'DON_OUT'
                if exit_px is not None:
                    px = exit_px * (1 - cost)
                    pnl = (px - p['entry']) * p['qty']
                    equity += pnl
                    trades.append(dict(sym=s, t_in=p['t_in'], t_out=ts, entry=p['entry'],
                                       exit=px, pnl=pnl, r=(px/p['entry']-1) / max(1e-9, 1-p['stop']/p['entry']),
                                       ret=px/p['entry']-1, mfe=p['mfe'], hold_h=p['bars'],
                                       reason=reason, kind=p['kind'], eq=equity))
                    del pos[s]

            # ---- signal generation on completed bars (fill next hour) ----
            if s in pos or s in pending: continue
            dj = f['di'][i]; qj = f['qi'][i]
            if dj is None or qj is None: continue
            D = f['daily']; Q = f['q4']
            if D['sma200'][dj] is None or Q['atr'][qj] is None: continue
            bear = D['c'][dj] < D['sma200'][dj]
            if P['regime'] == 'bear' and not bear: continue
            if P['regime'] == 'bull' and bear: continue
            sg = P['subgate']
            if sg != 'none':
                if sg in ('ma20', 'ma30', 'ma50'):     # close > RISING MA(w)
                    key = {'ma20': 'sma20', 'ma30': 'sma30', 'ma50': 'sma50'}[sg]
                    mv = D[key][dj]; mvp = D[key][dj-5] if dj >= 5 else None
                    if mv is None or mvp is None: continue
                    if not (D['c'][dj] > mv and mv > mvp): continue
                else:
                    s50 = D['sma50'][dj]; s50p = D['sma50'][dj-5] if dj >= 5 else None
                    if s50 is None or s50p is None: continue
                    if sg in ('sma50', 'both') and not D['c'][dj] > s50: continue
                    if sg in ('sma50up', 'both') and not s50 > s50p: continue

            sig = None
            if P['candidate'] == 'C1':
                nd = P['flush_nd']; key = 'ret3' if nd == 3 else 'ret5'
                fl = D[key][dj]
                if fl is not None and fl <= -P['flush_pct']:
                    # reclaim trigger: last completed 4h close > prior 4h high
                    if qj >= 1 and Q['c'][qj] > Q['h'][qj-1]:
                        stop = min(Q['l'][qj], Q['l'][qj-1]) - 0.25 * Q['atr'][qj]
                        sig = dict(kind='C1', stop=stop)
            elif P['candidate'] == 'C2':
                dh = don[s]['hi'][qj-1] if qj >= 1 else None
                if dh is not None and Q['c'][qj] > dh and (ts % 14400) < 3600:
                    stop = Q['c'][qj] - P['stop_atr'] * Q['atr'][qj]
                    sig = dict(kind='C2', stop=stop)
            elif P['candidate'] == 'C3':
                if (ts % 86400) < 3600 and dj >= 1 and D['ema9'][dj] is not None:
                    crossed = D['c'][dj] > D['ema9'][dj] and D['below9'][dj-1] >= P['below_min']
                    if crossed:
                        stop = D['l'][dj] - P['c3_stop_atr'] * (D['atr'][dj] or 0)
                        sig = dict(kind='C3', stop=stop)
            elif P['candidate'] == 'C7':
                # W-bottom on daily bars, checked at first hour of new day
                if (ts % 86400) < 3600 and dj >= P['c7_look'] + 21:
                    look = P['c7_look']
                    # pivot: the min low of the lookback window is also a 20d low
                    win_lo = D['l'][dj-look:dj+1]
                    pmin = min(win_lo); pidx = dj - look + win_lo.index(pmin)
                    ok = (pidx < dj - 1                                  # pivot >=2d ago
                          and pmin <= min(D['l'][pidx-20:pidx])          # was a 20d low
                          and min(D['l'][pidx+1:dj+1]) > pmin            # never undercut since
                          and D['c'][dj] > D['h'][dj-1])                 # strong-close trigger
                    if ok:
                        stop = pmin - 0.25 * (D['atr'][dj] or 0)
                        sig = dict(kind='C7', stop=stop)
            elif P['candidate'] == 'C6':
                # max hourly close over prior 24h (excluding current bar)
                if i >= 25:
                    m24 = max(f['c'][i-24:i])
                    if c / m24 - 1.0 <= -P['c6_panic']:
                        sig = dict(kind='C6', stop=c * (1 - P['c6_stop']), use_tp=True)
            elif P['candidate'] == 'C5':
                # flush: a c5_lowdays-day low printed within the last c5_recent
                # completed daily bars; trigger: THIS hourly close reclaims the
                # prior completed daily bar's HIGH (early day-0/1 turn signal)
                w = P['c5_lowdays']
                if dj >= w:
                    flush_lo = None
                    for k in range(P['c5_recent']):
                        jj = dj - k
                        if jj >= w and D['l'][jj] <= min(D['l'][jj-w:jj]):
                            flush_lo = min(D['l'][dj-P['c5_recent']+1:dj+1])
                            break
                    if flush_lo is not None and c > D['h'][dj]:
                        stop = flush_lo - 0.25 * (D['atr'][dj] or 0)
                        sig = dict(kind='C5', stop=stop, use_tp=True)
            elif P['candidate'] == 'C4':
                # armed: a >=c4_flush 5d flush within the last c4_arm_days daily bars
                armed = False
                for k in range(P['c4_arm_days']):
                    v = D['ret5'][dj-k] if dj-k >= 0 else None
                    if v is not None and v <= -P['c4_flush']:
                        armed = True; break
                if armed and qj >= 1 and (ts % 14400) < 3600:
                    fh = don[s]['fast_hi'][qj-1]
                    if fh is not None and Q['c'][qj] > fh:
                        stop = Q['c'][qj] - P['stop_atr'] * Q['atr'][qj]
                        sig = dict(kind='C4', stop=stop)
            if sig is not None and P['dual_confirm']:
                other = [s2 for s2 in frames_by_sym if s2 != s]
                if other:
                    f2 = frames_by_sym[other[0]]
                    i2 = idx[other[0]].get(t)
                    dj2 = f2['di'][i2] if i2 is not None else None
                    if dj2 is None or f2['daily']['ema9'][dj2] is None or \
                       f2['daily']['c'][dj2] <= f2['daily']['ema9'][dj2]:
                        sig = None
            if sig is not None:
                # disaster floor: never risk more than 25% below entry-ish level
                sig['stop'] = max(sig['stop'], c * 0.75)
                if sig['stop'] < c:
                    pending[s] = sig
        # mark-to-market equity (open positions at last close) -- honest DD
        mtm = equity
        for s2, p2 in pos.items():
            i2 = idx[s2].get(t)
            if i2 is not None:
                mtm += (frames_by_sym[s2]['c'][i2] - p2['entry']) * p2['qty']
        eq_curve.append((t, mtm))
    return trades, eq_curve

# ---------------- metrics ----------------

def metrics(trades, eq_curve, flt=None, equity0=100_000.0):
    tr = [t for t in trades if flt is None or flt(t)]
    if not tr: return None
    wins = [t['pnl'] for t in tr if t['pnl'] > 0]; losses = [-t['pnl'] for t in tr if t['pnl'] <= 0]
    pf = (sum(wins) / sum(losses)) if losses and sum(losses) > 0 else float('inf')
    net = sum(t['pnl'] for t in tr)
    rets = [t['ret'] for t in tr]
    # maxDD on the full curve (only meaningful unfiltered)
    peak = -1e18; mdd = 0.0
    for _, e in eq_curve:
        peak = max(peak, e); mdd = min(mdd, (e-peak)/peak if peak > 0 else 0)
    return dict(n=len(tr), wr=round(100*len(wins)/len(tr), 1), pf=round(pf, 2),
                net=round(net, 0), avg_ret=round(100*sum(rets)/len(rets), 2),
                worst=round(100*min(rets), 1), best=round(100*max(rets), 1),
                mdd=round(100*mdd, 1), med_hold=sorted(t['hold_h'] for t in tr)[len(tr)//2])

def year_of(t):
    import datetime as dt
    return dt.datetime.fromtimestamp(t, dt.timezone.utc).year

def report(tag, trades, eq, P):
    m = metrics(trades, eq, equity0=P['equity0'])
    if m is None:
        print(f"{tag:44} n=0"); return
    reasons = defaultdict(int)
    for t in trades: reasons[t['reason']] += 1
    rs = ' '.join(f'{k}:{v}' for k, v in sorted(reasons.items()))
    print(f"{tag:44} n={m['n']:4} WR={m['wr']:5}% PF={m['pf']:5} net=${m['net']:>10,.0f} "
          f"avg={m['avg_ret']:>6}% worst={m['worst']:>6}% mDD={m['mdd']:>6}% hold={m['med_hold']}h  [{rs}]")
    for yr in sorted(set(year_of(t['t_in']) for t in trades)):
        ym = metrics(trades, eq, lambda t, y=yr: year_of(t['t_in']) == y)
        if ym:
            print(f"    {yr}: n={ym['n']:4} WR={ym['wr']:5}% PF={ym['pf']:5} net=${ym['net']:>9,.0f} worst={ym['worst']}%")

# ---------------- main ----------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='.')
    ap.add_argument('--phase', default='entries')
    a = ap.parse_args()

    frames = {}
    for sym in ('BTCUSD', 'ETHUSD'):
        fp = os.path.join(a.data, f'{sym}_1h.csv')
        rows = load_1h(fp)
        if not integrity(rows, sym):
            print(f'REJECTED {sym}'); sys.exit(1)
        frames[sym] = build_frames(rows)
    print()

    def run(tag, **kw):
        P = dict(DEF); P.update(kw)
        tr, eq = simulate(frames, P)
        report(tag, tr, eq, P)
        return tr, eq

    if a.phase == 'entries':
        # raw candidates, bear-gated, minimal protection (structural stop only)
        run('C1 bear (gb0.4 ts168)', candidate='C1', gb=0.4, time_stop=168)
        run('C2 bear (donchian exit)', candidate='C2')
        run('C3 bear (ema9 exit)', candidate='C3', exit_ema9=True)
        run('C4 bear (donchian exit)', candidate='C4')
        run('C4 bear (gb0.4 ts240)', candidate='C4', gb=0.4, time_stop=240)
    elif a.phase == 'c4sweep':
        for flush in (0.08, 0.12, 0.16, 0.20):
            for armd in (5, 10):
                for fast in (6, 12, 24):
                    run(f'C4 fl{flush} arm{armd} don{fast}',
                        candidate='C4', c4_flush=flush, c4_arm_days=armd, c4_don_fast=fast)
    elif a.phase == 'final':
        FIN = dict(candidate='C3', exit_ema9=True, subgate='both',
                   be_arm=0.02, be_lock=0.0, risk_pct=0.02)
        tr, eq = run('FINAL BearRecovery (BE2% risk2%)', **FIN)
        mid = 1640995200  # 2022-01-01: half the span, splits the two bear clusters
        for tag, flt in [('H1 2017-2021', lambda t: t['t_in'] < mid),
                         ('H2 2022-2026', lambda t: t['t_in'] >= mid),
                         ('BTC', lambda t: t['sym'] == 'BTCUSD'),
                         ('ETH', lambda t: t['sym'] == 'ETHUSD')]:
            m = metrics(tr, eq, flt)
            if m: print(f"  {tag:14} n={m['n']:3} WR={m['wr']}% PF={m['pf']} net=${m['net']:,.0f} worst={m['worst']}%")
        print('\n-- sensitivity neighbors --')
        for bm in (2, 4):
            run(f'below_min={bm}', **FIN, below_min=bm)
        for arm in (0.015, 0.03):
            run(f'BE arm {arm}', **{**FIN, 'be_arm': arm})
        print('\n-- trade list --')
        import datetime as dt2
        for t in tr:
            print(f"  {t['sym']} {dt2.datetime.fromtimestamp(t['t_in'],dt2.timezone.utc):%Y-%m-%d} -> "
                  f"{dt2.datetime.fromtimestamp(t['t_out'],dt2.timezone.utc):%Y-%m-%d} {t['ret']*100:+6.1f}% "
                  f"mfe={t['mfe']*100:5.1f}% {t['reason']}")
    elif a.phase == 'protect':
        CH = dict(candidate='C3', exit_ema9=True, subgate='both')
        run('CHAMP baseline', **CH)
        print('\n-- BE-and-ride floor sweep --')
        for arm in (0.02, 0.03, 0.05, 0.08):
            for lock in (0.0, 0.005, 0.01):
                run(f'BE arm{arm} lock{lock}', **CH, be_arm=arm, be_lock=lock)
        print('\n-- cold-cut v2 sweep (never-green catastrophe) --')
        for cut in (0.03, 0.05, 0.08, 0.12):
            run(f'COLDCUT {cut}', **CH, cold_cut=cut, cut_minhold=24)
        print('\n-- giveback clip sweep (expect: hurts) --')
        for gb in (0.3, 0.5, 0.7):
            run(f'GB {gb} gate3%', **CH, gb=gb, gate_pct=0.03)
        print('\n-- stop width (x ATR below entry-day low) --')
        for w in (0.25, 1.0, 2.0):
            run(f'stopATR {w}', **CH, c3_stop_atr=w)
        print('\n-- cost stress --')
        run('cost x2 (12bps/side, 20bps slip)', **CH, cost_bps=12.0, stop_slip_bps=20.0)
        run('cost x3 (18bps/side)', **CH, cost_bps=18.0, stop_slip_bps=30.0)
        print('\n-- sizing (aggression) --')
        for rp in (0.01, 0.02, 0.03, 0.05):
            run(f'risk {int(rp*100)}%', **CH, risk_pct=rp)
    elif a.phase == 'recovery':
        for sg in ('ma20', 'ma30', 'ma50'):
            run(f'C3 ema9-exit {sg}', candidate='C3', exit_ema9=True, subgate=sg)
        for sg in ('ma20', 'ma30'):
            run(f'C3 ema9 {sg} DUAL', candidate='C3', exit_ema9=True, subgate=sg, dual_confirm=True)
    elif a.phase == 'c7':
        run('C7 W-bottom ema9-exit', candidate='C7', exit_ema9=True)
        run('C7 W-bottom tp8 ts240', candidate='C7', tp=0.0, time_stop=240, gb=0.5, gate_pct=0.03)
        run('C7 W-bottom ema9 DUAL', candidate='C7', exit_ema9=True, dual_confirm=True)
        run('C3 ema9 DUAL', candidate='C3', exit_ema9=True, dual_confirm=True)
        run('C3 ema9 DUAL sub50up', candidate='C3', exit_ema9=True, dual_confirm=True, subgate='sma50up')
        run('C7 ema9 DUAL sub50up', candidate='C7', exit_ema9=True, dual_confirm=True, subgate='sma50up')
    elif a.phase == 'c6':
        for panic in (0.06, 0.08, 0.10, 0.12):
            for tp in (0.03, 0.05, 0.08):
                for tstop in (24, 48):
                    run(f'C6 p{int(panic*100)} tp{int(tp*100)} ts{tstop}',
                        candidate='C6', c6_panic=panic, tp=tp, time_stop=tstop)
    elif a.phase == 'subgate':
        for sg in ('sma50', 'sma50up', 'both'):
            run(f'C2 bear+{sg}', candidate='C2', subgate=sg)
            run(f'C3 bear+{sg}', candidate='C3', exit_ema9=True, subgate=sg)
            run(f'C5 bear+{sg} tp8 ts168', candidate='C5', tp=0.08, time_stop=168, subgate=sg)
    elif a.phase == 'c5':
        for tp in (0.05, 0.08, 0.10, 0.12):
            for tstop in (168, 240):
                run(f'C5 tp{int(tp*100)}% ts{tstop}h', candidate='C5', tp=tp, time_stop=tstop)
        run('C5 tp8 ts168 UNGATED', candidate='C5', tp=0.08, time_stop=168, regime='any')
        run('C5 tp8 ts168 BULL', candidate='C5', tp=0.08, time_stop=168, regime='bull')
    elif a.phase == 'c1sweep':
        for flush in (0.08, 0.12, 0.16):
            for nd in (3, 5):
                for gb in (0.3, 0.5):
                    for tstop in (120, 240):
                        run(f'C1 fl{flush} nd{nd} gb{gb} ts{tstop}',
                            candidate='C1', flush_pct=flush, flush_nd=nd, gb=gb, time_stop=tstop)

if __name__ == '__main__':
    main()
