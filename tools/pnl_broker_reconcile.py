#!/usr/bin/env python3
# pnl_broker_reconcile.py — RECONCILE internal (ledger) realized PnL vs the IBKR account
# (audit gap 16, S-2026-07-24). Nothing checked Omega's own PnL against the broker's
# NetLiquidation / cash / realized-PnL, so an ETF-scale seam, a phantom fill, or an
# unlogged trade could drift for days unseen. This is a READ-ONLY reconcile:
#   * reqAccountSummary -> NetLiquidation, TotalCashValue, RealizedPnL
#   * reqPnL(account)   -> dailyPnL, realizedPnL, unrealizedPnL
#   * ledger            -> sum of today's realized (broker_pnl, fallback net_pnl)
# It goes RED when |IB realized - ledger realized| exceeds the threshold. NO orders, NO
# account mutation (AUDIT_PROBE_SAFETY). Runs ON THE BOX (connects 127.0.0.1:4001, the
# only place the gateway lives); a Mac wrapper ssh's it. Prints a `RESULT: GREEN|RED|BLIND`
# line the wrapper keys on.
#
# This is NOT a bulk historical pull (feedback-no-bulk-pulls-production-gateway) — it is
# two lightweight account queries; it does not stress the exec gateway.
#
# Usage:
#   python tools/pnl_broker_reconcile.py                 # live reconcile on the box
#   python tools/pnl_broker_reconcile.py --port 4001 --abs 250 --pct 0.05
#   python tools/pnl_broker_reconcile.py --self-test     # offline: synthetic, no ib_insync
#
# Exit: 0 GREEN, 2 RED (divergence), 1 BLIND (could not connect/read — fail LOUD, never
# a false green).
from __future__ import annotations
import argparse, csv, sys, time
from datetime import datetime, timezone

DEFAULT_LEDGER = "C:/Omega/logs/trades/omega_trade_closes.csv"


def _f(x) -> float:
    try:
        return float(x)
    except (TypeError, ValueError):
        return 0.0


def ledger_realized_today(path: str) -> tuple[float, float, int]:
    """Sum today's (UTC) realized PnL from the close ledger.
    Returns (broker_pnl_sum, net_pnl_sum, n_trades). broker_pnl is the broker-reported
    column when present; net_pnl is Omega's internal net."""
    today = datetime.now(timezone.utc).date()
    bsum = nsum = 0.0
    n = 0
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            ux = _f(r.get("exit_ts_unix"))
            if ux <= 0:
                continue
            if datetime.fromtimestamp(ux, timezone.utc).date() != today:
                continue
            n += 1
            nsum += _f(r.get("net_pnl"))
            bsum += _f(r.get("broker_pnl"))
    return bsum, nsum, n


def verdict(ib_realized: float, led_broker: float, led_net: float, n: int,
            abs_thr: float, pct_thr: float,
            netliq: float, cash: float) -> int:
    # Prefer the broker_pnl column for the comparison (like-for-like vs IB realized);
    # fall back to net_pnl if the ledger booked no broker_pnl today.
    led = led_broker if abs(led_broker) > 1e-9 else led_net
    basis = "broker_pnl" if abs(led_broker) > 1e-9 else "net_pnl(fallback)"
    div = ib_realized - led
    tol = max(abs_thr, pct_thr * abs(ib_realized))
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    print(f"[{ts}] IB: NetLiq={netliq:.2f} Cash={cash:.2f} realizedPnL={ib_realized:+.2f}")
    print(f"          Ledger today: {basis}={led:+.2f} (net={led_net:+.2f} broker={led_broker:+.2f}, n={n})")
    print(f"          divergence={div:+.2f}  tolerance={tol:.2f} (abs={abs_thr} pct={pct_thr})")
    if abs(div) > tol:
        print(f"RESULT: RED PnL reconcile DIVERGENCE {div:+.2f} > tol {tol:.2f} — "
              f"IB realized {ib_realized:+.2f} vs ledger {led:+.2f} ({basis}). "
              f"Phantom fill / unlogged trade / mismark — investigate.")
        return 2
    print(f"RESULT: GREEN PnL reconciled (divergence {div:+.2f} within {tol:.2f}).")
    return 0


def self_test() -> int:
    print("== self-test: synthetic reconcile (no ib_insync, no connection) ==")
    rc_green = verdict(ib_realized=1230.50, led_broker=1225.00, led_net=1180.00, n=7,
                       abs_thr=250.0, pct_thr=0.05, netliq=105000.0, cash=40000.0)
    print(f"  -> expected GREEN, got rc={rc_green}")
    rc_red = verdict(ib_realized=1230.50, led_broker=100.00, led_net=95.00, n=3,
                     abs_thr=250.0, pct_thr=0.05, netliq=105000.0, cash=40000.0)
    print(f"  -> expected RED, got rc={rc_red}")
    ok = (rc_green == 0 and rc_red == 2)
    print("SELF-TEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def live(port: int, ledger: str, abs_thr: float, pct_thr: float) -> int:
    try:
        from ib_insync import IB
    except Exception as e:  # noqa: BLE001
        print(f"RESULT: BLIND ib_insync import failed: {e}", file=sys.stderr)
        return 1
    ib = IB()
    try:
        ib.connect("127.0.0.1", port, clientId=73, timeout=20)
    except Exception as e:  # noqa: BLE001
        print(f"RESULT: BLIND could not connect 127.0.0.1:{port}: {e}", file=sys.stderr)
        return 1
    try:
        accts = ib.managedAccounts()
        if not accts:
            print("RESULT: BLIND no managed accounts", file=sys.stderr)
            return 1
        acct = accts[0]
        summ = {v.tag: v.value for v in ib.accountSummary(acct)}
        netliq = _f(summ.get("NetLiquidation"))
        cash = _f(summ.get("TotalCashValue"))
        # reqPnL streams; give it a moment to populate.
        pnl = ib.reqPnL(acct)
        for _ in range(10):
            ib.sleep(0.5)
            if pnl.realizedPnL == pnl.realizedPnL and pnl.realizedPnL is not None:  # not NaN/None
                break
        ib_realized = _f(getattr(pnl, "realizedPnL", 0.0))
        # accountSummary RealizedPnL is a robust fallback when reqPnL hasn't streamed.
        if ib_realized == 0.0 and "RealizedPnL" in summ:
            ib_realized = _f(summ.get("RealizedPnL"))
        try:
            led_broker, led_net, n = ledger_realized_today(ledger)
        except FileNotFoundError:
            print(f"RESULT: BLIND ledger not found: {ledger}", file=sys.stderr)
            return 1
        return verdict(ib_realized, led_broker, led_net, n, abs_thr, pct_thr, netliq, cash)
    finally:
        ib.disconnect()


def main() -> None:
    ap = argparse.ArgumentParser(description="Read-only IBKR PnL <-> ledger reconcile.")
    ap.add_argument("--port", type=int, default=4001)
    ap.add_argument("--ledger", default=DEFAULT_LEDGER)
    ap.add_argument("--abs", dest="abs_thr", type=float, default=250.0,
                    help="absolute $ divergence tolerance")
    ap.add_argument("--pct", dest="pct_thr", type=float, default=0.05,
                    help="fractional tolerance vs IB realized (max of abs/pct wins)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        sys.exit(self_test())
    sys.exit(live(args.port, args.ledger, args.abs_thr, args.pct_thr))


if __name__ == "__main__":
    main()
