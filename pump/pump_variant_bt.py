#!/usr/bin/env python3
"""
pump_variant_bt — entry-variant A/B for PumpScalpEngine (5m, long side).

Question (operator, 2026-06-10): should the engine enter on the BAR THAT
CROSSES the +100% day-expansion gate (no thrust/volume/strength conditions),
instead of waiting for the validated ignition thrust? And: how load-bearing is
the VOLUME condition (live tick-delta volume is fragile on the delayed feed)?

Variants (exits IDENTICAL: 3% tick-trail approximated at bar path, 6% hard,
30-bar maxhold, long-only, gate 100, 5m):
  BASE      ignition: c/c_lb3>=+3% AND v>=3x avg20 AND close in top 40% AND
            c>VWAP AND slope>=0          (deployed config)
  GATECROSS enter on the closed bar where run_high first reaches gate
            (no other conditions)
  NOVOL     ignition WITHOUT the volume condition (rest identical)

Run ON THE VPS (gateway 127.0.0.1:4002), throttled, clientId 77:
  python pump_variant_bt.py --days "SYM:YYYYMMDD ..." [--slip 1.0] [--gate 100]
Defaults to the standard basket (Jun-08/09 + Apr-08 INHD + Jun-10 live names).
"""
import argparse, math, sys, time
from ib_async import IB, Stock, util

IB_HOST, IB_PORT, IB_CID = "127.0.0.1", 4002, 77   # dedicated research clientId
THROTTLE_S = 2.5                                   # pacing: be a polite citizen

# engine params (mirror PumpScalpEngine 5m)
LB, IG_PCT, VOLX, STRENGTH = 3, 3.0, 3.0, 0.60
TRAIL_PCT, HARD_PCT, MAXHOLD = 3.0, 6.0, 30
REG_LB, SLOPE_MIN = 12, 0.0

BASKET = ("INHD:20260408 EPSM:20260608 YOUL:20260608 CCTG:20260608 RGNT:20260609 "
          "CHAI:20260609 SLGB:20260609 AZI:20260609 "
          "CIIT:20260610 HCAI:20260610 CHOW:20260610 HKIT:20260610 MSW:20260610 HWH:20260610")


def fetch_5m(ib, sym, yyyymmdd):
    c = Stock(sym, "SMART", "USD")
    q = ib.qualifyContracts(c)
    if q: c = q[0]
    end = f"{yyyymmdd} 23:59:59 US/Eastern"
    bars = ib.reqHistoricalData(c, endDateTime=end, durationStr="1 D",
                                barSizeSetting="5 mins", whatToShow="TRADES",
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


def run_day(bars, variant, gate, slip_pct):
    """Long-only 5m sim, bar-path exits (mirrors pump_backtest mechanics)."""
    if not bars: return []
    day_open = bars[0][1]
    run_high = 0.0; gate_armed_prev = False
    cum_pv = cum_v = 0.0
    ema9 = None; closes = []; vols = []
    pos = None; trades = []
    for i, (ts, o, h, l, c, v) in enumerate(bars):
        run_high = max(run_high, h)
        cum_pv += (h+l+c)/3.0*v; cum_v += v
        ema9 = c if ema9 is None else (c-ema9)*(2/10)+ema9
        closes.append(c); vols.append(v)
        # ---- manage open (bar-path: low first for longs) ----
        if pos:
            e, peak, ei = pos
            peak = max(peak, h)
            stop = max(peak*(1-TRAIL_PCT/100), e*(1-HARD_PCT/100))
            exit_px = None
            if l <= stop: exit_px = stop
            elif i - ei >= MAXHOLD: exit_px = c
            if exit_px is not None:
                gross = (exit_px/e - 1)*100
                net = gross - 2*slip_pct
                trades.append(net)
                pos = None
            else:
                pos = (e, peak, ei)
        # ---- entries ----
        if pos or day_open <= 0: continue
        gate_now = (run_high/day_open - 1)*100 >= gate
        crossed_this_bar = gate_now and not gate_armed_prev
        gate_armed_prev = gate_armed_prev or gate_now
        if not gate_now: continue
        if len(closes) < LB+21 and variant != "GATECROSS": continue
        vwap = cum_pv/cum_v if cum_v > 0 else 0
        sl_ok = (c > vwap and slope(closes) >= SLOPE_MIN) if vwap > 0 else True
        if variant == "GATECROSS":
            if crossed_this_bar:
                pos = (c*(1+slip_pct/100), c, i)
            continue
        c_lb = closes[-1-LB] if len(closes) > LB else None
        if c_lb is None or c_lb <= 0: continue
        ig = (c/c_lb - 1)*100 >= IG_PCT
        avgv = sum(vols[-21:-1])/max(1, len(vols[-21:-1]))
        vol_ok = (v >= VOLX*avgv) if variant == "BASE" else True
        stren = (c >= l + STRENGTH*(h-l)) if h > l else True
        if ig and vol_ok and stren and sl_ok:
            pos = (c*(1+slip_pct/100), c, i)
    if pos:  # mark last
        trades.append((bars[-1][4]/pos[0]-1)*100 - 2*slip_pct)
    return trades


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", default=BASKET)
    ap.add_argument("--slip", type=float, default=1.0)
    ap.add_argument("--gate", type=float, default=100.0)
    a = ap.parse_args()

    ib = IB(); ib.connect(IB_HOST, IB_PORT, clientId=IB_CID, timeout=20)
    try: ib.reqMarketDataType(3)
    except Exception: pass

    data = {}
    for tok in a.days.split():
        sym, day = tok.split(":")
        try:
            data[tok] = fetch_5m(ib, sym, day)
            print(f"# {tok}: {len(data[tok])} bars", file=sys.stderr)
        except Exception as e:
            print(f"# {tok}: FETCH FAIL {e}", file=sys.stderr)
    ib.disconnect()

    for slip in (a.slip, 2.0):
        print(f"\n==== slip {slip}%/side, gate {a.gate} ====")
        print(f"{'variant':10} {'n':>4} {'net%':>8} {'PF':>6} {'win%':>5} {'avg%':>7}")
        for variant in ("BASE", "NOVOL", "GATECROSS"):
            allt = []
            per = []
            for tok, bars in data.items():
                t = run_day(bars, variant, a.gate, slip)
                allt += t
                if t: per.append((tok, sum(t)))
            gp = sum(x for x in allt if x > 0); gl = -sum(x for x in allt if x < 0)
            pf = gp/gl if gl > 0 else float("inf")
            n = len(allt)
            wr = 100*sum(1 for x in allt if x > 0)/n if n else 0
            print(f"{variant:10} {n:4d} {sum(allt):8.1f} {pf:6.2f} {wr:5.0f} "
                  f"{(sum(allt)/n if n else 0):7.2f}")
        # per-day detail for the decider
    print("\nper-day net% (BASE | NOVOL | GATECROSS) @ slip", a.slip)
    for tok, bars in data.items():
        row = [sum(run_day(bars, v, a.gate, a.slip)) for v in ("BASE", "NOVOL", "GATECROSS")]
        print(f"  {tok:16} {row[0]:8.1f} {row[1]:8.1f} {row[2]:8.1f}")


if __name__ == "__main__":
    main()
