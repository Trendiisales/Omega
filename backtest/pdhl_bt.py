#!/usr/bin/env python3
"""
PDH/PDL Mean Reversion Backtest

Logic:
1. Track PDH/PDL (previous day high/low)
2. Only trade inside the range
3. SHORT when price in top 25% of range + drift > threshold (overbought stretch)
4. LONG when price in bottom 25% of range + drift < -threshold (oversold stretch)
5. TP = 50% back toward mid-range
6. SL = 0.4x ATR
7. Max hold = 15min

This is NOT a new idea -- it's the only thing the 2yr data proved works.
"""

import sys, math, collections
from dataclasses import dataclass
from typing import List

DATA = sys.argv[1] if len(sys.argv) > 1 else "/Users/jo/Tick/2yr_XAUUSD_tick.csv"
SPREAD   = 0.25   # realistic London/NY spread
SLIP     = 0.05   # slippage
COST     = SPREAD + SLIP * 2
RISK_USD = 30.0
USD_PER_PT = 100.0

def make_ewm(hl):
    s = {"v": None, "ts": None}
    def u(v, ts):
        if s["v"] is None: s["v"]=v; s["ts"]=ts; return v
        dt = max(ts-s["ts"], 0.001)
        a = 1 - math.exp(-dt * 0.693147 / hl)
        s["v"] = a*v + (1-a)*s["v"]; s["ts"]=ts; return s["v"]
    return u

@dataclass
class Trade:
    ts: int; side: str; entry: float; exit_px: float
    sl: float; tp: float; size: float
    pnl: float; reason: str; hold_s: int
    mfe: float; mae: float

def size_lot(sl_pts):
    if sl_pts <= 0: return 0.01
    s = RISK_USD / (sl_pts * USD_PER_PT)
    s = math.floor(s/0.001)*0.001
    return max(0.01, min(0.20, s))

# Test multiple parameter combinations
configs = [
    # (entry_pct, drift_min, sl_atr, tp_frac, label)
    (0.20, 1.0, 0.4, 0.5, "20pct_d1.0_sl0.4_tp0.5"),
    (0.20, 1.5, 0.4, 0.5, "20pct_d1.5_sl0.4_tp0.5"),
    (0.20, 2.0, 0.4, 0.5, "20pct_d2.0_sl0.4_tp0.5"),
    (0.25, 1.0, 0.4, 0.5, "25pct_d1.0_sl0.4_tp0.5"),
    (0.25, 1.5, 0.4, 0.5, "25pct_d1.5_sl0.4_tp0.5"),
    (0.25, 2.0, 0.4, 0.5, "25pct_d2.0_sl0.4_tp0.5"),
    (0.25, 2.0, 0.6, 0.5, "25pct_d2.0_sl0.6_tp0.5"),
    (0.30, 2.0, 0.4, 0.5, "30pct_d2.0_sl0.4_tp0.5"),
    (0.25, 2.0, 0.4, 0.4, "25pct_d2.0_sl0.4_tp0.4"),
    (0.25, 3.0, 0.4, 0.5, "25pct_d3.0_sl0.4_tp0.5"),
    (0.20, 2.0, 0.4, 0.6, "20pct_d2.0_sl0.4_tp0.6"),
    # Session-filtered versions (London + NY only = UTC 07-17)
    (0.25, 2.0, 0.4, 0.5, "25pct_d2.0_LON_NY"),
    (0.20, 1.5, 0.4, 0.5, "20pct_d1.5_LON_NY"),
    (0.25, 1.5, 0.4, 0.5, "25pct_d1.5_LON_NY"),
]

print(f"Loading {DATA}...")

all_trades = {cfg[-1]: [] for cfg in configs}

# State per config
class State:
    def __init__(self):
        self.active = False
        self.is_long = False
        self.entry = 0.0
        self.sl = 0.0
        self.tp = 0.0
        self.size = 0.0
        self.entry_ts = 0
        self.mfe = 0.0
        self.mae = 0.0
        self.last_close = 0

states = {cfg[-1]: State() for cfg in configs}

ewm30 = make_ewm(30)
atr = 0.0

# PDH/PDL
cur_day = -1
today_hi = 0.0; today_lo = 1e9
prev_hi = 0.0;  prev_lo = 0.0

last_ts = 0
n = 0

with open(DATA) as f:
    for line in f:
        n += 1
        if n % 20_000_000 == 0: print(f"  {n//1_000_000}M rows...")
        line = line.strip()
        if not line: continue
        p = line.split(',')
        if len(p) < 4: continue
        try:
            bid=float(p[2]); ask=float(p[3])
        except: continue
        if bid<=0 or ask<=bid: continue
        try:
            ds=p[0]; ts_s=p[1]
            y=int(ds[:4]); mo=int(ds[4:6]); dy=int(ds[6:8])
            h=int(ts_s[:2]); mi=int(ts_s[3:5]); se=int(ts_s[6:8])
            if mo<=2: y-=1; mo+=12
            days=365*y+y//4-y//100+y//400+(153*mo+8)//5+dy-719469
            ts=(days*86400+h*3600+mi*60+se)
        except: continue

        mid = (bid+ask)*0.5
        if last_ts > 0 and ts - last_ts > 3600:
            ewm30 = make_ewm(30)
        last_ts = ts

        # Update ATR (tick-level EWM)
        if atr == 0: atr = ask - bid
        else: atr = 0.05*(ask-bid) + 0.95*atr

        # EWM drift
        em = ewm30(mid, ts)
        drift = mid - em

        # PDH/PDL
        day = ts // 86400
        if day != cur_day:
            if cur_day >= 0:
                prev_hi = today_hi
                prev_lo = today_lo if today_lo < 1e8 else 0.0
            today_hi = mid; today_lo = mid; cur_day = day
        else:
            if mid > today_hi: today_hi = mid
            if mid < today_lo: today_lo = mid

        if prev_hi <= 0 or prev_lo <= 0: continue
        pdh = prev_hi; pdl = prev_lo
        rng = pdh - pdl
        if rng < 5.0: continue  # skip thin days

        hour = (ts % 86400) // 3600
        lon_ny = (7 <= hour <= 17)

        # Inside daily range check
        if mid > pdh + 1.0 or mid < pdl - 1.0: continue

        for cfg in configs:
            entry_pct, drift_min, sl_atr, tp_frac, label = cfg
            session_ok = lon_ny if "LON_NY" in label else True
            if not session_ok: continue

            st = states[label]
            trades = all_trades[label]

            if st.active:
                m = (mid - st.entry) if st.is_long else (st.entry - mid)
                if m > st.mfe: st.mfe = m
                if m < st.mae: st.mae = m

                hold = ts - st.entry_ts
                sl_hit = (st.is_long and bid <= st.sl) or (not st.is_long and ask >= st.sl)
                tp_hit = (st.is_long and bid >= st.tp) or (not st.is_long and ask <= st.tp)
                time_exit = hold >= 900

                if sl_hit or tp_hit or time_exit:
                    ep = st.sl if sl_hit else (st.tp if tp_hit else (bid if st.is_long else ask))
                    pnl_pts = (ep - st.entry) if st.is_long else (st.entry - ep)
                    pnl_usd = pnl_pts * st.size * USD_PER_PT - COST * st.size * USD_PER_PT
                    reason = "SL" if sl_hit else ("TP" if tp_hit else "TIME")
                    trades.append(Trade(st.entry_ts, "L" if st.is_long else "S",
                        st.entry, ep, st.sl, st.tp, st.size,
                        pnl_usd, reason, hold, st.mfe, st.mae))
                    st.active = False
                    st.last_close = ts
                continue

            # Entry cooldown
            if ts - st.last_close < 120: continue
            if atr < 0.3: continue

            upper = pdh - rng * entry_pct
            lower = pdl + rng * entry_pct

            sl_pts = max(atr * sl_atr * 50, 0.30)  # atr is tick-level, scale up
            mid_range = (pdh + pdl) * 0.5

            # SHORT: price at top of range + drift > threshold
            if mid >= upper and drift > drift_min:
                ep = bid  # sell at bid
                sl_px = ep + sl_pts
                tp_px = max(mid_range, ep - rng * tp_frac)
                if tp_px >= ep: continue  # TP must be below entry for short
                sz = size_lot(sl_pts)
                st.active=True; st.is_long=False; st.entry=ep
                st.sl=sl_px; st.tp=tp_px; st.size=sz
                st.entry_ts=ts; st.mfe=0; st.mae=0

            # LONG: price at bottom of range + drift < -threshold
            elif mid <= lower and drift < -drift_min:
                ep = ask  # buy at ask
                sl_px = ep - sl_pts
                tp_px = min(mid_range, ep + rng * tp_frac)
                if tp_px <= ep: continue  # TP must be above entry for long
                sz = size_lot(sl_pts)
                st.active=True; st.is_long=True; st.entry=ep
                st.sl=sl_px; st.tp=tp_px; st.size=sz
                st.entry_ts=ts; st.mfe=0; st.mae=0

print(f"\nLoaded {n:,} rows\n")

# Results
print(f"{'Config':<35} {'N':>5} {'WR%':>6} {'Net':>10} {'Avg':>7} {'DD':>8} {'Pos/Mo':>7}")
print("─"*80)

results = []
for cfg in configs:
    label = cfg[-1]
    trades = all_trades[label]
    if not trades:
        continue
    pnls = [t.pnl for t in trades]
    wins = [p for p in pnls if p > 0]
    wr = len(wins)/len(pnls)*100
    net = sum(pnls)
    avg = net/len(pnls)
    peak = eq = dd = 0
    for t in sorted(trades, key=lambda x: x.ts):
        eq += t.pnl
        if eq > peak: peak = eq
        if peak - eq > dd: dd = peak - eq
    monthly = collections.defaultdict(float)
    for t in trades:
        mo = f"{1970+(t.ts//31557600):04d}-{((t.ts%31557600)//2592000)+1:02d}"
        monthly[mo] += t.pnl
    pos_mo = sum(1 for v in monthly.values() if v > 0)
    results.append((net, label, len(pnls), wr, net, avg, dd, pos_mo, len(monthly)))

results.sort(reverse=True)
for net, label, n, wr, net2, avg, dd, pos_mo, total_mo in results:
    print(f"{label:<35} {n:>5} {wr:>5.1f}% {net2:>+10.2f} {avg:>+7.2f} {dd:>8.2f} {pos_mo:>3}/{total_mo}")

# Detailed breakdown for best config
if results:
    best_label = results[0][1]
    trades = all_trades[best_label]
    pnls = [t.pnl for t in trades]
    monthly = collections.defaultdict(float)
    exits = collections.defaultdict(lambda: [0, 0.0])
    for t in trades:
        mo = f"{1970+(t.ts//31557600):04d}-{((t.ts%31557600)//2592000)+1:02d}"
        monthly[mo] += t.pnl
        exits[t.reason][0] += 1
        exits[t.reason][1] += t.pnl

    print(f"\n{'='*60}")
    print(f"BEST: {best_label}")
    wins = [p for p in pnls if p>0]
    losses = [p for p in pnls if p<0]
    print(f"Trades: {len(pnls)}  WR: {len(wins)/len(pnls)*100:.1f}%  Net: ${sum(pnls):.2f}")
    if wins and losses:
        print(f"Avg win: ${sum(wins)/len(wins):.2f}  Avg loss: ${sum(losses)/len(losses):.2f}  R:R: {abs(sum(wins)/len(wins)/(sum(losses)/len(losses))):.2f}")
    print("\nExit reasons:")
    for r, (cnt, pnl) in sorted(exits.items()):
        print(f"  {r:<10} n={cnt:4d}  ${pnl:+.2f}")
    print("\nMonthly P&L:")
    cum = 0
    for mo, v in sorted(monthly.items()):
        cum += v
        bar = '#'*int(v/50) if v>0 else '.'*int(abs(v)/50)
        print(f"  {mo}  {v:+8.2f}  cum={cum:+8.2f}  {bar[:40]}")

