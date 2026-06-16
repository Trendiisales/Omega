#!/usr/bin/env python3
"""gate_validate.py -- does the ATR-expansion regime gate improve the daily
gold bracket (13:00 + 14:00 UTC, OFFSET $2 / SL $2 / TP $50, 60-min hold)?

Compares BASELINE (fire every clock window) vs GATED (fire only when the
vol_regime_allow gate passes), causal, with a walk-forward split (each half
must improve, per BACKTEST_TRUTH discipline -- a gate that only helps pooled
is regime luck). Sweeps a few gate configs and prints the table.

Data: yfinance GC=F 1h. Within-bar fill order is SL-before-TP (conservative
for the $2 SL). This is bar-replay -> treat PF as a relative, optimism-biased
hint; the live MGC trades.ndjson is the truth.
"""
import sys, os
import numpy as np
import pandas as pd
import yfinance as yf

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from live.regime_gate import vol_regime_allow, GateCfg

OFFSET, TP, SL, HOLD_MIN = 2.0, 50.0, 2.0, 60

print("downloading GC=F 1h / 730d ...", flush=True)
gc = yf.download('GC=F', period='730d', interval='1h', progress=False, auto_adjust=False)
if isinstance(gc.columns, pd.MultiIndex):
    gc.columns = [c[0] for c in gc.columns]
gc = gc.reset_index()
gc.columns = [str(c).lower() for c in gc.columns]
tsc = next((c for c in ['datetime', 'date', 'index'] if c in gc.columns), gc.columns[0])
gc['ts'] = pd.to_datetime(gc[tsc], utc=True)
gc = gc.sort_values('ts').reset_index(drop=True)
gc['hour'] = gc['ts'].dt.hour
gc['dow'] = gc['ts'].dt.dayofweek
H = gc['high'].to_numpy(); L = gc['low'].to_numpy(); C = gc['close'].to_numpy()
TS = gc['ts'].to_numpy()
print(f"data: {len(gc)} rows  {gc['ts'].min()} -> {gc['ts'].max()}", flush=True)


def sim_one(open_ts, open_px):
    """Simulate one bracket from a clock-window open. Returns pnl, reason."""
    end = open_ts + pd.Timedelta(minutes=HOLD_MIN)
    win = gc[(gc['ts'] > open_ts) & (gc['ts'] <= end)]
    if win.empty:
        return None, 'NO_BARS'
    bt, st = open_px + OFFSET, open_px - OFFSET
    side = entry = None; eidx = None
    for idx, r in win.iterrows():
        if r['high'] >= bt: side, entry, eidx = 'BUY', bt, idx; break
        if r['low'] <= st:  side, entry, eidx = 'SELL', st, idx; break
    if side is None:
        return 0.0, 'NO_TRIG'
    tp_px = entry + TP if side == 'BUY' else entry - TP
    sl_px = entry - SL if side == 'BUY' else entry + SL
    for _, r in win.loc[eidx:].iterrows():
        if side == 'BUY':
            if r['low'] <= sl_px:  return -SL, 'SL'   # SL checked first (conservative)
            if r['high'] >= tp_px: return TP, 'TP'
        else:
            if r['high'] >= sl_px: return -SL, 'SL'
            if r['low'] <= tp_px:  return TP, 'TP'
    cp = win.iloc[-1]['close']
    return ((cp - entry) if side == 'BUY' else (entry - cp)), 'TIME'


# decision rows: first bar of hour 13 and 14 on weekdays
opens = gc[(gc['hour'].isin([13, 14])) & (gc['dow'] < 5)].copy()
print(f"clock-window opens: {len(opens)}", flush=True)

# precompute each open's trade + its causal gate decision under a given cfg
def run(cfg: GateCfg):
    rows = []
    for idx, op in opens.iterrows():
        # causal: bars strictly BEFORE this open bar
        h, l, c = H[:idx], L[:idx], C[:idx]
        allow, info = vol_regime_allow(list(h), list(l), list(c), cfg)
        pnl, reason = sim_one(op['ts'], op['open'])
        if pnl is None:
            continue
        rows.append({'ts': op['ts'], 'pnl': pnl, 'reason': reason,
                     'allow': allow, 'ratio': info.get('ratio')})
    return pd.DataFrame(rows)


def stats(df):
    tr = df[df['reason'] != 'NO_TRIG']
    if tr.empty:
        return None
    wins = tr[tr['pnl'] > 0]; loss = tr[tr['pnl'] <= 0]
    pos = wins['pnl'].sum(); neg = abs(loss['pnl'].sum())
    pf = pos / neg if neg > 0 else 99.0
    cum = tr['pnl'].cumsum().to_numpy()
    dd = float((cum - np.maximum.accumulate(cum)).min()) if len(cum) else 0.0
    return dict(n_open=len(df), n_trade=len(tr), win=len(wins) / len(tr) * 100,
                sum=tr['pnl'].sum(), pf=pf, dd=dd,
                avg_w=pos / len(wins) if len(wins) else 0,
                avg_l=-neg / len(loss) if len(loss) else 0)


def show(tag, df):
    s = stats(df)
    if not s:
        print(f"{tag:<28} (no trades)"); return s
    print(f"{tag:<28} n={s['n_trade']:>4} win={s['win']:5.1f}% "
          f"sum=${s['sum']:>9.1f} PF={s['pf']:5.2f} DD=${s['dd']:>8.1f} "
          f"avgW=${s['avg_w']:.0f} avgL=${s['avg_l']:.1f}")
    return s


# split date for walk-forward (median open date)
mid_ts = opens['ts'].quantile(0.5)
print(f"\nwalk-forward split @ {mid_ts}\n", flush=True)

# baseline once (gate-independent)
base = run(GateCfg(k_expand=0.0))   # k_expand 0 => always allow == baseline
print("=== BASELINE (fire every window) ===")
show("baseline ALL", base)
show("  baseline H1", base[base['ts'] < mid_ts])
show("  baseline H2", base[base['ts'] >= mid_ts])

print("\n=== GATED sweep (fire only when ATR expanded) ===")
print(f"{'cfg':<28}{'(gated trades only)'}")
for atr_n in (24, 48):
    for k in (0.90, 1.00, 1.15, 1.30):
        cfg = GateCfg(atr_n=atr_n, k_expand=k)
        df = run(cfg)
        g = df[df['allow']]
        tag = f"atr_n={atr_n} k={k:.2f}"
        s = show(tag, g)
        if s:
            h1 = stats(g[g['ts'] < mid_ts]); h2 = stats(g[g['ts'] >= mid_ts])
            bh = ""
            if h1 and h2:
                bh = (f"   H1 PF={h1['pf']:.2f} sum=${h1['sum']:.0f} | "
                      f"H2 PF={h2['pf']:.2f} sum=${h2['sum']:.0f}")
            skipped = df[~df['allow']]
            sk = stats(skipped)
            skinfo = f"   SKIPPED: n={sk['n_trade']} sum=${sk['sum']:.0f} PF={sk['pf']:.2f}" if sk else "   SKIPPED: none"
            print(bh)
            print(skinfo)
