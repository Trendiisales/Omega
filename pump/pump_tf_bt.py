#!/usr/bin/env python3
"""
pump_tf_bt — entry-TIMEFRAME A/B for PumpScalpEngine (long side).

Question (operator, 2026-06-11): does a 2/3/4-minute entry bar beat the
deployed 5m? (1m already known = slippage trap; included as the control —
if 1m doesn't die here the harness is broken.)

Held CONSTANT (deployed config): gate 100, ignition c/c_lb3 >= +3%, strength
0.60, VWAP+slope filter, NO volume condition (VOLX=0 live), exits trail 3% /
hard 6% / maxhold 30 bars OF THAT TF, long-only, NO BE-lock (operator call).
Only the bar interval changes: 1/2/3/4/5 minutes.

Run ON THE VPS (gateway 127.0.0.1:4002), clientId 79:
  python pump_tf_bt.py [--days "SYM:YYYYMMDD ..."] [--gate 100]
"""
import argparse, sys, time
from ib_async import IB, Stock

IB_HOST, IB_PORT, IB_CID = "127.0.0.1", 4002, 79
THROTTLE_S = 2.5

LB, IG_PCT, STRENGTH = 3, 3.0, 0.60
TRAIL_PCT, HARD_PCT, MAXHOLD = 3.0, 6.0, 30
REG_LB, SLOPE_MIN = 12, 0.0

TFS = {1: "1 min", 2: "2 mins", 3: "3 mins", 4: "4 mins", 5: "5 mins"}

BASKET = ("INHD:20260408 EPSM:20260608 YOUL:20260608 CCTG:20260608 RGNT:20260609 "
          "CHAI:20260609 SLGB:20260609 AZI:20260609 "
          "CIIT:20260610 HCAI:20260610 CHOW:20260610 HKIT:20260610 MSW:20260610 HWH:20260610 "
          "WCT:20260610 VSME:20260610")


def fetch(ib, sym, yyyymmdd, bar_size):
    c = Stock(sym, "SMART", "USD")
    q = ib.qualifyContracts(c)
    if q: c = q[0]
    end = f"{yyyymmdd} 23:59:59 US/Eastern"
    bars = ib.reqHistoricalData(c, endDateTime=end, durationStr="1 D",
                                barSizeSetting=bar_size, whatToShow="TRADES",
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


def run_day(bars, gate, slip_pct, min_warmup=LB+21):
    """Long-only sim on the given bars, deployed exits (trail3/hard6/30-bar)."""
    if not bars: return []
    day_open = bars[0][1]
    run_high = 0.0
    cum_pv = cum_v = 0.0
    closes = []
    pos = None; trades = []
    for i, (ts, o, h, l, c, v) in enumerate(bars):
        run_high = max(run_high, h)
        cum_pv += (h+l+c)/3.0*v; cum_v += v
        closes.append(c)
        if pos:
            e, peak, ei = pos
            peak = max(peak, h)
            stop = max(peak*(1-TRAIL_PCT/100), e*(1-HARD_PCT/100))
            exit_px = None
            if l <= stop: exit_px = stop
            elif i - ei >= MAXHOLD: exit_px = c
            if exit_px is not None:
                trades.append((exit_px/e - 1)*100 - 2*slip_pct)
                pos = None
            else:
                pos = (e, peak, ei)
        if pos or day_open <= 0: continue
        if (run_high/day_open - 1)*100 < gate: continue
        if len(closes) < min_warmup: continue
        vwap = cum_pv/cum_v if cum_v > 0 else 0
        if vwap > 0 and not (c > vwap and slope(closes) >= SLOPE_MIN): continue
        c_lb = closes[-1-LB] if len(closes) > LB else None
        if c_lb is None or c_lb <= 0: continue
        ig = (c/c_lb - 1)*100 >= IG_PCT
        stren = (c >= l + STRENGTH*(h-l)) if h > l else True
        if ig and stren:
            pos = (c, c, i)
    if pos:
        trades.append((bars[-1][4]/pos[0]-1)*100 - 2*slip_pct)
    return trades


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", default=BASKET)
    ap.add_argument("--gate", type=float, default=100.0)
    a = ap.parse_args()

    ib = IB(); ib.connect(IB_HOST, IB_PORT, clientId=IB_CID, timeout=20)
    try: ib.reqMarketDataType(3)
    except Exception: pass

    # data[tf][tok] = bars
    data = {tf: {} for tf in TFS}
    for tok in a.days.split():
        sym, day = tok.split(":")
        for tf, bs in TFS.items():
            try:
                data[tf][tok] = fetch(ib, sym, day, bs)
            except Exception as e:
                print(f"# {tok} {bs}: FETCH FAIL {e}", file=sys.stderr)
        print(f"# {tok}: " + " ".join(f"{tf}m={len(data[tf].get(tok, []))}" for tf in TFS),
              file=sys.stderr)
    ib.disconnect()

    for slip in (1.0, 2.0):
        print(f"\n==== slip {slip}%/side, gate {a.gate} (exits: trail3/hard6/30-bar, NO BE) ====")
        print(f"{'tf':>4} {'n':>4} {'net%':>8} {'PF':>6} {'win%':>5} {'avg%':>7}")
        for tf in TFS:
            allt = []
            for tok, bars in data[tf].items():
                allt += run_day(bars, a.gate, slip)
            gp = sum(x for x in allt if x > 0); gl = -sum(x for x in allt if x < 0)
            pf = gp/gl if gl > 0 else float("inf")
            n = len(allt)
            wr = 100*sum(1 for x in allt if x > 0)/n if n else 0
            print(f"{tf:3d}m {n:4d} {sum(allt):8.1f} {pf:6.2f} {wr:5.0f} "
                  f"{(sum(allt)/n if n else 0):7.2f}")

    print("\nper-day net% @ slip 1.0:  " + "  ".join(f"{tf}m".rjust(8) for tf in TFS))
    for tok in data[5]:
        row = [sum(run_day(data[tf].get(tok, []), a.gate, 1.0)) for tf in TFS]
        print(f"  {tok:16} " + "  ".join(f"{x:8.1f}" for x in row))


if __name__ == "__main__":
    main()
