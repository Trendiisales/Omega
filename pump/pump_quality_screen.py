#!/usr/bin/env python3
"""
pump_quality_screen — test the operator's idea: can we drop the whipsaw penny
shells and trade only pump names with BETTER VALUE / BETTER HISTORY (higher
price, real liquidity, an established trading record)?

For each basket name, compute CAUSAL pre-pump quality from Yahoo DAILY bars
(everything as-of the day BEFORE the pump day):
  - prior_close     : price level the day before (penny <$3 vs real >$3/$5)
  - avg_$vol_20d    : avg daily dollar-volume over prior 20 sessions (liquidity)
  - history_days    : # daily bars on record (fresh shell vs established)
  - atr_pct_20d     : prior 20-day daily range/close avg (intrinsic choppiness)
Then run the BASELINE pump model on the pump-day 5m bars and tabulate quality
vs net$. Finally test a QUALITY GATE and compare gated vs full net/PF.
"""
import urllib.request, json, sys, time, datetime as dt, statistics as st

UA = {"User-Agent": "Mozilla/5.0"}
NOTIONAL = 1000.0
COMMISSION_MIN = 1.00; COMMISSION_PER_SHARE = 0.005

BASKET = ("INHD:20260608 EPSM:20260608 YOUL:20260608 CCTG:20260608 RGNT:20260609 "
          "CHAI:20260609 SLGB:20260609 AZI:20260609 "
          "CIIT:20260610 HCAI:20260610 CHOW:20260610 HKIT:20260610 MSW:20260610 HWH:20260610 "
          "WCT:20260610 VSME:20260610")

def yget(url):
    for a in range(5):
        try:
            d = json.load(urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=20))
            r = d["chart"]["result"][0]
            if not r.get("timestamp"): time.sleep(2*(a+1)); continue
            return r
        except Exception:
            time.sleep(2*(a+1))
    return None

def daily_quality(sym, yyyymmdd):
    r = yget(f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?interval=1d&range=3mo")
    if not r: return None
    ts = r["timestamp"]; q = r["indicators"]["quote"][0]
    pump_day = dt.datetime.strptime(yyyymmdd, "%Y%m%d").date()
    rows = []
    for i, t in enumerate(ts):
        d = dt.datetime.utcfromtimestamp(t).date()
        c = q["close"][i]; v = q["volume"][i]
        if c is None or v is None: continue
        if d < pump_day: rows.append((d, c, v, q["high"][i], q["low"][i]))
    if len(rows) < 3: return {"history_days": len(rows), "prior_close": None}
    prior_close = rows[-1][1]
    last20 = rows[-20:]
    avg_dvol = st.mean(c*v for _, c, v, _, _ in last20)
    atr_pct = st.mean((hi-lo)/c for _, c, _, hi, lo in last20 if c > 0) * 100
    return {"history_days": len(rows), "prior_close": prior_close,
            "avg_dvol": avg_dvol, "atr_pct": atr_pct}

# ---- baseline pump model on 5m (ignition + tight 2% trail + 15min cap) ----
def fetch_5m_day(sym, yyyymmdd):
    r = yget(f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?interval=5m&range=60d")
    if not r: return []
    ts = r["timestamp"]; q = r["indicators"]["quote"][0]
    day = dt.datetime.strptime(yyyymmdd, "%Y%m%d").date()
    out = []
    for i, t in enumerate(ts):
        if dt.datetime.utcfromtimestamp(t).date() != day: continue
        o,h,l,c,v = q["open"][i],q["high"][i],q["low"][i],q["close"][i],q["volume"][i]
        if None in (o,h,l,c): continue
        out.append((o,h,l,c,float(v or 0)))
    return out

def usd(e, x, slip=2.0):
    if e <= 0: return 0.0
    sh = NOTIONAL/e
    return sh*(x-e) - 2*max(COMMISSION_MIN, COMMISSION_PER_SHARE*sh) - 2*(NOTIONAL*slip/100.0)

def run_pump(bars, gate=40.0, trail=2.0, cap=3, hard=6.0):
    if not bars: return []
    day_open=bars[0][0]; rh=0; cpv=cv=0; closes=[]; out=[]; pos=None
    for i,(o,h,l,c,v) in enumerate(bars):
        rh=max(rh,h); cpv+=(h+l+c)/3*v; cv+=v; closes.append(c)
        if pos:
            e,peak,ei=pos; peak=max(peak,h); held=i-ei
            stop=max(peak*(1-trail/100), e*(1-hard/100)); ex=None
            if l<=stop: ex=stop
            elif held>=cap: ex=c
            if ex is not None: out.append(usd(e,ex)); pos=None
            else: pos=(e,peak,ei)
        if pos or day_open<=0: continue
        if (rh/day_open-1)*100<gate: continue
        if len(closes)<8: continue
        vwap=cpv/cv if cv>0 else 0
        if not (vwap>0 and c>vwap): continue
        clb=closes[-4] if len(closes)>3 else None
        if not clb or clb<=0: continue
        if (c/clb-1)*100>=3.0 and (c>=l+0.6*(h-l) if h>l else True): pos=(c,c,i)
    if pos: out.append(usd(pos[0],bars[-1][3]))
    return out

def main():
    print(f"{'name':16}{'prClose':>8}{'avg$vol':>11}{'histD':>6}{'atr%':>6} {'pumpNet$(n)':>12}  {'tier':>6}")
    rows=[]
    for tok in BASKET.split():
        sym,day=tok.split(":")
        ql=daily_quality(sym,day); time.sleep(0.8)
        bars=fetch_5m_day(sym,day); time.sleep(0.8)
        tr=run_pump(bars); net=sum(tr); n=len(tr)
        pc=ql.get("prior_close") if ql else None
        dv=ql.get("avg_dvol") if ql else None
        hd=ql.get("history_days") if ql else 0
        ap=ql.get("atr_pct") if ql else None
        # quality tier: established+priced+liquid vs penny shell
        tier="JUNK"
        if pc and dv and pc>=3.0 and dv>=5e6 and hd>=40: tier="QUALITY"
        elif pc and dv and pc>=1.0 and dv>=2e6 and hd>=20: tier="MID"
        rows.append((sym,pc,dv,hd,ap,net,n,tier))
        print(f"{tok:16}{(pc or 0):8.2f}{(dv or 0)/1e6:10.1f}M{hd:6d}{(ap or 0):6.0f} {net:8.0f}({n}) {tier:>9}")
    # ---- compare gated vs full ----
    def agg(sel):
        t=[r for r in rows if sel(r)]
        net=sum(r[5] for r in t); ntr=sum(r[6] for r in t)
        gw=sum(r[5] for r in t if r[5]>0); gl=-sum(r[5] for r in t if r[5]<0)
        pf=gw/gl if gl>0 else (999 if gw>0 else 0)
        return len(t),ntr,net,pf
    print()
    for label,sel in [("FULL basket",lambda r:True),
                      ("MID+QUALITY only (price>=1,$vol>=2M,hist>=20)",lambda r:r[7] in("MID","QUALITY")),
                      ("QUALITY only (price>=3,$vol>=5M,hist>=40)",lambda r:r[7]=="QUALITY"),
                      ("JUNK only (the pennies we'd drop)",lambda r:r[7]=="JUNK")]:
        names,ntr,net,pf=agg(sel)
        print(f"{label:48} names={names:2d} trades={ntr:3d} net=${net:7.0f} PF={pf:.2f}")

if __name__=="__main__":
    main()
