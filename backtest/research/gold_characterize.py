#!/usr/bin/env python3
"""S39 gold edge rethink -- DESCRIPTIVE characterization only (no strategy sim).
Grounds edge hypotheses; any tradeable edge is validated later in C++ edge_pipeline.
Reads the 2yr XAUUSD bar files. Outputs an edge-oriented profile."""
import numpy as np, pandas as pd, sys
TICK = "/Users/jo/Tick"

def load_ohlc(path, has_header, datefmt=None):
    if has_header:
        df = pd.read_csv(path)
        df.columns = ["ts","o","h","l","c"][:len(df.columns)]
        df["dt"] = pd.to_datetime(df["ts"], unit="s", utc=True)
    else:
        df = pd.read_csv(path, header=None, names=["d","o","h","l","c"])
        df["dt"] = pd.to_datetime(df["d"].astype(str), format="%Y%m%d", utc=True)
    return df

d  = load_ohlc(f"{TICK}/2yr_XAUUSD_daily.csv", False)
h1 = load_ohlc(f"{TICK}/2yr_XAUUSD_tick_fresh.h1.csv", True)
h4 = load_ohlc(f"{TICK}/2yr_XAUUSD_tick_fresh.h4.csv", True)

def rets(df): return np.log(df["c"]/df["c"].shift(1)).dropna()

print("="*70); print("GOLD DATA CHARACTERIZATION (2yr XAUUSD, descriptive)"); print("="*70)
print(f"daily {len(d)} bars {d.dt.min().date()}..{d.dt.max().date()}  "
      f"price {d.c.iloc[0]:.0f}->{d.c.iloc[-1]:.0f} ({d.c.iloc[-1]/d.c.iloc[0]:.2f}x)")

# 1. RETURN AUTOCORRELATION by TF -- trend (AC>0) vs mean-revert (AC<0)
print("\n[1] RETURN AUTOCORRELATION (AC>0=>momentum/trend, AC<0=>mean-revert)")
for name, df in [("daily",d),("H4",h4),("H1",h1)]:
    r = rets(df); acs = [r.autocorr(k) for k in range(1,6)]
    print(f"  {name:5} lag1..5: " + " ".join(f"{a:+.3f}" for a in acs))

# 2. REGIME (markov) -- 20-bar rolling return sign -> Bull/Bear/Side, transition matrix
print("\n[2] DAILY REGIME STRUCTURE (20d rolling return label)")
d2 = d.copy(); d2["r20"] = d2["c"].pct_change(20)
thr = 0.02
d2["reg"] = np.where(d2.r20> thr,"Bull",np.where(d2.r20<-thr,"Bear","Side"))
d2 = d2.dropna(subset=["reg"])
counts = d2.reg.value_counts(normalize=True)
print("  occupancy: " + "  ".join(f"{k}={v:.0%}" for k,v in counts.items()))
# transition matrix + persistence
regs = d2.reg.values; states=["Bull","Side","Bear"]
T = pd.DataFrame(0.0,index=states,columns=states)
for a,b in zip(regs[:-1],regs[1:]): T.loc[a,b]+=1
T = T.div(T.sum(1),axis=0)
print("  transition matrix (row=from):")
for s in states: print(f"    {s}: " + " ".join(f"{c}={T.loc[s,c]:.2f}" for c in states))
print(f"  persistence (diag): Bull={T.loc['Bull','Bull']:.2f} Side={T.loc['Side','Side']:.2f} Bear={T.loc['Bear','Bear']:.2f}")
# fwd return conditional on regime
d2["fwd5"] = d2["c"].pct_change(5).shift(-5)
print("  fwd-5d mean return by regime: " + "  ".join(f"{s}={d2[d2.reg==s].fwd5.mean()*100:+.2f}%" for s in states))

# 3. VOLATILITY CLUSTERING + regime
print("\n[3] VOLATILITY CLUSTERING")
r = rets(d); absr = r.abs()
print(f"  |ret| autocorr lag1..5: " + " ".join(f"{absr.autocorr(k):+.3f}" for k in range(1,6)))
vol20 = r.rolling(20).std()
hi = vol20 > vol20.median()
fwd = r.shift(-1)
print(f"  next-day |ret| in HIGH-vol regime: {r.abs().shift(-1)[hi].mean()*100:.3f}%  LOW-vol: {r.abs().shift(-1)[~hi].mean()*100:.3f}%")

# 4. SESSION / HOUR-OF-DAY effects (H1)
print("\n[4] HOUR-OF-DAY (UTC) mean return + vol (H1 bars)")
h1b = h1.copy(); h1b["ret"]=np.log(h1b.c/h1b.o); h1b["hr"]=h1b.dt.dt.hour
g = h1b.groupby("hr")["ret"].agg(["mean","std","count"])
g["t"] = g["mean"]/(g["std"]/np.sqrt(g["count"]))
g = g.sort_values("t")
print("  strongest +hours (mean bps, t):")
for hr,row in g.tail(4).iloc[::-1].iterrows(): print(f"    {hr:02d}:00 UTC  {row['mean']*1e4:+.1f}bps  t={row['t']:+.1f}")
print("  strongest -hours:")
for hr,row in g.head(4).iterrows(): print(f"    {hr:02d}:00 UTC  {row['mean']*1e4:+.1f}bps  t={row['t']:+.1f}")

# 5. SESSION blocks (Asian 0-7, London 7-12, NY 12-21 UTC)
print("\n[5] SESSION RETURN + RANGE")
def sess(h): return "Asian" if h<7 else ("London" if h<12 else ("NY" if h<21 else "Late"))
h1b["sess"]=h1b.hr.map(sess); h1b["rng"]=(h1b.h-h1b.l)
for s in ["Asian","London","NY","Late"]:
    sub=h1b[h1b.sess==s]; print(f"  {s:7} mean={sub.ret.mean()*1e4:+.1f}bps  range=${sub.rng.mean():.2f}  n={len(sub)}")

# 6. DAY-OF-WEEK
print("\n[6] DAY-OF-WEEK (daily)")
d3=d.copy(); d3["ret"]=np.log(d3.c/d3.c.shift(1)); d3["dow"]=d3.dt.dt.dayofweek
for dow,nm in enumerate(["Mon","Tue","Wed","Thu","Fri","Sat","Sun"]):
    sub=d3[d3.dow==dow].ret.dropna()
    if len(sub)>5: print(f"  {nm} {sub.mean()*100:+.3f}%  t={sub.mean()/(sub.std()/np.sqrt(len(sub))):+.1f}  n={len(sub)}")

# 7. CONDITIONAL MOMENTUM: does H4 trend persist? (breakout follow-through)
print("\n[7] BREAKOUT FOLLOW-THROUGH (H4: after N-bar high, fwd ret)")
for N in [10,20,40]:
    hh = h4.c > h4.h.rolling(N).max().shift(1)
    fwd = np.log(h4.c.shift(-N)/h4.c)
    up = fwd[hh].mean(); base=fwd.mean()
    print(f"  close>{N}-bar-high -> fwd{N} ret {up*1e4:+.1f}bps (base {base*1e4:+.1f})  n={hh.sum()}")
print("\nDONE")
