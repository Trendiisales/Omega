#!/usr/bin/env python3
# gex_gold.py -- dealer Gamma Exposure (GEX) profile for COMEX GOLD futures (GC).
#
# Build #2 from the 2026-06-17 "best gold traders" research deep-dive. GC options
# are the deepest pool of gold gamma; dealer hedging near large +gamma strikes
# pins price (mean-revert), near -gamma amplifies. This is the GOLD sibling of
# ibkr/gex_chain.py (which is index-only: SPX/NDX/DAX via Index+Option). GC needs
# a FUTURE + FUTURES OPTION (FOP) chain instead -- hence a separate script.
#
# STATUS / BLOCKER (be honest): there is NO historical GC option-chain data
# anywhere, and this needs a LIVE IBKR gateway with GC-OPTIONS entitlement
# (operator login; memory notes GC *tape* is not entitled -- options entitlement
# is UNCONFIRMED and must be checked: see the probe at the bottom). Until a
# snapshot history is collected (this script, scheduled daily), GEX CANNOT be
# backtested -- fabricating a chain would be exactly the BACKTEST_TRUTH disease.
# So: stand this up to COLLECT, then run a discrimination study (aurora-style:
# fwd-return of entries near vs far from a +gamma wall) BEFORE wiring anything.
#
# usage: python ibkr/gex_gold.py [--port 4001] [--expiries 2] [--strikes-pct 6]
#        [--dealer-sign classic|inverse] [--append logs/macro/gex_GC_history.csv]
#        [--out logs/macro/gex_GC.json] [--ts <utc-iso>]
# env: GEX_MDTYPE (3=delayed default, 1=live)
import sys, os, json, argparse
from ib_async import IB, Future, FuturesOption

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4001)
    ap.add_argument("--expiries", type=int, default=2)        # nearest N option expiries
    ap.add_argument("--strikes-pct", type=float, default=6.0) # NTM band +/- % of spot
    ap.add_argument("--dealer-sign", default="classic", choices=["classic", "inverse"])
    ap.add_argument("--append", default=None, help="append a snapshot row to this history CSV (builds the dataset)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--ts", default="STAMP", help="UTC timestamp for the row (caller stamps; no clock here)")
    a = ap.parse_args()
    sgn = 1.0 if a.dealer_sign == "classic" else -1.0   # classic: call +gamma, put -gamma

    ib = IB(); ib.connect("127.0.0.1", a.port, clientId=76, timeout=20)
    ib.reqMarketDataType(int(os.environ.get("GEX_MDTYPE", "3")))  # 3=delayed ok for a slow levels input

    # 1. front-month GC future (COMEX). Let IBKR resolve the nearest contract.
    fut = Future(symbol="GC", exchange="COMEX", currency="USD")
    cds = ib.reqContractDetails(fut)
    if not cds:
        print("[gex-gold] no GC future contract details (entitlement / symbol?). abort", flush=True); ib.disconnect(); return
    # nearest non-expired contract = smallest lastTradeDateOrContractMonth
    front = sorted((c.contract for c in cds), key=lambda c: c.lastTradeDateOrContractMonth)[0]
    ib.qualifyContracts(front)
    ut = ib.reqMktData(front, "", False, False); ib.sleep(2.5)
    spot = ut.last or ut.close or ut.marketPrice()
    print(f"[gex-gold] GC front={front.lastTradeDateOrContractMonth} spot={spot}", flush=True)
    if not spot or spot != spot:
        print("[gex-gold] no GC future price -- futures data entitlement? abort", flush=True); ib.disconnect(); return

    # 2. FOP chain params for the future
    chains = ib.reqSecDefOptParams(front.symbol, "COMEX", front.secType, front.conId)
    chain = chains[0] if chains else None
    if not chain:
        print("[gex-gold] no GC option-chain params -- NO GC-OPTIONS entitlement on this account.", flush=True)
        ib.disconnect(); return
    expiries = sorted(chain.expirations)[: a.expiries]
    band = spot * a.strikes_pct / 100.0
    strikes = sorted(k for k in chain.strikes if abs(k - spot) <= band)
    print(f"[gex-gold] expiries={expiries}  NTM strikes={len(strikes)} ({a.strikes_pct}% band)", flush=True)

    # 3. per-(expiry,strike,right) gamma + OI -> GEX  (same formula as gex_chain.py)
    per_strike = {}
    for exp in expiries:
        for k in strikes:
            for right in ("C", "P"):
                opt = FuturesOption(symbol="GC", lastTradeDateOrContractMonth=exp,
                                    strike=k, right=right, exchange="COMEX", currency="USD")
                try: ib.qualifyContracts(opt)
                except Exception: continue
                t = ib.reqMktData(opt, "100,101", False, False); ib.sleep(0.6)
                g = t.modelGreeks.gamma if t.modelGreeks else None
                oi = (t.callOpenInterest if right == "C" else t.putOpenInterest)
                ib.cancelMktData(opt)
                if g is None or oi is None: continue
                gex = oi * g * 100.0 * spot * spot * 0.01 * (sgn if right == "C" else -sgn)
                per_strike[k] = per_strike.get(k, 0.0) + gex

    if not per_strike:
        print("[gex-gold] no greeks/OI returned -- GC-options (FOP) data not entitled on this account.", flush=True)
        ib.disconnect(); return

    # 4. net profile -> regime + walls + zero-gamma flip
    net = sum(per_strike.values())
    ks = sorted(per_strike)
    call_wall = max(ks, key=lambda k: per_strike[k])
    put_wall  = min(ks, key=lambda k: per_strike[k])
    cum = 0.0; flip = None; prev = None
    for k in ks:
        ncum = cum + per_strike[k]
        if prev is not None and (cum <= 0 < ncum or cum >= 0 > ncum): flip = k
        cum = ncum; prev = k
    regime = "positive(mean-revert/pin)" if net > 0 else "negative(momentum/amplify)"

    out = dict(symbol="GC", ts=a.ts, spot=spot, dealer_sign=a.dealer_sign, net_gex=net,
               regime=regime, flip=flip, call_wall=call_wall, put_wall=put_wall,
               per_strike={str(k): round(v, 1) for k, v in sorted(per_strike.items())})
    print(json.dumps({k: v for k, v in out.items() if k != "per_strike"}, indent=2), flush=True)
    print(f"[gex-gold] CALL WALL {call_wall}  PUT WALL {put_wall}  FLIP {flip}  net {net:.3e}  spot {spot}", flush=True)
    if a.append:
        newf = not os.path.exists(a.append) or os.path.getsize(a.append) == 0
        os.makedirs(os.path.dirname(a.append) or ".", exist_ok=True)
        with open(a.append, "a") as fh:
            if newf: fh.write("ts_utc,symbol,spot,net_gex,regime,flip,call_wall,put_wall,dealer_sign\n")
            fh.write(f"{a.ts},GC,{spot},{net},{regime},{flip},{call_wall},{put_wall},{a.dealer_sign}\n")
        print(f"[gex-gold] appended snapshot -> {a.append}", flush=True)
    if a.out:
        open(a.out, "w").write(json.dumps(out, indent=2))
        print(f"[gex-gold] wrote {a.out}", flush=True)
    ib.disconnect()

if __name__ == "__main__":
    main()

# -----------------------------------------------------------------------------
# ENTITLEMENT PROBE (operator, on the LIVE gateway 4001) -- run BEFORE scheduling:
#   python -c "from ib_async import IB,Future; ib=IB(); ib.connect('127.0.0.1',4001,clientId=76); \
#     f=Future('GC',exchange='COMEX',currency='USD'); cds=ib.reqContractDetails(f); \
#     print('GC future details:',len(cds)); c=sorted((x.contract for x in cds),key=lambda c:c.lastTradeDateOrContractMonth)[0]; \
#     ib.qualifyContracts(c); ch=ib.reqSecDefOptParams(c.symbol,'COMEX',c.secType,c.conId); \
#     print('GC option-chain params:',len(ch)); ib.disconnect()"
# If "GC option-chain params: 0" => NO GC-options entitlement => add the CME/COMEX
# options market-data sub in IBKR before this can collect anything.
# -----------------------------------------------------------------------------
