#!/usr/bin/env python3
# 2026-05-29 walk-forward filter -- join IS + OOS, S43 gate on BOTH
# Outputs:
#   wf_survivors.csv  -- combos with both IS Sharpe>=0.5 net>0 n>=20 AND OOS Sharpe>=0.3 net>0 n>=10
#   wf_robust.csv     -- subset with sign-stable mean_r between IS and OOS
import csv, sys, os

base = 'outputs/discovery_2026-05-29'

def load(path):
    out = {}
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            key = (row['symbol'], row['timeframe'], row['family'], row['params'])
            out[key] = row
    return out

is_d  = load(f'{base}/leaderboard_is.csv')
oos_d = load(f'{base}/leaderboard_oos.csv')
full  = load(f'{base}/leaderboard_full.csv')

rows = []
for k, ir in is_d.items():
    or_ = oos_d.get(k)
    fr  = full.get(k)
    if not or_ or not fr: continue
    try:
        is_n  = int(ir['n_trades']);   oos_n  = int(or_['n_trades'])
        is_sh = float(ir['sharpe']);   oos_sh = float(or_['sharpe'])
        is_net= float(ir['net_pnl']);  oos_net= float(or_['net_pnl'])
        is_mr = float(ir['mean_r']);   oos_mr = float(or_['mean_r'])
        fu_sh = float(fr['sharpe']);   fu_net = float(fr['net_pnl']); fu_n = int(fr['n_trades'])
    except ValueError:
        continue
    # S43 gate IS: Sharpe>=0.5, net>0, n>=30
    if not (is_n >= 30 and is_sh >= 0.5 and is_net > 0): continue
    # OOS gate (relaxed for 30% data): Sharpe>=0.3, net>0, n>=10
    if not (oos_n >= 10 and oos_sh >= 0.3 and oos_net > 0): continue
    sign_stable = (is_mr * oos_mr) > 0  # both positive (or both negative — never here)
    rows.append((k, is_n, is_sh, is_net, is_mr, oos_n, oos_sh, oos_net, oos_mr, fu_n, fu_sh, fu_net, sign_stable))

# Sort by OOS Sharpe desc
rows.sort(key=lambda r: -r[6])

surv_path = f'{base}/wf_survivors.csv'
robust_path = f'{base}/wf_robust.csv'
with open(surv_path,'w') as f, open(robust_path,'w') as g:
    hdr = ('symbol,timeframe,family,params,is_n,is_sharpe,is_net,is_mr,'
           'oos_n,oos_sharpe,oos_net,oos_mr,full_n,full_sharpe,full_net,sign_stable\n')
    f.write(hdr); g.write(hdr)
    for k, isn,issh,isnet,ismr, on,osh,onet,omr, fn,fsh,fnet, stab in rows:
        line = f'{k[0]},{k[1]},{k[2]},"{k[3]}",{isn},{issh:.3f},{isnet:.2f},{ismr:.4f},{on},{osh:.3f},{onet:.2f},{omr:.4f},{fn},{fsh:.3f},{fnet:.2f},{1 if stab else 0}\n'
        f.write(line)
        if stab: g.write(line)

print(f'IS+OOS survivors: {len(rows)}')
print(f'  sign-stable (robust): {sum(1 for r in rows if r[-1])}')
print(f'output: {surv_path}, {robust_path}')
