#!/usr/bin/env python3
# =============================================================================
# ibkr_crypto_probe.py -- exhaustively probe what crypto IBKR will ACTUALLY let
# this account trade, and DECODE the "approved but can't trade" contradiction.
# (S-2026-07-02, operator: "I got approval from IBKR then it tells me I cannot
#  trade it -- which is it?  probe every option and lever.")
#
# WHY THIS EXISTS
#   "Trading permission granted" (the account-mgmt checkbox) and "this specific
#   CONTRACT is executable right now" are DIFFERENT gates. A contract can be
#   permissioned yet still refuse an order for any of: no market-data
#   subscription, wrong product-type entitlement (CRYPTO vs FUT vs the paper
#   account not mirroring the live permission), exchange not enabled for your
#   region, or the paper account simply not carrying the live grant. This tool
#   pokes EACH gate independently and prints which one is red.
#
# RUN ON THE VPS (or Mac) where the IB Gateway is reachable -- NOT from the
# cloud sandbox (no route to 127.0.0.1:4002 there):
#     python3 tools/ibkr_crypto_probe.py --port 4002          # paper
#     python3 tools/ibkr_crypto_probe.py --port 4001          # live (READ-ONLY: whatIf, no real orders)
#     python3 tools/ibkr_crypto_probe.py --port 4002 --live-check   # also try a whatIf order on each
#
# It NEVER sends a real order. Order viability is tested with whatIf=True
# (margin-preview) which returns the SAME reject codes as a live order without
# transmitting. Safe on live and paper.
# =============================================================================
import argparse, sys, asyncio

# Python 3.12+ / 3.14 removed the implicit main-thread event loop that ib_insync
# grabs at import via asyncio.get_event_loop(). Create one first so the import
# succeeds. (ib_async, the maintained fork, is preferred and needs this too on 3.14.)
try:
    asyncio.get_event_loop()
except RuntimeError:
    asyncio.set_event_loop(asyncio.new_event_loop())

# Prefer ib_async (maintained, supports modern Python); fall back to ib_insync.
try:
    from ib_async import IB, Crypto, Stock, ContFuture, MarketOrder
except Exception:
    try:
        from ib_insync import IB, Crypto, Stock, ContFuture, MarketOrder
    except Exception as e:
        sys.exit("Neither ib_async nor ib_insync importable (%s).\n"
                 "  On Python 3.12+/3.14:  pip3 install ib_async\n"
                 "  Or run this on the VPS venv where the pump scripts already work." % e)

# ---- the full candidate matrix: every way IBKR can quote/trade crypto --------
# (product, symbol, exchange, currency, note)  -- ContFuture used for the futures
# so we don't hardcode an expiry; the probe resolves the front month.
CANDIDATES = [
    # --- CME crypto futures (regulated; the cheap "ladder" venue you measured 2-8bps) ---
    ("FUT-CONT", "MBT",  "CME",      "USD", "Micro Bitcoin future (0.1 BTC)"),
    ("FUT-CONT", "MET",  "CME",      "USD", "Micro Ether future (0.1 ETH)"),
    ("FUT-CONT", "BRR",  "CME",      "USD", "Bitcoin future (5 BTC, big)"),
    ("FUT-CONT", "ETH",  "CME",      "USD", "Ether future (big)"),
    ("FUT-CONT", "MSOL", "CME",      "USD", "Micro Solana future (if listed)"),
    ("FUT-CONT", "SOL",  "CME",      "USD", "Solana future (if listed)"),
    ("FUT-CONT", "XRP",  "CME",      "USD", "XRP future (if listed)"),
    # --- IBKR spot-quoted futures (SQF; you measured 22-28bps -> slow book only) ---
    #     these resolve as CRYPTO on PAXOS in some accounts; probed below too.
    # --- Native IBKR crypto (spot via Paxos) -- the "CRYPTO" secType ---
    ("CRYPTO",   "BTC",  "PAXOS",    "USD", "Spot BTC (Paxos)"),
    ("CRYPTO",   "ETH",  "PAXOS",    "USD", "Spot ETH (Paxos)"),
    ("CRYPTO",   "SOL",  "PAXOS",    "USD", "Spot SOL (Paxos, if listed)"),
    ("CRYPTO",   "LTC",  "PAXOS",    "USD", "Spot LTC (Paxos)"),
    ("CRYPTO",   "BCH",  "PAXOS",    "USD", "Spot BCH (Paxos)"),
    # --- Spot-crypto ETFs (ordinary shares: pennies, shortable, no expiry/financing on long) ---
    ("STK",      "IBIT", "SMART",    "USD", "iShares spot-BTC ETF (shortable)"),
    ("STK",      "FBTC", "SMART",    "USD", "Fidelity spot-BTC ETF"),
    ("STK",      "ETHA", "SMART",    "USD", "iShares spot-ETH ETF"),
    ("STK",      "BITO", "SMART",    "USD", "BTC futures ETF"),
]

def mk_contract(prod, sym, exch, cur):
    if prod == "FUT-CONT": return ContFuture(sym, exch, cur)
    if prod == "CRYPTO":   return Crypto(sym, exch, cur)
    if prod == "STK":      return Stock(sym, exch, cur)
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4002, help="4002 paper / 4001 live")
    ap.add_argument("--client-id", type=int, default=97, help="probe clientId (unused elsewhere)")
    ap.add_argument("--live-check", action="store_true",
                    help="also run a whatIf() margin-preview order on each viable contract")
    a = ap.parse_args()

    ib = IB()
    try:
        ib.connect(a.host, a.port, clientId=a.client_id, timeout=20)
    except Exception as e:
        sys.exit(f"[CONNECT-FAIL] {a.host}:{a.port} -- gateway up? logged in? ({e})")
    # delayed data if no live subscription -> lets us still get a quote for the probe
    try: ib.reqMarketDataType(3)
    except Exception: pass

    acct = ib.managedAccounts()
    print(f"=== IBKR CRYPTO PROBE  gw={a.host}:{a.port}  accounts={acct} ===\n")
    print(f"{'PRODUCT':9} {'SYM':5} {'EXCH':7} {'QUALIFY':8} {'QUOTE':10} {'TRADE?':22} note")
    print("-"*100)

    results = []
    for prod, sym, exch, cur, note in CANDIDATES:
        c = mk_contract(prod, sym, exch, cur)
        qualify, quote, tradeable = "no", "-", "-"
        # GATE 1: does the contract DEFINITION resolve? (product/exchange/region entitlement)
        try:
            det = ib.reqContractDetails(c)
            if det:
                qualify = "YES"
                c = det[0].contract
            else:
                tradeable = "NO-CONTRACT (not entitled/listed)"
        except Exception as e:
            tradeable = f"NO-CONTRACT ({str(e)[:18]})"
        # GATE 2: can we get a QUOTE? (market-data subscription gate)
        if qualify == "YES":
            try:
                t = ib.reqMktData(c, "", True, False); ib.sleep(2.0)
                px = t.last if t.last==t.last else (t.close if t.close==t.close else None)
                quote = f"{px:.2f}" if px else "NO-DATA(sub?)"
                ib.cancelMktData(c)
            except Exception as e:
                quote = f"ERR({str(e)[:8]})"
            # GATE 3: will an ORDER be accepted? whatIf margin-preview = real reject codes, no fill
            if a.live_check:
                try:
                    o = MarketOrder("BUY", 1); o.whatIf = True
                    tr = ib.placeOrder(c, o); ib.sleep(2.5)
                    st = tr.orderStatus
                    if getattr(st, "initMarginChange", "") not in ("", None):
                        tradeable = "YES (margin preview OK)"
                    elif tr.log and any("reject" in str(l.message).lower() or "not" in str(l.message).lower() for l in tr.log):
                        tradeable = f"BLOCKED: {tr.log[-1].message[:40]}"
                    else:
                        tradeable = "UNCLEAR (see raw log)"
                    ib.cancelOrder(o)
                except Exception as e:
                    tradeable = f"ORDER-ERR: {str(e)[:30]}"
            elif qualify == "YES":
                tradeable = "permissioned (add --live-check)"
        print(f"{prod:9} {sym:5} {exch:7} {qualify:8} {quote:10} {tradeable:22} {note}")
        results.append((prod, sym, qualify, quote, tradeable, note))

    print("\n=== DECODE: 'approved but cannot trade' ===")
    print(" QUALIFY=NO   -> product/exchange NOT entitled for THIS account/region (or paper")
    print("                 account doesn't mirror the live grant). This is the usual culprit.")
    print(" QUALIFY=YES + QUOTE=NO-DATA -> permissioned but NO MARKET-DATA SUBSCRIPTION; IBKR")
    print("                 refuses orders on instruments you can't see a live price for.")
    print(" QUALIFY=YES + BLOCKED:... -> read the reject text: margin/short-avail/trading-hours.")
    print(" Anything YES with a margin-preview OK -> genuinely tradeable now.")
    ib.disconnect()

if __name__ == "__main__":
    main()
