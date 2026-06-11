#!/usr/bin/env python3
"""
pump_recalib_bt — calibrate the 3-min pump engine to the operator's model and
sweep EVERY exit lever to see if anything beats it. Reports NET DOLLARS (not %)
so fixed-cost viability is explicit.

Operator model (2026-06-11, PRIMARY): "trail immediately and then exit after 3
min or when price turns." =>
  entry  : day run-high >= GATE% from open AND ignition (c/c_lb3 >= +3%)
           AND close>VWAP AND slope>=0 AND strength>=0.60   (deployed entry)
  manage : trail from entry (tight). Exit on the FIRST of:
             - trail stop hit (price turns), OR
             - TIMECAP bars elapsed (3 min = 1 bar of 3m), OR
             - hard catastrophe stop.
  "UNLESS a variance is better — try all settings and levers."

SWEEP (all combined, both 1% and 2% slip):
  trail%   in {0.5,1.0,1.5,2.0,3.0}
  timecap  in {1,2,3,5,10,30} bars of 3m  (1=3min ... 30=90min/none-ish)
  be-lock  in {off,(arm1,floor1),(arm2,floor2)}
  hard%    in {4,6,10}
  GRACE variant: hold timecap then green-or-cut then trail (vs trail-from-entry)
Sizing is reported at $5000 notional; net$ scales ~linearly with notional, so
the WINNER is size-independent — sizing only sets dollars-per-unit-edge.

Decision rule (shop standard): ship a config only if net AND PF are positive at
BOTH slips AND the per-day table isn't carried by one monster day.

Run ON THE VPS (gateway 127.0.0.1:4002), clientId 77:
  python pump_recalib_bt.py [--days "SYM:YYYYMMDD ..."] [--gate 100] [--top 15]
"""
import argparse, sys, time
from ib_async import IB, Stock

IB_HOST, IB_PORT, IB_CID = "127.0.0.1", 4002, 77
THROTTLE_S = 2.5

LB, IG_PCT, STRENGTH = 3, 3.0, 0.60
REG_LB, SLOPE_MIN = 12, 0.0
MIN_WARMUP = LB + 21

COMMISSION_PER_SHARE = 0.005
COMMISSION_MIN       = 1.00
NOTIONAL_REPORT      = 5000

# sweep axes
TRAILS   = [0.5, 1.0, 1.5, 2.0, 3.0]
TIMECAPS = [1, 2, 3, 5, 10, 30]
BELOCKS  = [(None, None), (1.0, 1.0), (2.0, 2.0)]
HARDS    = [4.0, 6.0, 10.0]
GRACES   = [False, True]

BASKET = ("INHD:20260608 EPSM:20260608 YOUL:20260608 CCTG:20260608 RGNT:20260609 "
          "CHAI:20260609 SLGB:20260609 AZI:20260609 "
          "CIIT:20260610 HCAI:20260610 CHOW:20260610 HKIT:20260610 MSW:20260610 HWH:20260610 "
          "WCT:20260610 VSME:20260610")


def fetch_3m(ib, sym, yyyymmdd):
    c = Stock(sym, "SMART", "USD")
    q = ib.qualifyContracts(c)
    if q: c = q[0]
    end = f"{yyyymmdd} 23:59:59 US/Eastern"
    bars = ib.reqHistoricalData(c, endDateTime=end, durationStr="1 D",
                                barSizeSetting="3 mins", whatToShow="TRADES",
                                useRTH=False, formatDate=1)
    time.sleep(THROTTLE_S)
    return [(int(b.date.timestamp()), b.open, b.high, b.low, b.close, float(b.volume))
            for b in bars]


def slope(closes):
    n = min(len(closes), REG_LB)
    if n < 3: return 0.0
    ys = closes[-n:]; sx = sy = sxx = sxy = 0.0
    for k, y in enumerate(ys):
        sx += k; sy += y; sxx += k*k; sxy += k*y
    den = n*sxx - sx*sx
    if abs(den) < 1e-9: return 0.0
    b = (n*sxy - sx*sy)/den
    m = sy/n
    return b/m*100.0 if m > 0 else 0.0


def usd(entry, exit_px, notional, slip_pct):
    shares = notional/entry if entry > 0 else 0.0
    gross  = shares*(exit_px-entry)
    comm   = 2*max(COMMISSION_MIN, COMMISSION_PER_SHARE*shares)
    slip   = 2*(notional*slip_pct/100.0)
    return gross - comm - slip


def run_day(bars, gate, slip_pct, notional, trail, timecap, be_arm, be_floor,
            hard, grace, min_dvol=0.0, price_min=0.0):
    if not bars: return []
    day_open = bars[0][1]
    run_high = 0.0; cum_pv = cum_v = 0.0; closes = []
    pos = None     # (entry, peak, idx, armed)
    out = []
    for i, (ts, o, h, l, c, v) in enumerate(bars):
        run_high = max(run_high, h)
        cum_pv += (h+l+c)/3.0*v; cum_v += v
        closes.append(c)
        if pos:
            e, peak, ei, armed = pos
            peak = max(peak, h); held = i - ei
            exit_px = None
            hard_stop = e*(1-hard/100)
            if grace and not armed:
                # grace window: only catastrophe guard, then green-or-cut
                if l <= hard_stop: exit_px = hard_stop
                elif held >= timecap:
                    if c > e: armed = True
                    else: exit_px = c
            else:
                # trail-from-entry (operator PRIMARY) or grace-armed phase
                stop = max(peak*(1-trail/100), hard_stop)
                if be_arm is not None and peak >= e*(1+be_arm/100):
                    stop = max(stop, e*(1+be_floor/100))
                if l <= stop: exit_px = stop
                elif (not grace) and held >= timecap: exit_px = c   # 3-min time cap
            if exit_px is not None:
                out.append(usd(e, exit_px, notional, slip_pct)); pos = None
            else:
                pos = (e, peak, ei, armed)
        if pos or day_open <= 0: continue
        if (run_high/day_open-1)*100 < gate: continue
        if len(closes) < MIN_WARMUP: continue
        if price_min > 0 and c < price_min: continue          # liquidity: skip ultra-thin sub-$X
        if min_dvol > 0 and c*v < min_dvol: continue          # liquidity: bar $-volume floor
        vwap = cum_pv/cum_v if cum_v > 0 else 0
        if vwap > 0 and not (c > vwap and slope(closes) >= SLOPE_MIN): continue
        c_lb = closes[-1-LB] if len(closes) > LB else None
        if c_lb is None or c_lb <= 0: continue
        ig = (c/c_lb-1)*100 >= IG_PCT
        stren = (c >= l + STRENGTH*(h-l)) if h > l else True
        if ig and stren:
            pos = (c, c, i, False)
    if pos:
        out.append(usd(pos[0], bars[-1][4], notional, slip_pct))
    return out


def stat(allt):
    n = len(allt)
    if not n: return (0, 0.0, 0.0, 0.0)
    gp = sum(x for x in allt if x > 0); gl = -sum(x for x in allt if x < 0)
    pf = gp/gl if gl > 0 else 999.0
    wr = 100*sum(1 for x in allt if x > 0)/n
    return (n, sum(allt), pf, wr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", default=BASKET)
    ap.add_argument("--gate", type=float, default=100.0)
    ap.add_argument("--top", type=int, default=15)
    a = ap.parse_args()

    ib = IB(); ib.connect(IB_HOST, IB_PORT, clientId=IB_CID, timeout=20)
    try: ib.reqMarketDataType(3)
    except Exception: pass
    data = {}
    for tok in a.days.split():
        sym, day = tok.split(":")
        try:
            data[tok] = fetch_3m(ib, sym, day)
            print(f"# {tok}: {len(data[tok])} bars", file=sys.stderr)
        except Exception as e:
            print(f"# {tok}: FETCH FAIL {e}", file=sys.stderr)
    ib.disconnect()

    # build full grid
    configs = []
    for grace in GRACES:
        for trail in TRAILS:
            for tc in TIMECAPS:
                for (ba, bf) in BELOCKS:
                    for hard in HARDS:
                        configs.append((grace, trail, tc, ba, bf, hard))

    rows = []
    for (grace, trail, tc, ba, bf, hard) in configs:
        t1 = []; t2 = []
        for tok, bars in data.items():
            t1 += run_day(bars, a.gate, 1.0, NOTIONAL_REPORT, trail, tc, ba, bf, hard, grace)
            t2 += run_day(bars, a.gate, 2.0, NOTIONAL_REPORT, trail, tc, ba, bf, hard, grace)
        n1, net1, pf1, wr1 = stat(t1)
        n2, net2, pf2, wr2 = stat(t2)
        rows.append((grace, trail, tc, ba, hard, n1, net1, pf1, wr1, net2, pf2, wr2))

    # operator PRIMARY = grace False, trail immediate, timecap 1 (3min), be off, hard 6
    print("\n==== OPERATOR PRIMARY (trail-from-entry, 3-min cap, no BE) @ $5000 ====")
    print(f"{'cfg':32} {'n':>4} {'net1%slip':>9} {'PF1':>5} {'net2%slip':>9} {'PF2':>5} {'win%':>5}")
    for r in rows:
        grace, trail, tc, ba, hard, n1, net1, pf1, wr1, net2, pf2, wr2 = r
        if (not grace) and trail == 2.0 and tc == 1 and ba is None and hard == 6.0:
            print(f"{'trail2/cap3min/noBE/hard6':32} {n1:4d} {net1:9.0f} {pf1:5.2f} {net2:9.0f} {pf2:5.2f} {wr2:5.0f}")

    # robust winners: positive net at BOTH slips, sort by net @ 2% slip
    robust = [r for r in rows if r[6] > 0 and r[9] > 0]
    robust.sort(key=lambda r: r[9], reverse=True)
    print(f"\n==== TOP {a.top} BY NET @ 2% SLIP (positive at both slips) ====")
    print(f"{'grace':5} {'trl':>4} {'cap':>3} {'be':>4} {'hrd':>4} {'n':>4} "
          f"{'net@1%':>8} {'PF1':>5} {'net@2%':>8} {'PF2':>5} {'win%':>5}")
    for r in robust[:a.top]:
        grace, trail, tc, ba, hard, n1, net1, pf1, wr1, net2, pf2, wr2 = r
        print(f"{str(grace):5} {trail:4.1f} {tc:3d} {str(ba):>4} {hard:4.0f} {n1:4d} "
              f"{net1:8.0f} {pf1:5.2f} {net2:8.0f} {pf2:5.2f} {wr2:5.0f}")

    if not robust:
        print("  (none positive at both slips — pump 3m not viable under cost on this basket)")

    # per-day for the top config (one-day-driven check)
    if robust:
        grace, trail, tc, ba, bf_unused, hard = robust[0][:6]
        be = next((bf for (aa, bf) in BELOCKS if aa == ba), None)
        print(f"\nper-day netUSD @ winner (grace={grace} trail={trail} cap={tc} be={ba} hard={hard}), $5000, slip2%:")
        for tok, bars in data.items():
            net = sum(run_day(bars, a.gate, 2.0, NOTIONAL_REPORT, trail, tc, ba, be, hard, grace))
            print(f"  {tok:16} {net:9.0f}")


if __name__ == "__main__":
    main()
