#!/usr/bin/env python3
"""
pump_lever_bt -- MORE-TRADES lever sweep on the DEPLOYED pump-scalp config.

Operator question 2026-06-12: "recheck pump scalp; what makes MORE trades while
keeping the edge? we should react fast when scanner names gain quickly."

Tombstone-respecting: does NOT retest ride-once / smoothness / quality filters
(all failed, see memory omega-pump-be2t2-clustergate). New levers only:
  A. GATE TIERS   : day-gate 40/60/80/100% (with the $2M/bar liq gate that did
                    not exist when gate100>gate50 was established)
  B. FASTER ENTRY : ignition 2%/3% over LB 2/3 bars (earlier trigger)
  C. RE-ENTRY CAP : max entries/day 1/2/4/unlimited (deployed=2)
  D. SHORT LEG    : exhaustion top-fade (runup>=20, ext>=5 over EMA9, fresh HOD
                    <=8 bars, bearish break) -- promising thin-n in 2026-06-10 test

Data : Yahoo 5m RTH (60d window). 5m proxies the live 3m -- mechanism A/B only,
       absolute net is optimistic vs live fills (the known live drain).
Cost : $1000 notional, commission $1min/$0.005sh, slip 1%/2% per side (both
       must agree to call a winner -- shop standard).
Deployed baseline config: gate100 trail2 hard6 cap15min maxent2 liq($2M,p>=1).

Usage: python3 pump_lever_bt.py [--basket-csv pump/mover_scan_out40.csv]
                                [--max-names 70] [--min-day 20260501]
"""
import argparse, csv, datetime as dt, json, math, sys, time, urllib.request

UA = {"User-Agent": "Mozilla/5.0"}
NOTIONAL = 1000.0
COMMISSION_MIN = 1.00
COMMISSION_PER_SHARE = 0.005
PRICE_MIN = 1.0
MIN_DVOL_USD = 2.0e6
CAP_BARS = 3          # 15 min at 5m
TRAIL = 2.0
HARD = 6.0

LEGACY = ("INHD:20260608 EPSM:20260608 YOUL:20260608 CCTG:20260608 RGNT:20260609 "
          "CHAI:20260609 SLGB:20260609 AZI:20260609 "
          "CIIT:20260610 HCAI:20260610 CHOW:20260610 HKIT:20260610 MSW:20260610 HWH:20260610 "
          "WCT:20260610 VSME:20260610")


def fetch_5m(sym):
    url = (f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}"
           f"?interval=5m&range=60d&includePrePost=true")
    for attempt in range(4):
        try:
            req = urllib.request.Request(url, headers=UA)
            d = json.load(urllib.request.urlopen(req, timeout=20))
            r = d["chart"]["result"][0]
            ts = r.get("timestamp")
            q = r["indicators"]["quote"][0]
            if not ts:
                time.sleep(1.5 * (attempt + 1)); continue
            out = []
            for i, t in enumerate(ts):
                o, h, l, c, v = q["open"][i], q["high"][i], q["low"][i], q["close"][i], q["volume"][i]
                if None in (o, h, l, c):
                    continue
                out.append((t, o, h, l, c, float(v or 0)))
            return out
        except Exception:
            time.sleep(1.5 * (attempt + 1))
    return []


def day_bars(bars, yyyymmdd):
    d = dt.datetime.strptime(yyyymmdd, "%Y%m%d").date()
    return [b for b in bars if dt.datetime.utcfromtimestamp(b[0]).date() == d]


def usd(entry, exit_px, slip_pct, side=1):
    if entry <= 0:
        return 0.0
    shares = NOTIONAL / entry
    gross = shares * (exit_px - entry) * side
    comm = 2 * max(COMMISSION_MIN, COMMISSION_PER_SHARE * shares)
    slip = 2 * (NOTIONAL * slip_pct / 100.0)
    return gross - comm - slip


def slope_pct_per_bar(closes, lb=12):
    ys = closes[-lb:]
    n = len(ys)
    if n < 3 or ys[0] <= 0:
        return 0.0
    sx = sy = sxx = sxy = 0.0
    for k, y in enumerate(ys):
        sx += k; sy += y; sxx += k * k; sxy += k * y
    den = n * sxx - sx * sx
    if den == 0:
        return 0.0
    b = (n * sxy - sx * sy) / den
    return 100.0 * b / ys[0]


def run_long(bars, gate, slip, ig_pct=3.0, lb=3, maxent=2,
             trail=TRAIL, cap=CAP_BARS, hard=HARD):
    """Deployed long leg: gate + ignition + VWAP + slope>=0 + strength + liq gate."""
    if not bars:
        return []
    day_open = bars[0][1]
    run_high = 0.0; cum_pv = cum_v = 0.0
    closes = []; out = []; pos = None; nent = 0
    for i, (t, o, h, l, c, v) in enumerate(bars):
        run_high = max(run_high, h)
        cum_pv += (h + l + c) / 3 * v; cum_v += v
        closes.append(c)
        if pos:
            e, peak, ei = pos
            peak = max(peak, h)
            held = i - ei
            stop = max(peak * (1 - trail / 100), e * (1 - hard / 100))
            ex = None
            if l <= stop: ex = stop
            elif held >= cap: ex = c
            if ex is not None:
                out.append(usd(e, ex, slip)); pos = None
            else:
                pos = (e, peak, ei)
        if pos or day_open <= 0:
            continue
        if maxent and nent >= maxent:
            continue
        if (run_high / day_open - 1) * 100 < gate:
            continue
        if len(closes) < max(lb + 1, 13):
            continue
        if c < PRICE_MIN or c * v < MIN_DVOL_USD:      # deployed liq gate
            continue
        vwap = cum_pv / cum_v if cum_v > 0 else 0
        if not (vwap > 0 and c > vwap):
            continue
        if slope_pct_per_bar(closes) < 0.0:
            continue
        clb = closes[-(lb + 1)]
        if clb <= 0:
            continue
        strong = (c >= l + 0.6 * (h - l)) if h > l else True
        if (c / clb - 1) * 100 >= ig_pct and strong:
            pos = (c, c, i); nent += 1
    if pos:
        out.append(usd(pos[0], bars[-1][4], slip))
    return out


def run_short(bars, slip, runup=20.0, ext=5.0, hod_m=8, trail=3.0, cap=6,
              hard=HARD, maxent=2):
    """Exhaustion top-fade short. Trail off trough; liq gate applies."""
    if not bars:
        return []
    day_open = bars[0][1]
    run_high = 0.0; hod_idx = 0
    closes = []; ema9 = []; out = []; pos = None; nent = 0
    k = 2.0 / 10.0
    prev_low = None
    for i, (t, o, h, l, c, v) in enumerate(bars):
        if h > run_high:
            run_high = h; hod_idx = i
        closes.append(c)
        ema9.append(c if not ema9 else ema9[-1] + k * (c - ema9[-1]))
        if pos:
            e, trough, ei = pos
            trough = min(trough, l)
            held = i - ei
            stop = min(trough * (1 + trail / 100), e * (1 + hard / 100))
            ex = None
            if h >= stop: ex = stop
            elif held >= cap: ex = c
            if ex is not None:
                out.append(usd(e, ex, slip, side=-1)); pos = None
            else:
                pos = (e, trough, ei)
        if pos is None and day_open > 0 and i > 0 and len(closes) >= 13:
            ru = (c / day_open - 1) * 100
            ex_pct = (c / ema9[-1] - 1) * 100 if ema9[-1] > 0 else 0
            fresh = (i - hod_idx) <= hod_m
            bearish = c < o and prev_low is not None and c < prev_low
            liq_ok = c >= PRICE_MIN and c * v >= MIN_DVOL_USD
            if (not (maxent and nent >= maxent)) and ru >= runup and ex_pct >= ext and fresh and bearish and liq_ok:
                pos = (c, c, i); nent += 1
        prev_low = l
    if pos:
        out.append(usd(pos[0], bars[-1][4], slip, side=-1))
    return out


def stat(t):
    n = len(t)
    if not n:
        return (0, 0.0, 0.0, 0.0)
    gp = sum(x for x in t if x > 0); gl = -sum(x for x in t if x < 0)
    pf = gp / gl if gl > 0 else 999.0
    wr = 100 * sum(1 for x in t if x > 0) / n
    return (n, sum(t), pf, wr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--basket-csv", default=None)
    ap.add_argument("--max-names", type=int, default=70)
    ap.add_argument("--min-day", default="20260501")
    ap.add_argument("--min-pct", type=float, default=40.0)
    a = ap.parse_args()

    basket = []           # list of (sym, yyyymmdd, scan_pct)
    seen = set()
    for tok in LEGACY.split():
        s, d = tok.split(":")
        basket.append((s, d, 100.0)); seen.add((s, d))
    if a.basket_csv:
        rows = []
        with open(a.basket_csv) as f:
            for r in csv.DictReader(f):
                try:
                    day = r["date"].replace("-", "")
                    pct = float(r["intraday_pct"])
                except Exception:
                    continue
                if day < a.min_day or pct < a.min_pct:
                    continue
                if (r["symbol"], day) in seen:
                    continue
                rows.append((r["symbol"], day, pct))
        rows.sort(key=lambda x: -x[2])
        per_tier = {"40-60": [], "60-80": [], "80-100": [], ">=100": []}
        for r in rows:
            p = r[2]
            tier = ("40-60" if p < 60 else "60-80" if p < 80 else
                    "80-100" if p < 100 else ">=100")
            per_tier[tier].append(r)
        # stratified: cover the sub-100 tiers properly (the whole point of the test)
        take = {"40-60": 25, "60-80": 25, "80-100": 20, ">=100": 25}
        for tier, k in take.items():
            basket += per_tier[tier][:k]

    print(f"# basket: {len(basket)} name-days", file=sys.stderr)
    data = {}
    for sym, day, pct in basket:
        b = fetch_5m(sym)
        db = day_bars(b, day) if b else []
        if len(db) >= 20:
            data[(sym, day)] = (db, pct)
        time.sleep(0.35)
    print(f"# fetched {len(data)}/{len(basket)} name-days with usable 5m bars", file=sys.stderr)

    tiers = {"40-60": [], "60-80": [], "80-100": [], ">=100": []}
    for (s, d), (db, pct) in data.items():
        if pct < 60: tiers["40-60"].append((s, d))
        elif pct < 80: tiers["60-80"].append((s, d))
        elif pct < 100: tiers["80-100"].append((s, d))
        else: tiers[">=100"].append((s, d))
    print("# tier counts: " + " ".join(f"{k}:{len(v)}" for k, v in tiers.items()), file=sys.stderr)

    # ---- A+C: gate tier x re-entry cap (deployed entry params) ----
    print("\n=== A+C: GATE x MAXENT (long, deployed entry, trail2 cap15m hard6, liq gate) ===")
    print(f"{'gate':>5} {'maxent':>6} | {'n@1%':>5} {'net$@1%':>9} {'PF@1%':>6} {'WR@1%':>5} | "
          f"{'n@2%':>5} {'net$@2%':>9} {'PF@2%':>6}")
    for gate in (40.0, 60.0, 80.0, 100.0):
        for maxent in (1, 2, 4, 0):
            row = []
            for slip in (1.0, 2.0):
                allt = []
                for (s, d), (db, pct) in data.items():
                    allt += run_long(db, gate, slip, maxent=maxent)
                row.append(stat(allt))
            (n1, net1, pf1, wr1), (n2, net2, pf2, _) = row
            print(f"{gate:5.0f} {maxent or 'inf':>6} | {n1:5d} {net1:9.0f} {pf1:6.2f} {wr1:5.0f} | "
                  f"{n2:5d} {net2:9.0f} {pf2:6.2f}")

    # ---- B: faster ignition at deployed gate100 maxent2 ----
    print("\n=== B: FASTER ENTRY (gate100, maxent2): ignition % x lookback ===")
    print(f"{'ig%':>4} {'lb':>3} | {'n@1%':>5} {'net$@1%':>9} {'PF@1%':>6} | {'n@2%':>5} {'net$@2%':>9} {'PF@2%':>6}")
    for ig in (2.0, 3.0, 4.0):
        for lb in (2, 3):
            row = []
            for slip in (1.0, 2.0):
                allt = []
                for (s, d), (db, pct) in data.items():
                    allt += run_long(db, 100.0, slip, ig_pct=ig, lb=lb, maxent=2)
                row.append(stat(allt))
            (n1, net1, pf1, _), (n2, net2, pf2, _) = row
            print(f"{ig:4.1f} {lb:3d} | {n1:5d} {net1:9.0f} {pf1:6.2f} | {n2:5d} {net2:9.0f} {pf2:6.2f}")

    # ---- D: short leg ----
    print("\n=== D: EXHAUSTION SHORT (runup20 ext5 hod8, liq gate): trail x cap ===")
    print(f"{'trail':>5} {'cap':>4} | {'n@1%':>5} {'net$@1%':>9} {'PF@1%':>6} {'WR@1%':>5} | "
          f"{'n@2%':>5} {'net$@2%':>9} {'PF@2%':>6}")
    for trail in (2.0, 3.0, 4.0):
        for cap in (3, 6, 12):
            row = []
            for slip in (1.0, 2.0):
                allt = []
                for (s, d), (db, pct) in data.items():
                    allt += run_short(db, slip, trail=trail, cap=cap)
                row.append(stat(allt))
            (n1, net1, pf1, wr1), (n2, net2, pf2, _) = row
            print(f"{trail:5.1f} {cap:4d} | {n1:5d} {net1:9.0f} {pf1:6.2f} {wr1:5.0f} | "
                  f"{n2:5d} {net2:9.0f} {pf2:6.2f}")

    # ---- winner detail: per-tier + per-day for best gate config ----
    print("\n=== TIER DETAIL at deployed (gate as labelled, maxent2, slip 2%) ===")
    for label, gate in (("40", 40.0), ("60", 60.0), ("80", 80.0), ("100", 100.0)):
        per_tier = {}
        for (s, d), (db, pct) in data.items():
            tr = run_long(db, gate, 2.0, maxent=2)
            if not tr:
                continue
            tier = ("40-60" if pct < 60 else "60-80" if pct < 80 else
                    "80-100" if pct < 100 else ">=100")
            per_tier.setdefault(tier, []).extend(tr)
        parts = []
        for tier in ("40-60", "60-80", "80-100", ">=100"):
            n, net, pf, _ = stat(per_tier.get(tier, []))
            parts.append(f"{tier}: n={n} net={net:+.0f} PF={pf:.2f}")
        print(f"gate{label:>4}: " + "  |  ".join(parts))


if __name__ == "__main__":
    main()
