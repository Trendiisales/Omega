import luke_bt as L, sys
L.DATA='/tmp/luke_crypto'
u=L.load_universe()
for d in u.values(): L.add_avwap(d)
reg=L.load_regime(u, sma=200)
import collections
print('regime days',dict(collections.Counter(reg.values())),file=sys.stderr)
def run(P,tag):
    pp=dict(entry_modes=['A','C'],touch_buf=0.015,base_buf=0.06,stop_buf=0.003,avwap_band=0.02,
      cluster_band=0.02,cluster_min=2,min_stopw=0.005,max_stopw=0.06,risk_pct=0.01,max_pos_pct=0.35,
      max_concurrent=5,cost_bps=6.0,stop_slip_bps=10.0,partial_frac=0.15,partial_mult=3.0,partial_step=2.0,
      max_partials=3,be_after_partial=True,trail_ema=True,time_stop=0,regime_gate=False,equity0=100000.0,adr_min=3.0)
    pp.update(P)
    tr,eqc=L.simulate(u,reg,pp); 
    print(f'--- {tag} (gate={pp["regime_gate"]} adr={pp["adr_min"]} cost={pp["cost_bps"]}bps) ---')
    for k,f in [('ALL',None),('bull',lambda t:t['reg']=='bull'),('bear',lambda t:t['reg']=='bear'),
                ('CY2022',lambda t:str(t['date_in'])[:4]=='2022')]:
        m=L.metrics(tr,eqc,f)
        if m: print(f'  {k:7} n={m["n"]:4} PF={m["pf"]:5} WR={m["wr"]:5} avgR={m["avgR"]:6} net={m["net"]:10} DD={m["maxDD"]:5}')
    a=L.metrics(tr,eqc); print(f'  RETURN={a["ret"]}%  PF={a["pf"]}  DD={a["maxDD"]}%  n={a["n"]}')
run({'regime_gate':False},'CRYPTO champion NO-GATE')
run({'regime_gate':True},'CRYPTO champion + REGIME-GATE (BTC-breadth>SMA200)')
run({'regime_gate':True,'adr_min':4.0},'CRYPTO gated adr4')
