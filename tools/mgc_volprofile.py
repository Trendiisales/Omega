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
    "generated_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
    "candles": candles,
}
with open(OUT, "w") as f: json.dump(payload, f, indent=2)

# self-contained HTML viewer (data embedded -> works via file://, no server/CORS)
HTML = """<!doctype html><html><head><meta charset=utf-8>
<title>HTF Candle Volume Profile — MGC</title>
<style>body{background:#0b0e11;color:#cfd6e4;font:13px -apple-system,Segoe UI,sans-serif;margin:0}
#hd{padding:10px 16px;border-bottom:1px solid #1c2230}#hd b{color:#fff}
canvas{display:block}</style></head><body>
<div id=hd><b>HTF Candle Volume Profile</b> — __SYM__ (__CON__) · HTF __HTF__ · real COMEX volume · gen __GEN__
&nbsp;|&nbsp; <span style=color:#0fcf5d>bull</span> / <span style=color:#f85321>bear</span> · <span style=color:orange>POC</span> · VAH/VAL dashed · H/L dotted</div>
<canvas id=c></canvas>
<script>
const DATA=__PAYLOAD__;
const cv=document.getElementById('c'),x=cv.getContext('2d');
const W=Math.max(900,DATA.candles.length*260+80),H=620;cv.width=W;cv.height=H;
const cs=DATA.candles,allP=cs.flatMap(c=>[c.high,c.low]);
let pmin=Math.min(...allP),pmax=Math.max(...allP);const pad=(pmax-pmin)*0.06;pmin-=pad;pmax+=pad;
const top=40,bot=H-30,Y=p=>top+(pmax-p)/(pmax-pmin)*(bot-top);
x.fillStyle='#0b0e11';x.fillRect(0,0,W,H);
// price gridlines
x.strokeStyle='#161c28';x.fillStyle='#5b6678';x.font='11px sans-serif';
for(let i=0;i<=8;i++){const p=pmin+(pmax-pmin)*i/8,yy=Y(p);x.beginPath();x.moveTo(60,yy);x.lineTo(W,yy);x.stroke();x.fillText(p.toFixed(1),6,yy+3);}
const slotW=240,gap=20,profW=150;
cs.forEach((c,ci)=>{
 const x0=70+ci*(slotW+gap);                 // left of slot (profile grows right)
 const bodyX=x0+profW+30;                     // candle body x
 const col=c.bull?'#0fcf5d':'#f85321';
 const maxv=Math.max(...c.bins.map(b=>b.vol),1);
 const binH=(Y(c.low)-Y(c.high))/c.bins.length;
 // volume bins
 c.bins.forEach(b=>{const bw=Math.max(1,b.vol/maxv*profW),yy=Y(b.price);
   const isPoc=Math.abs(b.price-c.poc)<=(c.high-c.low)/c.bins.length/2;
   x.fillStyle=isPoc?'rgba(255,165,0,.85)':(c.bull?'rgba(15,207,93,.35)':'rgba(248,83,33,.35)');
   x.fillRect(x0,yy-binH/2,bw,Math.max(1,binH));});
 // H/L dotted
 x.setLineDash([2,3]);x.strokeStyle=col;x.beginPath();x.moveTo(x0,Y(c.high));x.lineTo(bodyX+12,Y(c.high));x.moveTo(x0,Y(c.low));x.lineTo(bodyX+12,Y(c.low));x.stroke();
 // VAH/VAL dashed
 x.setLineDash([6,4]);x.strokeStyle='#7da0ff';x.beginPath();x.moveTo(x0,Y(c.vah));x.lineTo(bodyX+12,Y(c.vah));x.moveTo(x0,Y(c.val));x.lineTo(bodyX+12,Y(c.val));x.stroke();
 // POC solid
 x.setLineDash([]);x.strokeStyle='orange';x.lineWidth=1.5;x.beginPath();x.moveTo(x0,Y(c.poc));x.lineTo(bodyX+12,Y(c.poc));x.stroke();x.lineWidth=1;
 // candle wick+body
 x.strokeStyle=col;x.beginPath();x.moveTo(bodyX,Y(c.high));x.lineTo(bodyX,Y(c.low));x.stroke();
 x.fillStyle=col;const yo=Y(c.open),yc=Y(c.close);x.fillRect(bodyX-7,Math.min(yo,yc),14,Math.max(2,Math.abs(yc-yo)));
 // labels
 x.fillStyle='#cfd6e4';x.font='11px sans-serif';x.textAlign='left';
 x.fillText(c.key,x0,top-22);
 x.fillStyle='orange';x.fillText('POC '+c.poc,bodyX+16,Y(c.poc)+3);
 x.fillStyle='#7da0ff';x.fillText('VAH '+c.vah,bodyX+16,Y(c.vah)+3);x.fillText('VAL '+c.val,bodyX+16,Y(c.val)+3);
 x.fillStyle=col;x.fillText('H '+c.high,bodyX+16,Y(c.high)+3);x.fillText('L '+c.low,bodyX+16,Y(c.low)+3);
 x.fillStyle='#5b6678';x.fillText('vol '+Math.round(c.total_vol),x0,bot+18);
});
</script></body></html>"""
html = (HTML.replace("__SYM__", payload["symbol"]).replace("__CON__", payload["contract"])
            .replace("__HTF__", str(HTF)).replace("__GEN__", payload["generated_utc"])
            .replace("__PAYLOAD__", json.dumps(payload)))
with open("data/mgc_volprofile.html", "w") as f: f.write(html)
print(f"wrote {OUT} + data/mgc_volprofile.html: {len(candles)} HTF candles")
for c_ in candles:
    print(f"  {c_['key']}  O{c_['open']} H{c_['high']} L{c_['low']} C{c_['close']}  "
          f"POC={c_['poc']} VAH={c_['vah']} VAL={c_['val']} vol={c_['total_vol']:.0f}")
