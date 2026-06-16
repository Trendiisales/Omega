import csv,sys,collections
# tick value (USD per point per 1.0 lot) — from include/sizing.hpp
TV={"XAUUSD":100,"XAGUSD":5000,"US500.F":50,"USTEC.F":20,"DJ30.F":5,"NAS100":1,
    "GER40":1.10,"UK100":1.33,"ESTX50":1.10,"EURUSD":100000,"GBPUSD":100000,"EURGBP":100000,
    "AUDUSD":100000,"NZDUSD":100000,"USDJPY":667}
rows=[]
hdr=None
for ln in open("(sys.argv[1] if len(sys.argv)>1 else "/tmp/all_closes_raw.csv")"):
    p=ln.rstrip("\n").split(",")
    if p and p[0]=="trade_id": hdr=p; continue
    if hdr and len(p)==len(hdr): rows.append(dict(zip(hdr,p)))
print(f"# parsed {len(rows)} closed trades")
def f(x):
    try: return float(x)
    except: return 0.0
agg=collections.defaultdict(lambda:{"n":0,"net":0.0,"mfe_usd":0.0,"mae_usd":0.0,"wins":0,"caps":[]})
for r in rows:
    eng=r.get("engine","?"); sym=r.get("symbol","?"); tv=TV.get(sym,1)
    net=f(r.get("net_pnl")); mfe=f(r.get("mfe")); mae=f(r.get("mae")); sz=f(r.get("size")) or 1
    mfe_usd=abs(mfe)*sz*tv; mae_usd=abs(mae)*sz*tv
    a=agg[eng]; a["n"]+=1; a["net"]+=net; a["mfe_usd"]+=mfe_usd; a["mae_usd"]+=mae_usd
    if net>0: a["wins"]+=1
    if mfe_usd>1e-9: a["caps"].append(net/mfe_usd)
print(f"\n{'engine':<22}{'n':>4}{'net$':>9}{'sumMFE$':>10}{'capture':>9}{'WR%':>6}{'avgMAE$':>9}  flag")
print("-"*78)
for eng,a in sorted(agg.items(),key=lambda kv:-kv[1]["n"]):
    cap = a["net"]/a["mfe_usd"] if a["mfe_usd"]>1e-9 else 0   # aggregate capture = banked / favorable-excursion
    wr = 100*a["wins"]/a["n"] if a["n"] else 0
    flag = "LEAVING $ (trail/TP loose)" if (cap<0.35 and a["net"]>0) else ("NEGATIVE" if a["net"]<0 else "")
    print(f"{eng:<22}{a['n']:>4}{a['net']:>9.0f}{a['mfe_usd']:>10.0f}{cap:>9.2f}{wr:>6.0f}{a['mae_usd']/a['n']:>9.0f}  {flag}")
print("\ncapture = sum(net_pnl) / sum(MFE_usd). <0.35 on a net-positive engine = leaves >65% of the favorable move on the table -> trail/TP mis-set.")
