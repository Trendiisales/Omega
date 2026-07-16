#!/usr/bin/env python3
"""
StockDip MIMIC overlay backtest (S-2026-07-15).

Faithful replication of the GoldTrendMimicBook mechanism (include/GoldTrendMimicLadder.hpp)
triggered by StockDipTurtleEngine DIP entries (include/StockDipTurtleEngine.hpp).

MECHANISM (exact port of the two engines):
  StockDip DIP entry (per name, daily close): close>SMA200(prior 200 closes) AND
    RSI2(incl today, Cutler)<10 -> LONG at that day's close. (Exit logic of the
    UNDERLYING is irrelevant here -- the mimic is an INDEPENDENT book that only
    needs the ENTRY as a trigger.)
  Mimic leg (per StockDip entry): spawn an independent LONG leg at the signal-day
    close; manage on each SUBSEQUENT daily bar intrabar low->high->close (adverse
    first). arm at arm_pct MFE; post-arm trail keeps (1-gb)*peak (BE-FLOORED since
    peak>=arm_pct>0 -> armed leg can never book negative); pre-arm LOSS_CUT at
    -lc_pct; INDEPENDENT window-cap flush at today's close after cap_bars.
    Fills at the stop LEVEL (resting-stop convention, exactly as the engine books).
  Cost: 8bp RT debit per clip (the validated US-equity StockDip gate).

Judged STANDALONE (feedback-companion-independent-engine): the mimic's own book,
net of cost; NEVER compared to riding the stock or to StockDip's own return.

Data: backtest/data/bigcap_daily_ohlc/<SYM>.csv  (date,o,h,l,c ; Yahoo split+div
adjusted). Signal + trail both use this internally-consistent adjusted series.
"""
import os, sys, csv, math
from collections import defaultdict

OHLC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "bigcap_daily_ohlc")
NAMES = ["MU","NVDA","AVGO","DELL","CRDO","STX","INTC","AMD","AAPL","TPR","MSFT"]

COST_BP = 8.0            # RT cost debit (bp of entry)  -> 0.08% per clip
DIP_TREND_SMA = 200
DIP_RSI_LEN   = 2
DIP_RSI_IN    = 10.0

# ---------------------------------------------------------------- data
def load(sym):
    p = os.path.join(OHLC_DIR, f"{sym}.csv")
    ds, o, h, l, c = [], [], [], [], []
    with open(p) as f:
        r = csv.reader(f); next(r)
        for row in r:
            if len(row) < 5: continue
            ds.append(row[0])
            o.append(float(row[1])); h.append(float(row[2]))
            l.append(float(row[3])); c.append(float(row[4]))
    return ds, o, h, l, c

def integrity(sym, ds, o, h, l, c):
    """basic daily-OHLC sanity: h>=max(o,c), l<=min(o,c), no >60% adj day gap."""
    bad = 0
    for i in range(len(c)):
        if c[i] <= 0 or h[i] < max(o[i], c[i]) - 1e-6 or l[i] > min(o[i], c[i]) + 1e-6:
            bad += 1
        if i > 0 and c[i-1] > 0 and abs(c[i]/c[i-1]-1.0) > 0.60:
            bad += 1
    return bad

# ---------------------------------------------------------------- StockDip entry signal
def dip_entries(ds, c):
    """return list of (idx, date, entry_close) where a DIP LONG fires, faithful to
       StockDipTurtleSym: sma_prior_(200)=mean of 200 closes BEFORE today;
       rsi_incl_(2)=Cutler RSI over last 2 changes incl today; sig = c>sma200 & rsi<10."""
    ents = []
    N = len(c)
    for i in range(N):
        if i < DIP_TREND_SMA + 1:   # need 200 prior closes (+1 for a change)
            continue
        sma200 = sum(c[i-DIP_TREND_SMA:i]) / DIP_TREND_SMA        # 200 closes BEFORE today
        # Cutler RSI over last DIP_RSI_LEN changes including today
        g = ll = 0.0
        for k in range(i-DIP_RSI_LEN+1, i+1):
            ch = c[k]-c[k-1]
            if ch > 0: g += ch
            else: ll += -ch
        rsi = 100.0 if ll == 0 else 100.0 - 100.0/(1.0 + g/ll)
        if c[i] > sma200 and rsi < DIP_RSI_IN:
            ents.append((i, ds[i], c[i]))
    return ents

# ---------------------------------------------------------------- mimic leg simulate (exact on_h1_bar port)
def sim_leg(entry_idx, entry_px, o, h, l, c, arm_pct, gb, lc_pct, cap_bars):
    """Port of GoldTrendMimicBook.on_h1_bar (long, dir=+1), managed from entry_idx+1.
       Returns (ret_pct, reason, hold_bars, mfe_pct, mae_pct). Fills AT the stop level."""
    peak = 0.0; trough = 0.0; armed = False; bars = 0
    N = len(c)
    j = entry_idx + 1
    while j < N:
        bars += 1
        seq = (l[j], h[j], c[j])          # adverse (low) first for a long
        for p in seq:
            ret = (p/entry_px - 1.0) * 100.0
            if ret > peak:   peak = ret
            if ret < trough: trough = ret
            if not armed:
                if ret <= -lc_pct:
                    return (-lc_pct, "LOSS_CUT", bars, peak, trough)
                if peak >= arm_pct:
                    armed = True
            else:
                stop_ret = (1.0 - gb) * peak      # BE-floored (peak>=arm>0)
                if ret <= stop_ret:
                    return (stop_ret, "TRAIL_STOP", bars, peak, trough)
        if bars >= cap_bars:
            ret = (c[j]/entry_px - 1.0) * 100.0
            return (ret, "WINDOW_CAP", bars, peak, min(trough, ret))
        j += 1
    # ran off end of data -> flush at last close (open-leg mark)
    ret = (c[-1]/entry_px - 1.0) * 100.0
    return (ret, "EOD_FLUSH", bars, peak, min(trough, ret))

# ---------------------------------------------------------------- market regime proxy (equal-weight of the 11, vs its 200DMA)
def build_regime(data):
    """union of dates -> equal-weight normalized index of the 11 names; bull if idx>=SMA200(idx)."""
    # collect per-date close per name
    bydate = defaultdict(dict)
    for sym,(ds,o,h,l,c) in data.items():
        base = None
        for i,d in enumerate(ds):
            bydate[d][sym] = c[i]
    dates = sorted(bydate.keys())
    # equal-weight index: average of per-name (close/first_close)
    firsts = {}
    idx_series = []
    for d in dates:
        vals = []
        for sym in NAMES:
            if sym in bydate[d]:
                if sym not in firsts: firsts[sym] = bydate[d][sym]
                vals.append(bydate[d][sym]/firsts[sym])
        idx_series.append(sum(vals)/len(vals) if vals else float('nan'))
    regime = {}
    for i,d in enumerate(dates):
        if i < 200:
            regime[d] = True   # fail-open warmup
        else:
            sma = sum(idx_series[i-200:i])/200.0
            regime[d] = idx_series[i] >= sma
    return regime

# ---------------------------------------------------------------- run one variant across all names
def run_variant(data, entries_by_sym, regime, arm_pct, gb, lc_pct, cap_bars):
    clips = []   # each: dict(sym,date,ret,ret_real,reason,hold,mfe,mae,bull)
    for sym in NAMES:
        ds,o,h,l,c = data[sym]
        for (ei, ed, epx) in entries_by_sym[sym]:
            ret,reason,hold,mfe,mae = sim_leg(ei, epx, o,h,l,c, arm_pct, gb, lc_pct, cap_bars)
            ret_real = ret - COST_BP/100.0     # 8bp = 0.08%
            clips.append(dict(sym=sym, date=ed, ret=ret, ret_real=ret_real, reason=reason,
                              hold=hold, mfe=mfe, mae=mae, bull=regime.get(ed, True)))
    return clips

def stats(clips):
    n = len(clips)
    if n == 0: return dict(n=0)
    net = sum(x['ret_real'] for x in clips)
    gross_pos = sum(x['ret_real'] for x in clips if x['ret_real'] > 0)
    gross_neg = -sum(x['ret_real'] for x in clips if x['ret_real'] < 0)
    pf = (gross_pos/gross_neg) if gross_neg > 1e-9 else float('inf')
    wins = sum(1 for x in clips if x['ret_real'] > 1e-9)
    worst = min(x['ret_real'] for x in clips)
    best = max(x['ret_real'] for x in clips)
    avg = net/n
    # max drawdown of the banked cumulative curve (chronological)
    sc = sorted(clips, key=lambda x: x['date'])
    cum = 0.0; peak = 0.0; mdd = 0.0
    for x in sc:
        cum += x['ret_real']
        if cum > peak: peak = cum
        dd = cum - peak
        if dd < mdd: mdd = dd
    return dict(n=n, net=net, pf=pf, wr=100.0*wins/n, worst=worst, best=best, avg=avg, mdd=mdd)

def halves(clips):
    sc = sorted(clips, key=lambda x: x['date'])
    mid = len(sc)//2
    return sc[:mid], sc[mid:]

def regime_split(clips):
    bull = [x for x in clips if x['bull']]
    bear = [x for x in clips if not x['bull']]
    return bull, bear

def year_slice(clips, yr):
    return [x for x in clips if x['date'].startswith(yr)]

def fmt(s):
    if s.get('n',0)==0: return "n=0"
    pf = "inf" if s['pf']==float('inf') else f"{s['pf']:.2f}"
    return (f"n={s['n']:4d} net={s['net']:+8.1f}% avg={s['avg']:+6.2f}% PF={pf:>4} "
            f"WR={s['wr']:4.1f}% worst={s['worst']:+6.2f}% mdd={s['mdd']:+7.1f}%")

# ---------------------------------------------------------------- main
def main():
    data = {}
    print("=== DATA INTEGRITY ===")
    for sym in NAMES:
        ds,o,h,l,c = load(sym)
        bad = integrity(sym, ds,o,h,l,c)
        data[sym] = (ds,o,h,l,c)
        print(f"  {sym:5s} bars={len(c):5d} {ds[0]}..{ds[-1]} integrity_flags={bad}")
    regime = build_regime(data)

    entries_by_sym = {}
    tot_ent = 0
    print("\n=== StockDip DIP ENTRIES (reconstructed) ===")
    for sym in NAMES:
        ds,o,h,l,c = data[sym]
        ents = dip_entries(ds, c)
        entries_by_sym[sym] = ents
        tot_ent += len(ents)
        yrs = sorted(set(d[:4] for _,d,_ in ents))
        print(f"  {sym:5s} entries={len(ents):4d}  span {ents[0][1] if ents else '-'}..{ents[-1][1] if ents else '-'}")
    print(f"  TOTAL entries across 11 names = {tot_ent}")
    n_bear_ent = sum(1 for sym in NAMES for _,d,_ in entries_by_sym[sym] if not regime.get(d,True))
    print(f"  (of which fired in BEAR regime = {n_bear_ent})")

    # ------ param grid tune ------
    print("\n=== GRID TUNE (pooled all-11, net% of clip notional, net of 8bp) ===")
    grid = []
    for arm in (0.5,1.0,1.5,2.0,3.0,4.0):
        for gb in (0.3,0.4,0.5,0.6,0.7):
            for lc in (1.0,1.5,2.0,3.0):
                for cap in (10,):
                    clips = run_variant(data, entries_by_sym, regime, arm, gb, lc, cap)
                    s = stats(clips)
                    h1,h2 = halves(clips); s1=stats(h1); s2=stats(h2)
                    bl,br = regime_split(clips); sb=stats(bl); sr=stats(br)
                    both_halves = s1.get('net',0)>0 and s2.get('net',0)>0
                    both_reg = sb.get('net',0)>0 and (sr.get('n',0)==0 or sr.get('net',0)>0)
                    grid.append((s['net'], arm,gb,lc,cap, s, s1,s2, sb,sr, both_halves, both_reg))
    grid.sort(key=lambda x: -x[0])
    print(f"  {'arm':>4} {'gb':>4} {'lc':>4} cap | {'pooled':>44} | H1net H2net | bull bear | flags")
    for net,arm,gb,lc,cap,s,s1,s2,sb,sr,bh,brg in grid[:20]:
        flag = ("H+" if bh else "H-") + ("R+" if brg else "R-")
        print(f"  {arm:>4.1f} {gb:>4.1f} {lc:>4.1f} {cap:>3d} | {fmt(s)} | "
              f"{s1.get('net',0):+7.1f} {s2.get('net',0):+7.1f} | "
              f"{sb.get('net',0):+6.1f} {sr.get('net',0):+6.1f} | {flag}")

    # ------ finalist comparison across the task's param bands, ranked by a
    #        protection-aware score = net penalised by drawdown + 2022 bleed.
    print("\n=== FINALIST SCAN (task bands; protection-aware) ===")
    print(f"  {'arm':>4} {'gb':>4} {'lc':>4} | {'net':>8} {'PF':>5} {'mdd':>8} {'2022':>8} {'H1':>7} {'H2':>7} {'bear':>7} band")
    def scan(band, cond):
        rows=[]
        for arm,gb,lc in [(a,g,l) for a in (0.5,1.0,1.5,2.0,2.5,3.0,4.0)
                                   for g in (0.3,0.4,0.5,0.6,0.7) for l in (1.0,1.5,2.0,2.5,3.0)]:
            if not cond(arm,gb,lc): continue
            clips = run_variant(data, entries_by_sym, regime, arm, gb, lc, 10)
            s=stats(clips); h1,h2=halves(clips); s1=stats(h1); s2=stats(h2)
            bl,br=regime_split(clips); sr=stats(br); y22=stats(year_slice(clips,"2022"))
            rows.append((arm,gb,lc,s,s1,s2,sr,y22))
        # protection-aware: net + 1.5*mdd (mdd<0) + 2*2022net(if<0); require all-6
        def score(r):
            _,_,_,s,s1,s2,sr,y22=r
            pen = s['net'] + 1.2*s['mdd'] + 1.5*min(0.0,y22.get('net',0))
            ok = (s['net']>0 and s['pf']>=1.3 and s1['net']>0 and s2['net']>0 and
                  (sr.get('n',0)==0 or sr.get('net',0)>0))
            return (ok, pen)
        rows.sort(key=lambda r:(-int(score(r)[0]), -score(r)[1]))
        for arm,gb,lc,s,s1,s2,sr,y22 in rows[:6]:
            pf="inf" if s['pf']==float('inf') else f"{s['pf']:.2f}"
            print(f"  {arm:>4.1f} {gb:>4.1f} {lc:>4.1f} | {s['net']:>+8.0f} {pf:>5} {s['mdd']:>+8.0f} "
                  f"{y22.get('net',0):>+8.0f} {s1['net']:>+7.0f} {s2['net']:>+7.0f} {sr.get('net',0):>+7.0f} {band}")
        return rows[0]
    tf = scan("T", lambda a,gb,lc: 1.0<=a<=2.0 and gb<=0.5 and lc<=2.0)
    wf = scan("W", lambda a,gb,lc: 2.0<=a<=4.0 and gb>=0.5 and lc>=2.0)
    tight = (tf[3]['net'],tf[0],tf[1],tf[2],10,tf[3],tf[4],tf[5],None,tf[6],None,None)
    wide  = (wf[3]['net'],wf[0],wf[1],wf[2],10,wf[3],wf[4],wf[5],None,wf[6],None,None)

    for label,g in (("VARIANT T (Tight)",tight),("VARIANT W (Wide)",wide)):
        net,arm,gb,lc,cap,s,s1,s2,sb,sr,bh,brg = g
        clips = run_variant(data, entries_by_sym, regime, arm, gb, lc, cap)
        bl,br = regime_split(clips); sb=stats(bl); sr=stats(br)
        y22 = stats(year_slice(clips,"2022"))
        print(f"\n=== {label}: arm={arm} gb={gb} lc={lc} cap={cap} ===")
        print(f"  POOLED : {fmt(s)}")
        print(f"  half-1 : {fmt(s1)}")
        print(f"  half-2 : {fmt(s2)}")
        print(f"  BULL   : {fmt(sb)}")
        print(f"  BEAR   : {fmt(sr)}")
        print(f"  2022   : {fmt(y22)}")
        # reason breakdown
        rc = defaultdict(lambda: [0,0.0])
        for x in clips:
            rc[x['reason']][0]+=1; rc[x['reason']][1]+=x['ret_real']
        print("  exit-reason: " + " | ".join(f"{k}:{v[0]}({v[1]:+.0f}%)" for k,v in sorted(rc.items())))
        viable = (s['net']>0 and s['pf']>=1.3 and s1.get('net',0)>0 and s2.get('net',0)>0
                  and sb.get('net',0)>0 and (sr.get('n',0)==0 or sr.get('net',0)>0))
        print(f"  VERDICT: {'VIABLE' if viable else 'NOT VIABLE'} standalone "
              f"(net>0:{s['net']>0} PF>=1.3:{s['pf']>=1.3} H1+:{s1.get('net',0)>0} "
              f"H2+:{s2.get('net',0)>0} bull+:{sb.get('net',0)>0} bear+:{sr.get('net',0)>0 or sr.get('n',0)==0})")

if __name__ == "__main__":
    main()
