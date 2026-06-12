#!/usr/bin/env python3
"""
pump_smoothmode_bt -- redesign the pump engine to STOP the small-loss bleed.

Thesis (memory-backed): the current engine enters EVERY ignition + tight-trails 2%
-> chop names bleed small losses, only the rare CLEAN parabola pays (SLGB +834% =
most of gross; INHD +6018% but CHOPPY netted ~nothing). "Move size != profit;
smoothness decides." So:
  - ENTRY: only arm on a CLEAN, durable trend (trend-quality + R^2 smoothness gate),
           not any ignition. Skip the chop.
  - EXIT : MONSTER-MODE -- ride while structure holds (close>EMA9), wide catastrophe
           stop, EOD flat. NO tight 2% trail (it amputates the parabola).

Data: Yahoo 5m intraday (no IBKR gateway needed; covers the June basket). 5m is a
proxy for the live 3m -- tests the MECHANISM, not the exact fills.

Compares BASELINE (current: ignition + 2% trail + 15min cap) vs SMOOTH (redesign).
Cost: $1000 notional, 1%/2% per-side slip, commission -- same model as pump_recalib_bt.
"""
import urllib.request, json, sys, math, time, datetime as dt

UA = {"User-Agent": "Mozilla/5.0"}
COMMISSION_PER_SHARE = 0.005
COMMISSION_MIN = 1.00
NOTIONAL = 1000.0

BASKET = ("INHD:20260608 EPSM:20260608 YOUL:20260608 CCTG:20260608 RGNT:20260609 "
          "CHAI:20260609 SLGB:20260609 AZI:20260609 "
          "CIIT:20260610 HCAI:20260610 CHOW:20260610 HKIT:20260610 MSW:20260610 HWH:20260610 "
          "WCT:20260610 VSME:20260610")

def fetch_5m(sym):
    url = f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?interval=5m&range=60d"
    for attempt in range(5):
        try:
            req = urllib.request.Request(url, headers=UA)
            d = json.load(urllib.request.urlopen(req, timeout=20))
            r = d["chart"]["result"][0]
            ts = r.get("timestamp"); q = r["indicators"]["quote"][0]
            if not ts:
                time.sleep(2.0*(attempt+1)); continue          # throttled -> retry
            out = []
            for i, t in enumerate(ts):
                o,h,l,c,v = q["open"][i],q["high"][i],q["low"][i],q["close"][i],q["volume"][i]
                if None in (o,h,l,c): continue
                out.append((t,o,h,l,c,float(v or 0)))
            return out
        except Exception:
            time.sleep(2.0*(attempt+1))
    return []

def day_bars(bars, yyyymmdd):
    d = dt.datetime.strptime(yyyymmdd, "%Y%m%d").date()
    return [b for b in bars if dt.datetime.utcfromtimestamp(b[0]).date() == d]

def usd(entry, exit_px, slip_pct):
    if entry <= 0: return 0.0
    shares = NOTIONAL/entry
    gross = shares*(exit_px-entry)
    comm = 2*max(COMMISSION_MIN, COMMISSION_PER_SHARE*shares)
    slip = 2*(NOTIONAL*slip_pct/100.0)
    return gross - comm - slip

def r2(ys):
    n = len(ys)
    if n < 3: return 0.0
    sx=sy=sxx=sxy=syy=0.0
    for k,y in enumerate(ys):
        sx+=k; sy+=y; sxx+=k*k; sxy+=k*y; syy+=y*y
    den=(n*sxx-sx*sx)*(n*syy-sy*sy)
    if den<=0: return 0.0
    r=(n*sxy-sx*sy)/math.sqrt(den)
    return r*r

# ---- BASELINE: ignition entry + tight 2% trail + 15min cap (5min bars: cap=3) ----
def run_baseline(bars, gate=100.0, slip=2.0, trail=2.0, cap=3, hard=6.0):
    if not bars: return []
    day_open=bars[0][1]; run_high=0; cum_pv=cum_v=0; closes=[]; out=[]; pos=None
    for i,(t,o,h,l,c,v) in enumerate(bars):
        run_high=max(run_high,h); cum_pv+=(h+l+c)/3*v; cum_v+=v; closes.append(c)
        if pos:
            e,peak,ei=pos; peak=max(peak,h); held=i-ei
            stop=max(peak*(1-trail/100), e*(1-hard/100)); ex=None
            if l<=stop: ex=stop
            elif held>=cap: ex=c
            if ex is not None: out.append(usd(e,ex,slip)); pos=None
            else: pos=(e,peak,ei)
        if pos or day_open<=0: continue
        if (run_high/day_open-1)*100<gate: continue
        if len(closes)<8: continue
        vwap=cum_pv/cum_v if cum_v>0 else 0
        if not (vwap>0 and c>vwap): continue
        clb=closes[-4] if len(closes)>3 else None
        if not clb or clb<=0: continue
        if (c/clb-1)*100>=3.0 and (c>=l+0.6*(h-l) if h>l else True):
            pos=(c,c,i)
    if pos: out.append(usd(pos[0],bars[-1][4],slip))
    return out

# ---- SMOOTH = RIDE-ONCE: enter ONCE per name on a clean above-VWAP/EMA9 trend,
#      then RIDE (structure-hold exit), instead of re-entering 18x with a tight trail.
#      max_entries=1 stops the death-by-1000-cuts chop; monster-mode exit captures
#      the parabola. r2_min is a light smoothness filter (skip the violent chop).
def run_smooth(bars, gate=100.0, slip=2.0, ema_n=9, hard=0.15, confirm=2,
               r2_min=0.55, W=5, max_entries=1):
    if not bars: return []
    day_open=bars[0][1]; run_high=0; cum_pv=cum_v=0
    closes=[]; ema=[]; out=[]; pos=None; n_entries=0
    k=2.0/(ema_n+1)
    for i,(t,o,h,l,c,v) in enumerate(bars):
        run_high=max(run_high,h); cum_pv+=(h+l+c)/3*v; cum_v+=v; closes.append(c)
        ema.append(c if not ema else ema[-1]+k*(c-ema[-1]))
        vwap=cum_pv/cum_v if cum_v>0 else 0
        if pos:
            e,peak,ei,below=pos; peak=max(peak,h); held=i-ei; ex=None
            below = below+1 if c<ema[-1] else 0
            if l<=peak*(1-hard): ex=peak*(1-hard)          # wide catastrophe
            elif held>=1 and below>=confirm: ex=c          # structure break (EMA9) confirmed
            if ex is not None: out.append(usd(e,ex,slip)); pos=None
            else: pos=(e,peak,ei,below)
        if pos or day_open<=0: continue
        if n_entries>=max_entries: continue                # ENTER ONCE -- no re-entry chop
        if (run_high/day_open-1)*100<gate: continue
        if len(closes)<max(W+1, ema_n): continue
        if not (vwap>0 and c>vwap and c>ema[-1]): continue # above VWAP and EMA9
        if r2(closes[-W:])<r2_min: continue                # light smoothness (skip violent chop)
        clb=closes[-4] if len(closes)>3 else None
        if not clb or clb<=0: continue
        if (c/clb-1)*100<3.0: continue                     # ignition
        pos=(c,c,i,0); n_entries+=1
    if pos: out.append(usd(pos[0],bars[-1][4],slip))
    return out

def stat(t):
    n=len(t)
    if not n: return (0,0.0,0.0,0.0)
    gp=sum(x for x in t if x>0); gl=-sum(x for x in t if x<0)
    pf=gp/gl if gl>0 else 999.0
    wr=100*sum(1 for x in t if x>0)/n
    return (n,sum(t),pf,wr)

def main():
    toks=BASKET.split()
    data={}
    for tok in toks:
        sym,day=tok.split(":")
        db=day_bars(fetch_5m(sym),day)
        data[tok]=db
        print(f"# {tok}: {len(db)} 5m bars", file=sys.stderr)
        time.sleep(1.5)   # throttle Yahoo
    GATE=40.0   # relaxed from 100 so >1 name qualifies on this Yahoo-5m proxy basket
    print(f"\n(gate={GATE}% intraday-from-open; 5m proxy)")
    print(f"{'MODEL':10} {'slip':>4} {'N':>4} {'net$':>9} {'PF':>6} {'win%':>5}")
    for slip in (1.0,2.0):
        b=[]; s=[]
        for tok,bars in data.items():
            b+=run_baseline(bars,gate=GATE,slip=slip); s+=run_smooth(bars,gate=GATE,slip=slip)
        for name,t in (("BASELINE",b),("SMOOTH",s)):
            n,net,pf,wr=stat(t)
            print(f"{name:10} {slip:4.0f} {n:4d} {net:9.0f} {pf:6.2f} {wr:5.0f}")
    # per-name @ 2% slip + the day's actual move (high/open) so we see what's real
    print(f"\nper-name @2% slip   {'dayMove%':>8} {'BASELINE':>12} {'SMOOTH':>12}")
    for tok,bars in data.items():
        if not bars: print(f"  {tok:16} {'--':>8}"); continue
        mv=(max(b[2] for b in bars)/bars[0][1]-1)*100 if bars[0][1]>0 else 0
        nb=run_baseline(bars,gate=GATE,slip=2.0); ns=run_smooth(bars,gate=GATE,slip=2.0)
        print(f"  {tok:16} {mv:8.0f} {sum(nb):8.0f}({len(nb)}) {sum(ns):8.0f}({len(ns)})")

if __name__=="__main__":
    main()
