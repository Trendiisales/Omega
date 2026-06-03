#!/usr/bin/env python3
# HTF Candle Volume Profile producer (MGC micro gold, COMEX real volume).
# Pulls TRADES bars from IB Gateway, groups them into the last N HTF candles,
# bins (close-weighted) volume per candle -> POC + value-area(70%) + H/L, and
# writes data/mgc_volprofile.json for the GUI to render. Converted from the
# ChartPrime "HTF Candle Volume Profile" Pine (volume profile is real here,
# unlike the FIX feed which has no volume).
#
# Run:  python tools/mgc_volprofile.py [port] [htf] [ncandles] [bins]
#   port default 4002 (IB Gateway paper), htf default D, ncandles 4, bins 24.
import sys, json, datetime as dt
from collections import defaultdict
from ib_async import IB, ContFuture

PORT     = int(sys.argv[1]) if len(sys.argv) > 1 else 4002
HTF      = sys.argv[2] if len(sys.argv) > 2 else "D"      # D=daily, or e.g. 240(min)
NCAND    = int(sys.argv[3]) if len(sys.argv) > 3 else 4
BINS     = int(sys.argv[4]) if len(sys.argv) > 4 else 24
OUT      = "data/mgc_volprofile.json"

# base bar size + duration: fine enough to bin within an HTF candle
BASE_BAR = "30 mins"
DUR      = "10 D" if HTF == "D" else "5 D"

def htf_key(d):
    # group key for an HTF candle from a bar datetime (UTC-ish from IB)
    if HTF == "D":
        return d.date().isoformat()
    mins = int(HTF)
    epoch_min = int(d.timestamp() // 60)
    return str((epoch_min // mins) * mins)

ib = IB()
ib.connect("127.0.0.1", PORT, clientId=88, timeout=15)
c = ContFuture("MGC", "COMEX", "USD"); ib.qualifyContracts(c)
bars = ib.reqHistoricalData(c, "", barSizeSetting=BASE_BAR, durationStr=DUR,
                            whatToShow="TRADES", useRTH=False)
# basis = MGC futures - XAU spot, so the viewer can shift levels onto the spot
# (XAUUSD) chart the engines actually trade. Best-effort; 0 if spot unavailable.
basis = 0.0; spot = None
try:
    from ib_async import Forex
    xau = Forex("XAUUSD"); ib.qualifyContracts(xau)
    t = ib.reqMktData(xau, "", True, False); ib.sleep(2.5)
    spot = t.last if (t.last and t.last == t.last) else (t.close if t.close else None)
    if spot and bars:
        basis = round(bars[-1].close - spot, 2)
except Exception as e:
    print(f"(spot fetch failed, basis=0: {e})")
ib.disconnect()

# group base bars into HTF candles
groups = defaultdict(list)
order = []
for b in bars:
    k = htf_key(b.date)
    if k not in groups: order.append(k)
    groups[k].append(b)

candles = []
for k in order[-NCAND:]:
    g = groups[k]
    o = g[0].open; cl = g[-1].close
    hi = max(x.high for x in g); lo = min(x.low for x in g)
    if hi <= lo: continue
    binsz = (hi - lo) / BINS
    vol = [0.0] * BINS
    for x in g:
        if (x.volume or 0) <= 0: continue
        idx = int((x.close - lo) / binsz); idx = max(0, min(idx, BINS - 1))
        vol[idx] += x.volume
    tot = sum(vol)
    poc_i = max(range(BINS), key=lambda i: vol[i]) if tot > 0 else 0
    poc = lo + binsz * (poc_i + 0.5)
    # value area = bins around POC accumulating to 70% of volume
    inc = {poc_i}; acc = vol[poc_i]; target = tot * 0.70
    lo_i = hi_i = poc_i
    while acc < target and (lo_i > 0 or hi_i < BINS - 1):
        below = vol[lo_i - 1] if lo_i > 0 else -1
        above = vol[hi_i + 1] if hi_i < BINS - 1 else -1
        if above >= below: hi_i += 1; inc.add(hi_i); acc += vol[hi_i]
        else:              lo_i -= 1; inc.add(lo_i); acc += vol[lo_i]
    vah = lo + binsz * (max(inc) + 1); val = lo + binsz * min(inc)
    candles.append({
        "key": k, "open": o, "high": hi, "low": lo, "close": cl,
        "bull": cl >= o, "poc": round(poc, 2),
        "vah": round(vah, 2), "val": round(val, 2), "total_vol": round(tot, 0),
        "bins": [{"price": round(lo + binsz * (i + 0.5), 2), "vol": round(vol[i], 0)} for i in range(BINS)],
    })

payload = {
    "symbol": "MGC", "contract": c.localSymbol, "htf": HTF, "bins": BINS,
    "basis": basis, "spot": spot,   # futures-spot basis; subtract to map to XAUUSD
    "generated_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
    "candles": candles,
}
with open(OUT, "w") as f: json.dump(payload, f, indent=2)

# Compact HVN file the live MgcFastDonchian30m engine reads: prior COMPLETED day
# POC + HVN (bins >= 60% of max vol). candles[-1] is today (forming) -> use [-2].
if len(candles) >= 2:
    pc = candles[-2]
    mx = max((bn["vol"] for bn in pc["bins"]), default=0.0)
    hvn = [bn["price"] for bn in pc["bins"] if mx > 0 and bn["vol"] >= 0.60 * mx]
    with open("data/mgc_hvn.json", "w") as f:
        json.dump({"day": pc["key"], "poc": pc["poc"], "hvn": hvn,
                   "basis": basis, "generated_utc": payload["generated_utc"]}, f)
    print(f"wrote data/mgc_hvn.json: prior-day {pc['key']} POC={pc['poc']} HVN={len(hvn)} levels basis={basis}")

# self-contained HTML viewer (data embedded -> works via file://, no server/CORS).
# ASCII-only; prices shifted by basis so levels map onto the spot XAUUSD chart.
HTML = """<!doctype html><html><head><meta charset="utf-8">
<title>HTF Candle Volume Profile - MGC</title>
<style>body{background:#0b0e11;color:#cfd6e4;font:13px -apple-system,Segoe UI,sans-serif;margin:0}
#hd{padding:10px 16px;border-bottom:1px solid #1c2230}#hd b{color:#fff}
canvas{display:block}</style></head><body>
<div id=hd><b>HTF Candle Volume Profile</b> - __SYM__ (__CON__) | HTF __HTF__ | real COMEX volume | basis __BASIS__ (prices shown spot-adjusted) | gen __GEN__
&nbsp;|&nbsp; <span style="color:#0fcf5d">bull</span> / <span style="color:#f85321">bear</span> | <span style="color:orange">POC</span> | VAH/VAL dashed | H/L dotted</div>
<canvas id=c></canvas>
<script>
try{
const DATA=__PAYLOAD__;
const B=DATA.basis||0, D=p=>p-B;            // spot-adjust
const cv=document.getElementById('c'),x=cv.getContext('2d');
const cs=DATA.candles;
const W=Math.max(900,cs.length*260+90),H=620;cv.width=W;cv.height=H;
const allP=[];cs.forEach(c=>{allP.push(D(c.high),D(c.low));});
let pmin=Math.min.apply(null,allP),pmax=Math.max.apply(null,allP);
if(pmax<=pmin)pmax=pmin+1;const pad=(pmax-pmin)*0.06;pmin-=pad;pmax+=pad;
const top=40,bot=H-30,Y=p=>top+(pmax-p)/(pmax-pmin)*(bot-top);
x.fillStyle='#0b0e11';x.fillRect(0,0,W,H);
x.strokeStyle='#161c28';x.fillStyle='#5b6678';x.font='11px sans-serif';x.textAlign='left';
for(let i=0;i<=8;i++){const p=pmin+(pmax-pmin)*i/8,yy=Y(p);x.beginPath();x.moveTo(60,yy);x.lineTo(W,yy);x.stroke();x.fillText(p.toFixed(1),6,yy+3);}
const slotW=240,gap=20,profW=150;
cs.forEach((c,ci)=>{
 const x0=70+ci*(slotW+gap), bodyX=x0+profW+30;
 const col=c.bull?'#0fcf5d':'#f85321';
 let maxv=1;c.bins.forEach(b=>{if(b.vol>maxv)maxv=b.vol;});
 const binH=Math.max(1,(Y(D(c.low))-Y(D(c.high)))/c.bins.length);
 c.bins.forEach(b=>{const bw=Math.max(1,b.vol/maxv*profW),yy=Y(D(b.price));
   const isPoc=Math.abs(b.price-c.poc)<=(c.high-c.low)/c.bins.length/2;
   x.fillStyle=isPoc?'rgba(255,165,0,.85)':(c.bull?'rgba(15,207,93,.35)':'rgba(248,83,33,.35)');
   x.fillRect(x0,yy-binH/2,bw,binH);});
 x.setLineDash([2,3]);x.strokeStyle=col;x.beginPath();x.moveTo(x0,Y(D(c.high)));x.lineTo(bodyX+12,Y(D(c.high)));x.moveTo(x0,Y(D(c.low)));x.lineTo(bodyX+12,Y(D(c.low)));x.stroke();
 x.setLineDash([6,4]);x.strokeStyle='#7da0ff';x.beginPath();x.moveTo(x0,Y(D(c.vah)));x.lineTo(bodyX+12,Y(D(c.vah)));x.moveTo(x0,Y(D(c.val)));x.lineTo(bodyX+12,Y(D(c.val)));x.stroke();
 x.setLineDash([]);x.strokeStyle='orange';x.lineWidth=1.5;x.beginPath();x.moveTo(x0,Y(D(c.poc)));x.lineTo(bodyX+12,Y(D(c.poc)));x.stroke();x.lineWidth=1;
 x.strokeStyle=col;x.beginPath();x.moveTo(bodyX,Y(D(c.high)));x.lineTo(bodyX,Y(D(c.low)));x.stroke();
 x.fillStyle=col;const yo=Y(D(c.open)),yc=Y(D(c.close));x.fillRect(bodyX-7,Math.min(yo,yc),14,Math.max(2,Math.abs(yc-yo)));
 x.fillStyle='#cfd6e4';x.font='11px sans-serif';x.fillText(c.key,x0,top-22);
 x.fillStyle='orange';x.fillText('POC '+D(c.poc).toFixed(1),bodyX+16,Y(D(c.poc))+3);
 x.fillStyle='#7da0ff';x.fillText('VAH '+D(c.vah).toFixed(1),bodyX+16,Y(D(c.vah))+3);x.fillText('VAL '+D(c.val).toFixed(1),bodyX+16,Y(D(c.val))+3);
 x.fillStyle=col;x.fillText('H '+D(c.high).toFixed(1),bodyX+16,Y(D(c.high))+3);x.fillText('L '+D(c.low).toFixed(1),bodyX+16,Y(D(c.low))+3);
 x.fillStyle='#5b6678';x.fillText('vol '+Math.round(c.total_vol),x0,bot+18);
});
}catch(e){document.body.insertAdjacentHTML('beforeend','<pre style="color:#f85321;padding:16px">RENDER ERROR: '+e+'\\n'+(e.stack||'')+'</pre>');}
</script></body></html>"""
html = (HTML.replace("__SYM__", payload["symbol"]).replace("__CON__", payload["contract"])
            .replace("__HTF__", str(HTF)).replace("__BASIS__", str(basis))
            .replace("__GEN__", payload["generated_utc"])
            .replace("__PAYLOAD__", json.dumps(payload)))
with open("data/mgc_volprofile.html", "w", encoding="utf-8") as f: f.write(html)
print(f"wrote {OUT} + data/mgc_volprofile.html: {len(candles)} HTF candles")
for c_ in candles:
    print(f"  {c_['key']}  O{c_['open']} H{c_['high']} L{c_['low']} C{c_['close']}  "
          f"POC={c_['poc']} VAH={c_['vah']} VAL={c_['val']} vol={c_['total_vol']:.0f}")
