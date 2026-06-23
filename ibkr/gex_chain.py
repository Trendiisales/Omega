#!/usr/bin/env python3
# gex_chain.py -- dealer Gamma Exposure (GEX) profile from the IBKR option chain.
# Computes per-strike GEX, the net profile, the zero-gamma FLIP, and the call/put
# WALLS for an index (SPX/NDX/DAX). Standalone + validate-before-routing: emits a
# JSON snapshot; nothing here touches engine routing yet.
#
# GEX_strike ~= OI * gamma * 100 * spot^2 * 0.01, signed by the dealer convention
# (default: customers long calls/puts, dealers SHORT -> calls +gamma, puts -gamma).
# The sign convention is configurable (--dealer-sign) so we can A/B it -- the sign
# is the single biggest model risk (the analysis flagged exactly this).
#
# Per-index ONLY: SPX options for US500, NDX for NAS100, DAX (Eurex) for GER40.
# Never cross-apply. Recompute through the day (0DTE shifts intraday).
#
# usage: python ibkr/gex_chain.py --index SPX [--port 4001] [--expiries 2]
#        [--strikes-pct 5] [--dealer-sign classic|inverse] [--out gex_SPX.json]
import sys, os, json, argparse, datetime as dt
from ib_async import IB, Index, Option

# Per-index underlying definition (symbol, secType, exchange for the index + opt exch)
INDEX = {
    "SPX":  dict(symbol="SPX",  exch="CBOE",  opt_exch="SMART", cur="USD", mult="100"),  # -> US500
    "NDX":  dict(symbol="NDX",  exch="NASDAQ", opt_exch="SMART", cur="USD", mult="100"), # -> NAS100
    "DAX":  dict(symbol="DAX",  exch="EUREX", opt_exch="EUREX", cur="EUR", mult="1"),    # -> GER40
}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--index", default="SPX", choices=list(INDEX))
    ap.add_argument("--port", type=int, default=4001)
    ap.add_argument("--expiries", type=int, default=2)      # nearest N expiries
    ap.add_argument("--strikes-pct", type=float, default=5.0)  # NTM band +/- % of spot
    ap.add_argument("--dealer-sign", default="classic", choices=["classic", "inverse"])
    ap.add_argument("--out", default=None)
    ap.add_argument("--append", default=None, help="append a snapshot row to this history CSV (builds the validation dataset)")
    ap.add_argument("--ts", default="", help="UTC timestamp for the row (caller stamps; no clock here)")
    a = ap.parse_args()
    cfg = INDEX[a.index]
    sgn = 1.0 if a.dealer_sign == "classic" else -1.0  # classic: call +, put -

    ib = IB(); ib.connect("127.0.0.1", a.port, clientId=75, timeout=20)
    # GEX is a slow regime/levels input -> delayed (15-min) data is fine and avoids
    # needing the real-time CBOE/OPRA subs. 3=delayed, 4=delayed-frozen, 1=live.
    ib.reqMarketDataType(int(os.environ.get("GEX_MDTYPE", "3")))

    # 1. underlying spot
    und = Index(cfg["symbol"], cfg["exch"], cfg["cur"])
    ib.qualifyContracts(und)
    ut = ib.reqMktData(und, "", False, False); ib.sleep(2.5)
    spot = ut.last or ut.close or ut.marketPrice()
    print(f"[gex] {a.index} spot={spot}", flush=True)
    if not spot or spot != spot:
        print("[gex] no spot -- index data entitlement? aborting", flush=True); ib.disconnect(); return

    # 2. chain params -> expiries + strikes
    chains = ib.reqSecDefOptParams(und.symbol, "", und.secType, und.conId)
    chain = next((c for c in chains if c.exchange == cfg["opt_exch"]), chains[0] if chains else None)
    if not chain:
        print("[gex] no option chain params (entitlement?)", flush=True); ib.disconnect(); return
    expiries = sorted(chain.expirations)[: a.expiries]
    band = spot * a.strikes_pct / 100.0
    strikes = sorted(k for k in chain.strikes if abs(k - spot) <= band)
    print(f"[gex] expiries={expiries}  NTM strikes={len(strikes)} ({a.strikes_pct}% band)", flush=True)

    # 3. per-(expiry,strike,right) gamma + OI -> GEX
    rows = []
    for exp in expiries:
        for k in strikes:
            for right in ("C", "P"):
                opt = Option(cfg["symbol"], exp, k, right, cfg["opt_exch"], tradingClass=cfg["symbol"], multiplier=cfg["mult"], currency=cfg["cur"])
                try: ib.qualifyContracts(opt)
                except Exception: continue
                if not opt.conId: continue   # 2026-06-18: unqualified strike (e.g. NDX/DAX w/o
                                             # entitlement) -> conId stays empty -> reqMktData would
                                             # hash an unhashable contract and crash the whole run.
                                             # Skip gracefully so the index falls through to the
                                             # "no greeks/OI" abort instead of dying.
                t = ib.reqMktData(opt, "100,101", False, False); ib.sleep(0.6)
                g = t.modelGreeks.gamma if t.modelGreeks else None
                oi = (t.callOpenInterest if right == "C" else t.putOpenInterest)
                ib.cancelMktData(opt)
                if g is None or oi is None: continue
                gex = oi * g * 100.0 * spot * spot * 0.01 * (sgn if right == "C" else -sgn)
                rows.append(dict(exp=exp, strike=k, right=right, gamma=round(g, 6), oi=int(oi), gex=gex))

    if not rows:
        print("[gex] no greeks/OI returned -- likely NO index-OPTIONS (OPRA) data entitlement on this account.", flush=True)
        ib.disconnect(); return

    # 4. net profile per strike -> flip + walls
    per_strike = {}
    for r in rows: per_strike[r["strike"]] = per_strike.get(r["strike"], 0.0) + r["gex"]
    net = sum(per_strike.values())
    ks = sorted(per_strike)
    call_wall = max(ks, key=lambda k: per_strike[k])    # most positive-gamma strike
    put_wall  = min(ks, key=lambda k: per_strike[k])    # most negative-gamma strike
    # zero-gamma flip: strike where cumulative net GEX crosses zero
    cum = 0.0; flip = None; prev_k = None
    for k in ks:
        ncum = cum + per_strike[k]
        if prev_k is not None and (cum <= 0 < ncum or cum >= 0 > ncum): flip = k
        cum = ncum; prev_k = k

    out = dict(index=a.index, ts=None, spot=spot, dealer_sign=a.dealer_sign,
               net_gex=net, regime=("positive(mean-revert)" if net > 0 else "negative(momentum)"),
               flip=flip, call_wall=call_wall, put_wall=put_wall,
               per_strike={str(k): round(v, 1) for k, v in sorted(per_strike.items())})
    print(json.dumps({k: v for k, v in out.items() if k != "per_strike"}, indent=2), flush=True)
    print(f"[gex] CALL WALL {call_wall}  PUT WALL {put_wall}  FLIP {flip}  spot {spot}", flush=True)
    if a.append and net == 0:
        # 2026-06-24: skip dead rows. A closed session (weekend / Fri-evening / holiday)
        # returns no model-greeks on the delayed feed -> per-strike gamma all 0 -> net=0,
        # and the spot is the stale last-close. Appending these poisoned the predicate
        # study (net=0 defaults to "negative(momentum)" + frozen spot = fake low-vol
        # neg-gamma). No real GEX -> don't record it.
        print("[gex] net_gex==0 (no greeks; market likely closed) -> skip append", flush=True)
    elif a.append:
        newf = not os.path.exists(a.append) or os.path.getsize(a.append) == 0
        with open(a.append, "a") as fh:
            if newf: fh.write("ts_utc,index,spot,net_gex,regime,flip,call_wall,put_wall,dealer_sign\n")
            fh.write(f"{a.ts},{a.index},{spot},{net},{out['regime']},{flip},{call_wall},{put_wall},{a.dealer_sign}\n")
        print(f"[gex] appended snapshot -> {a.append}", flush=True)
    if a.out:
        out["ts"] = "STAMP"  # caller stamps real time (no Date.now in this context)
        open(a.out, "w").write(json.dumps(out, indent=2))
        print(f"[gex] wrote {a.out}", flush=True)
    ib.disconnect()

if __name__ == "__main__":
    main()
