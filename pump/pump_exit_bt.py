#!/usr/bin/env python3
"""
pump_exit_bt — EXIT-variant A/B for PumpScalpEngine (5m, long side).

Question (operator, 2026-06-11): trades that go positive then trail out
NEGATIVE — can a break-even lock stop that? "Track the trade and exit once it
tracks back to BE, especially if we are trailing aggressively."

Variants (entries IDENTICAL = deployed live config: ignition WITHOUT volume
condition [VOLX=0 live since 2026-06-10e], close>VWAP, slope>=0, strength 0.60,
gate 100, 5m, long-only):
  TRAIL3        3% tick-trail + 6% hard + 30-bar maxhold      (deployed)
  TRAIL2        2% trail (sweep said tighter=better; shipped 3% for slip margin)
  BE1/BE2/BE3   TRAIL3 + BE-lock: once peak >= entry*(1+arm%), stop floors at
                NET break-even (entry*(1+2*slip)) — the operator's ask
  BE2T2         BE2 on the 2% trail (aggressive-trail + BE combo)

History warning: BE-early locks have FAILED on evidence in this shop twice
(XauVolBreakout BE-OFF 4x better; Chimera S36 preset rewrite; gvb overlay
audit: nothing beats the wide-trail base exit). This run answers it for the
pump regime specifically. Decision rule: ship only if pooled net AND PF improve
at BOTH 1% and 2% slip AND per-day table doesn't show one-day-driven flips.

Run ON THE VPS (gateway 127.0.0.1:4002), throttled, clientId 78:
  python pump_exit_bt.py [--days "SYM:YYYYMMDD ..."] [--gate 100]
"""
import argparse, sys, time
from ib_async import IB, Stock

IB_HOST, IB_PORT, IB_CID = "127.0.0.1", 4002, 78   # distinct research clientId
THROTTLE_S = 2.5

LB, IG_PCT, STRENGTH = 3, 3.0, 0.60
HARD_PCT, MAXHOLD = 6.0, 30
REG_LB, SLOPE_MIN = 12, 0.0

BASKET = ("INHD:20260608 EPSM:20260608 YOUL:20260608 CCTG:20260608 RGNT:20260609 "
          "CHAI:20260609 SLGB:20260609 AZI:20260609 "
          "CIIT:20260610 HCAI:20260610 CHOW:20260610 HKIT:20260610 MSW:20260610 HWH:20260610 "
          "WCT:20260610 VSME:20260610")

EXITS = {  # name -> (trail_pct, be_arm_pct or None)
    "TRAIL3": (3.0, None),
    "TRAIL2": (2.0, None),
    "BE1":    (3.0, 1.0),
    "BE2":    (3.0, 2.0),
    "BE3":    (3.0, 3.0),
    "BE2T2":  (2.0, 2.0),
}


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


def run_day(bars, gate, slip_pct, trail_pct, be_arm, min_warmup=LB+21):
    """Long-only 5m sim, bar-path exits (low first), entries = deployed config."""
    if not bars: return []
    day_open = bars[0][1]
    run_high = 0.0
    cum_pv = cum_v = 0.0
    closes = []
    pos = None; trades = []          # pos = (entry_net, peak, idx)
    be_floor_mult = 1 + 2*slip_pct/100.0   # NET break-even exit price multiplier
    for i, (ts, o, h, l, c, v) in enumerate(bars):
        run_high = max(run_high, h)
        cum_pv += (h+l+c)/3.0*v; cum_v += v
        closes.append(c)
        if pos:
            e, peak, ei = pos
            peak = max(peak, h)
            stop = max(peak*(1-trail_pct/100), e*(1-HARD_PCT/100))
            if be_arm is not None and peak >= e*(1+be_arm/100):
                stop = max(stop, e*be_floor_mult)     # BE-lock armed
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
    ap.add_argument("--min-warmup", type=int, default=LB+21)
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

    for slip in (1.0, 2.0):
        print(f"\n==== slip {slip}%/side, gate {a.gate} ====")
        print(f"{'exit':8} {'n':>4} {'net%':>8} {'PF':>6} {'win%':>5} {'avg%':>7} {'neg-after-pos':>13}")
        for name, (trail, be) in EXITS.items():
            allt = []
            for tok, bars in data.items():
                allt += run_day(bars, a.gate, slip, trail, be, a.min_warmup)
            gp = sum(x for x in allt if x > 0); gl = -sum(x for x in allt if x < 0)
            pf = gp/gl if gl > 0 else float("inf")
            n = len(allt)
            wr = 100*sum(1 for x in allt if x > 0)/n if n else 0
            print(f"{name:8} {n:4d} {sum(allt):8.1f} {pf:6.2f} {wr:5.0f} "
                  f"{(sum(allt)/n if n else 0):7.2f}")

    print("\nper-day net% @ slip 1.0:  " + "  ".join(f"{k:>7}" for k in EXITS))
    for tok, bars in data.items():
        row = [sum(run_day(bars, a.gate, 1.0, t, b, a.min_warmup)) for (t, b) in EXITS.values()]
        print(f"  {tok:16} " + "  ".join(f"{x:7.1f}" for x in row))


if __name__ == "__main__":
    main()
