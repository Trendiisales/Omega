#!/usr/bin/env python3
# Combined index-edge portfolio: FVGcont (intraday momentum) + Overnight drift +
# Connors RSI2 (daily mean-rev), on NDX cash 15m. Builds each strategy's DAILY
# P&L (% of notional), vol-scales each to a common target, combines, and reports
# per-strategy Sharpe + correlation matrix + combined vs buy&hold. Lets the data
# decide whether the weak legs help (diversify) or dilute.
import csv,collections,math
R=[x for x in csv.reader(open('NDX_15m.csv'))][1:]
def et_hm(s):
    import datetime as d; g=d.datetime.utcfromtimestamp(s); m=g.month
    dst=(3<m<11) or (m==3 and g.day>=8) or (m==11 and g.day<7)
    mm=(g.hour*60+g.minute+(-4 if dst else -5)*60)%1440; return (mm//60)*100+mm%60
bars=[(int(x[0]),float(x[1]),float(x[2]),float(x[3]),float(x[4])) for x in R]
byday=collections.OrderedDict()
for b in bars: byday.setdefault(b[0]//86400,[]).append(b)
days=sorted(byday)
# daily close/open (cash)
dclose={};dopen={}
for d in days:
    rth=[b for b in byday[d] if 930<=et_hm(b[0])<1600]
    if rth: dopen[d]=rth[0][1]; dclose[d]=rth[-1][4]
D=[d for d in days if d in dclose]
closes=[dclose[d] for d in D]
def sma(arr,i,n): return sum(arr[i-n:i])/n if i>=n else None
def rsi(arr,i,n):
    g=l=0
    for k in range(i-n+1,i+1):
        ch=arr[k]-arr[k-1]; g+=max(ch,0); l+=max(-ch,0)
    return 100 if l==0 else 100-100/(1+g/l)
N=len(D)
pnlA={};pnlB={};pnlC={}     # date-index -> daily return (% of notional)
# ---- B: OVERNIGHT (close[t-1]->open[t]) trend>SMA20 ----
for i in range(1,N):
    if i<20: continue
    if closes[i-1] < sum(closes[i-20:i])/20: continue
    pnlB[i]=dopen[D[i]]/dclose[D[i-1]]-1-0.0002
# ---- C: CONNORS RSI2<10 & close>SMA200, 1-day hold ----
for i in range(200,N-1):
    s=sma(closes,i+1,200)
    if s and closes[i]>s and rsi(closes,i,2)<10:
        pnlC[i+1]=closes[i+1]/closes[i]-1-0.0008
# ---- A: FVGcont (intraday 15m FVG->DOL, NY killzone, MACD gate) ----
def atr(bs,i,n=14):
    if i<1: return 0
    s=0;c=0
    for k in range(max(1,i-n+1),i+1):
        s+=max(bs[k][2]-bs[k][3],abs(bs[k][2]-bs[k-1][4]),abs(bs[k][3]-bs[k-1][4]));c+=1
    return s/c if c else 0
# build 15m series + MACD + PDH/PDL
H=bars  # already 15m
e12=e26=sig=0;mb=[]
for i,b in enumerate(H):
    c=b[4]
    if i==0:e12=e26=c;sg=c-c
    else:e12=c*2/13+e12*11/13;e26=c*2/27+e26*25/27
    macd=e12-e26; sig=macd*2/10+sig*8/10 if i else macd; mb.append(macd>sig)
pdh={};pdl={};cd=-1;hi=lo=0
dayhl={}
for b in H:
    d=b[0]//86400
    if d!=cd:
        if cd>=0: dayhl[cd]=(hi,lo)
        cd=d;hi=b[2];lo=b[3]
    else:hi=max(hi,b[2]);lo=min(lo,b[3])
dayhl[cd]=(hi,lo)
ddays=sorted(dayhl)
def prevhl(d):
    import bisect; j=bisect.bisect_left(ddays,d)-1
    return dayhl[ddays[j]] if j>=0 else (None,None)
fvgs=[];pos=None
for i in range(2,len(H)):
    a=atr(H,i)
    if a>0:
        if H[i-2][2]<H[i][3]:
            g=H[i][3]-H[i-2][2]
            if 0.05*a<=g<=6*a: fvgs.append([1,H[i-2][2],H[i][3],i,False])
        if H[i-2][3]>H[i][2]:
            g=H[i-2][3]-H[i][2]
            if 0.05*a<=g<=6*a: fvgs.append([-1,H[i][2],H[i-2][3],i,False])
    # manage
    if pos:
        b=H[i]; hit_sl=(b[3]<=pos['sl']) if pos['dir']>0 else (b[2]>=pos['sl'])
        hit_tp=(b[2]>=pos['tp']) if pos['dir']>0 else (b[3]<=pos['tp'])
        if hit_sl or hit_tp:
            ex=pos['sl'] if hit_sl else pos['tp']
            r=(ex-pos['e'])/pos['e'] if pos['dir']>0 else (pos['e']-ex)/pos['e']
            di=pos['di']; pnlA[di]=pnlA.get(di,0)+r-0.0002; pos=None
        continue
    hm=et_hm(H[i][0])
    if not(1330<=hm<1500): continue
    a=atr(H,i)
    if a<=0: continue
    px=H[i][4]; d=H[i][0]//86400
    ph,pl=prevhl(d)
    swhi=max(x[2] for x in H[max(0,i-48):i-1]) if i>4 else -1
    swlo=min(x[3] for x in H[max(0,i-48):i-1]) if i>4 else -1
    dolU=min([v for v in [ph,swhi] if v and v>px+0.3*a and (v-px)<=3*a] or [0]) or 0
    dolD=max([v for v in [pl,swlo] if v and 0<v<px-0.3*a and (px-v)<=3*a] or [0]) or 0
    for f in fvgs:
        if f[4]: continue
        if i-f[3]>8: continue
        if f[0]>0 and not dolU: continue
        if f[0]<0 and not dolD: continue
        if mb and ((f[0]>0 and not mb[i]) or (f[0]<0 and mb[i])): continue
        if not(H[i][3]<=f[2] and H[i][2]>=f[1]): continue
        e=f[2] if f[0]>0 else f[1]
        sl=(f[1]-0.01*a) if f[0]>0 else (f[2]+0.01*a)
        rr=(e-sl) if f[0]>0 else (sl-e)
        if rr<=0.05*a: f[4]=True; continue
        dol=dolU if f[0]>0 else dolD
        if (abs(dol-e)/rr)<1: continue
        pos={'dir':f[0],'e':e,'sl':sl,'tp':dol,'di':None}
        # find date index for exit-day attribution (use current day)
        try:
            import bisect; pos['di']=bisect.bisect_left(D,d)
        except: pos['di']=0
        f[4]=True; break
# ---- assemble daily return matrix ----
def series(pnl): return [pnl.get(i,0.0) for i in range(N)]
A=series(pnlA);B=series(pnlB);C=series(pnlC)
bh=[closes[i]/closes[i-1]-1 if i>0 else 0 for i in range(N)]
def sharpe(x):
    a=[v for v in x]; n=len(a); m=sum(a)/n; sd=(sum((v-m)**2 for v in a)/(n-1))**.5
    return m/sd*math.sqrt(252) if sd else 0
def dd(x):
    eq=1;pk=1;m=0
    for v in x: eq*=1+v;pk=max(pk,eq);m=max(m,(pk-eq)/pk)
    return 100*m
def ann(x):
    eq=1
    for v in x:eq*=1+v
    return 100*(eq**(252/len(x))-1)
def vol(x):
    m=sum(x)/len(x); return (sum((v-m)**2 for v in x)/(len(x)-1))**.5*math.sqrt(252)
# vol-scale each leg to 10% annual vol (risk parity), then equal-weight
TGT=0.10
def scale(x):
    v=vol(x); k=TGT/v if v>0 else 0; return [r*k for r in x]
As,Bs,Cs=scale(A),scale(B),scale(C)
combo=[(As[i]+Bs[i]+Cs[i])/3 for i in range(N)]
fvg_only=scale(A)
fvg_on=[(As[i]+Bs[i])/2 for i in range(N)]
def corr(x,y):
    mx=sum(x)/len(x);my=sum(y)/len(y)
    cov=sum((x[i]-mx)*(y[i]-my) for i in range(len(x)))/(len(x)-1)
    sx=(sum((v-mx)**2 for v in x)/(len(x)-1))**.5;sy=(sum((v-my)**2 for v in y)/(len(y)-1))**.5
    return cov/(sx*sy) if sx*sy else 0
print(f"period: {N} trading days (~{N/252:.1f}y)\n")
print(f"{'strategy':22} {'Sharpe':>7} {'annRet':>8} {'maxDD':>7} {'#days-active':>12}")
for nm,x in [('FVGcont (A)',A),('Overnight (B)',B),('Connors (C)',C),('buy&hold',bh)]:
    print(f"{nm:22} {sharpe(x):7.2f} {ann(x):7.1f}% {dd(x):6.0f}% {sum(1 for v in x if v!=0):12}")
print(f"\ncorrelations:  A-B={corr(A,B):+.2f}  A-C={corr(A,C):+.2f}  B-C={corr(B,C):+.2f}")
print(f"\n{'PORTFOLIO (vol-parity 10% each leg)':36} {'Sharpe':>7} {'annRet':>8} {'maxDD':>7}")
for nm,x in [('FVGcont only',fvg_only),('FVGcont+Overnight',fvg_on),('All 3 (A+B+C)',combo)]:
    print(f"{nm:36} {sharpe(x):7.2f} {ann(x):7.1f}% {dd(x):6.0f}%")
