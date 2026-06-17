#!/usr/bin/env python3
"""
ledger_analytics.py — omnibus per-engine + book diagnostics over the Omega trade
ledger. Everything computable from the existing omega_trade_closes schema; no new
data capture required. Built 2026-06-16 (operator: "sweep all").

Per-engine:  n, net$, WR, capture ratio (net/MFE), expectancy (avg R), payoff,
             MAE p50/p90 (-> stop placement), cost% of gross, $/hour held,
             max consec losses, regime split (RISK_ON/OFF), broker recon mismatch.
Book:        engine-return correlation (hidden concentration), equity-curve
             Sharpe/Sortino/Calmar/maxDD/Ulcer (per-trade-sequence proxy).
Flags the worst offenders so the review has a ranked to-do.

usage: ledger_analytics.py <ledger.csv> [--min-n 5]
"""
import csv,sys,math,collections,datetime,glob,os

TV={"XAUUSD":100,"XAGUSD":5000,"US500.F":50,"USTEC.F":20,"DJ30.F":5,"NAS100":1,
    "GER40":1.10,"UK100":1.33,"ESTX50":1.10,"EURUSD":100000,"GBPUSD":100000,
    "EURGBP":100000,"AUDUSD":100000,"NZDUSD":100000,"USDJPY":667,"EURUSD+GBPUSD":100000,"MGC":10}

def f(x):
    try: return float(x)
    except: return 0.0

def _resolve(path):
    """Path -> list of csv files. Glob/dir expands to all daily close files;
    excludes .bak / .cleared snapshots. Single file passes through."""
    if any(c in path for c in "*?["):
        files = sorted(glob.glob(path))
    elif os.path.isdir(path):
        files = sorted(glob.glob(os.path.join(path, "omega_trade_closes*.csv")))
    else:
        return [path]
    return [f for f in files if ".bak" not in f and "cleared" not in f]

def load(path):
    # 2026-06-17: glob + concatenate the DAILY close files (dedup by trade_id) so
    # we read the FULL history, not the cumulative file that resets on every deploy
    # (a reset 30-trade snapshot made a +$3900 book look negative -- never again).
    rows=[]; hdr=None; seen=set()
    for fp in _resolve(path):
        try: fh=open(fp)
        except OSError: continue
        for ln in fh:
            p=ln.rstrip("\n").split(",")
            if p and p[0]=="trade_id": hdr=p; continue
            if hdr and len(p)==len(hdr):
                if p[0] in seen: continue
                seen.add(p[0]); rows.append(dict(zip(hdr,p)))
        fh.close()
    return rows

def main():
    # default: glob ALL daily close files under the standard trades dir (cwd-relative
    # so `cd C:\Omega; python tools\analytics\ledger_analytics.py` reads full history).
    DEFLT = "logs/trades/omega_trade_closes*.csv"
    args=[a for a in sys.argv[1:] if not a.startswith("--")]
    path=args[0] if args else DEFLT
    min_n=int(sys.argv[sys.argv.index("--min-n")+1]) if "--min-n" in sys.argv else 5
    rows=load(path)
    print(f"# ledger: {path}  |  {len(rows)} closed trades  |  per-engine min-n for verdicts: {min_n}\n")

    # --- artifact filter (2026-06-16): the shadow ledger carries pre-fix lot=1.0
    # (100x on metals) trades + multi-day PHANTOM holds that poisoned BOTH these
    # analytics flags AND the 2026-06-15 "6mo shadow-book BT" cull batch (e.g.
    # GoldOrb's fake -$784 vs its real PF 2.38). Excluded unless --raw.
    # --since YYYY-MM-DD adds a date cutoff (e.g. post the 2026-06-09 lot fix). ---
    RAW = "--raw" in sys.argv
    SINCE = sys.argv[sys.argv.index("--since")+1] if "--since" in sys.argv else None
    EX = collections.Counter()

    E=collections.defaultdict(lambda:{"net":[],"gross":[],"cap":[],"R":[],"mae_usd":[],
        "cost":[],"hrs":[],"wins":0,"reg":collections.defaultdict(float),"recon":0,"seq":[]})
    for r in rows:
        eng=r.get("engine","?").strip('"'); sym=r.get("symbol","?").strip('"'); tv=TV.get(sym,1)
        if not RAW:
            if f(r.get("hold_sec")) > 7*86400: EX["phantom_hold>7d"]+=1; continue
            if sym in ("XAUUSD","XAGUSD") and (f(r.get("size")) or 0) > 0.05: EX["oversized_metal_lot"]+=1; continue
            if SINCE and r.get("exit_ts_utc","").strip('"')[:10] < SINCE: EX["pre_since"]+=1; continue
        net=f(r.get("net_pnl")); gross=f(r.get("gross_pnl")); sz=f(r.get("size")) or 1
        mfe=abs(f(r.get("mfe")))*sz*tv; mae=abs(f(r.get("mae")))*sz*tv
        entry=f(r.get("entry_px")); sl=f(r.get("sl")); risk=abs(entry-sl)*sz*tv
        cost=abs(f(r.get("commission")))+abs(f(r.get("slippage_entry")))+abs(f(r.get("slippage_exit")))
        hrs=f(r.get("hold_sec"))/3600.0; reg=r.get("regime","?").strip('"')
        bpnl=f(r.get("broker_pnl")); bfilled=r.get("broker_close_filled","").strip('"')
        a=E[eng]; a["net"].append(net); a["gross"].append(gross); a["seq"].append(net)
        if net>0: a["wins"]+=1
        if mfe>1e-9: a["cap"].append(net/mfe)
        if risk>1e-9: a["R"].append(net/risk)
        a["mae_usd"].append(mae); a["cost"].append(cost); a["hrs"].append(hrs)
        a["reg"][reg]+=net
        if bfilled in ("1","true","True") and abs(bpnl-net)>max(5.0,0.1*abs(net)): a["recon"]+=1

    if EX: print("# EXCLUDED artifacts: " + "  ".join(f"{k}={v}" for k,v in EX.items()) + "   (--raw to include)\n")

    def pct(v,p):
        if not v: return 0
        s=sorted(v); k=min(len(s)-1,int(p/100*len(s))); return s[k]
    def maxcl(seq):
        m=c=0
        for x in seq:
            c=c+1 if x<=0 else 0; m=max(m,c)
        return m

    print(f"{'engine':<20}{'n':>4}{'net$':>9}{'WR%':>5}{'capt':>6}{'expR':>6}{'pay':>5}{'MAEp90$':>9}{'cost%':>6}{'$/hr':>8}{'mxCL':>5}  flags")
    print("-"*108)
    flags_all=[]
    for eng,a in sorted(E.items(),key=lambda kv:-sum(kv[1]["net"])):
        n=len(a["net"]); net=sum(a["net"]); gross=sum(a["gross"]); wr=100*a["wins"]/n if n else 0
        cap = sum(a["cap"])/len(a["cap"]) if a["cap"] else 0   # avg per-trade capture ratio
        expR=sum(a["R"])/len(a["R"]) if a["R"] else 0
        wins=[x for x in a["net"] if x>0]; loss=[-x for x in a["net"] if x<0]
        payoff=(sum(wins)/len(wins))/(sum(loss)/len(loss)) if wins and loss else 0
        maep90=pct(a["mae_usd"],90); costpct=100*sum(a["cost"])/abs(gross) if abs(gross)>1e-9 else 0
        perhr=net/sum(a["hrs"]) if sum(a["hrs"])>1e-9 else 0; mcl=maxcl(a["seq"])
        fl=[]
        if n>=min_n:
            if net>0 and cap<0.35: fl.append("LOOSE-EXIT")
            if expR<0: fl.append("NEG-EXPECTANCY")
            if costpct>40: fl.append("COST-FRAGILE")
            if payoff and payoff<0.6 and wr<45: fl.append("BAD-PAYOFF")
            regvals=a["reg"];
            if len(regvals)>1:
                rmin=min(regvals.values())
                if rmin< -abs(0.5*net) and net>0: fl.append("REGIME-DEPENDENT")
        if a["recon"]>0: fl.append(f"BROKER-MISMATCHx{a['recon']}")
        if fl: flags_all.append((eng,n,net,fl))
        print(f"{eng:<20}{n:>4}{net:>9.0f}{wr:>5.0f}{cap:>6.2f}{expR:>6.2f}{payoff:>5.2f}{maep90:>9.0f}{costpct:>6.0f}{perhr:>8.1f}{mcl:>5}  {','.join(fl)}")

    # ---- book-level: engine-return correlation (hidden concentration) ----
    print("\n--- book: engine daily-PnL correlation (hidden concentration; |r|>0.6 = not diversified) ---")
    daily=collections.defaultdict(lambda:collections.defaultdict(float))
    for r in rows:
        eng=r.get("engine","?").strip('"'); d=r.get("exit_ts_utc","")[:10]; daily[eng][d]+=f(r.get("net_pnl"))
    engs=[e for e in daily if len(daily[e])>=3]
    if len(engs)>=2:
        days=sorted(set(d for e in engs for d in daily[e]))
        def series(e): return [daily[e].get(d,0.0) for d in days]
        def corr(a,b):
            n=len(a); ma=sum(a)/n; mb=sum(b)/n
            cov=sum((a[i]-ma)*(b[i]-mb) for i in range(n)); va=sum((x-ma)**2 for x in a); vb=sum((x-mb)**2 for x in b)
            return cov/math.sqrt(va*vb) if va>0 and vb>0 else 0
        hi=[]
        for i in range(len(engs)):
            for j in range(i+1,len(engs)):
                c=corr(series(engs[i]),series(engs[j]))
                if abs(c)>0.6: hi.append((engs[i],engs[j],c))
        if hi:
            for e1,e2,c in sorted(hi,key=lambda x:-abs(x[2]))[:10]: print(f"   {e1} ~ {e2}: r={c:+.2f}")
        else: print("   (no |r|>0.6 pairs, or insufficient overlapping days)")
    else: print("   (need >=2 engines with >=3 trade-days)")

    # ---- book equity-curve metrics (per-trade-sequence proxy) ----
    seq=[f(r.get("net_pnl")) for r in sorted(rows,key=lambda r:r.get("exit_ts_unix","0"))]
    if len(seq)>=10:
        mu=sum(seq)/len(seq); sd=math.sqrt(sum((x-mu)**2 for x in seq)/len(seq))
        dn=[x for x in seq if x<0]; dsd=math.sqrt(sum(x*x for x in dn)/len(dn)) if dn else 0
        eq=0;pk=0;mdd=0;ssum=0;uind=0
        for x in seq:
            eq+=x; pk=max(pk,eq); dd=eq-pk; mdd=min(mdd,dd); uind+=dd*dd
        sharpe=mu/sd*math.sqrt(len(seq)) if sd>0 else 0
        sortino=mu/dsd*math.sqrt(len(seq)) if dsd>0 else 0
        ulcer=math.sqrt(uind/len(seq))
        calmar=sum(seq)/abs(mdd) if mdd<0 else 0
        print(f"\n--- book equity (per-trade proxy, n={len(seq)}) ---")
        print(f"   net=${sum(seq):.0f}  Sharpe={sharpe:.2f}  Sortino={sortino:.2f}  Calmar={calmar:.2f}  maxDD=${mdd:.0f}  Ulcer=${ulcer:.0f}")
    else:
        print(f"\n--- book equity: need >=10 trades (have {len(seq)}) ---")

    if flags_all:
        print("\n*** RANKED FLAGS (act on these) ***")
        for eng,n,net,fl in sorted(flags_all,key=lambda x:-abs(x[2])):
            print(f"   {eng} (n={n}, net=${net:.0f}): {', '.join(fl)}")
    print("\n[capture<0.35=loose exit | expR<0=no edge | cost%>40=friction-eaten | regime-dependent=needs gate | broker-mismatch=fill/phantom issue]")

if __name__=="__main__": main()
