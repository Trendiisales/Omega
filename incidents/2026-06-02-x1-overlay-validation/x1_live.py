#!/usr/bin/env python3
"""
x1_live.py — Mac-local live X1 overlay & interpreter (GOLD-FIRST)
=================================================================
Runs the gold-validated X1 momentum-confirm overlay on the Mac in REAL TIME,
driven by the Omega GUI's live feed.

Live feed (the key bit)
-----------------------
The Omega GUI telemetry (/api/telemetry, pushed ~250ms) carries top-level live
top-of-book for every symbol: gold_bid / gold_ask (also sp_bid/ask, nq_bid/ask,
eurusd_bid/ask, ...) plus xau_curh/curl/pdh/pdl. This is a CONTINUOUS live price
— available whether or not a trade is open. We poll it, take mid=(bid+ask)/2,
and aggregate the stream into M1 bars on the Mac in real time.

History is built FROM THE FEED. Live M1 bars are persisted to a local cache
(_x1_live_cache_XAUUSD_m1.csv) so warmup accumulates across restarts — no
Dukascopy needed. WaveTrend needs ~55 bars; on a first-ever run with no cache it
warms up from the feed over ~55 min. Use --seed-dukascopy once to skip that.

Scope: XAUUSD only. The confirm-filter edge is gold-specific
(X1_MULTISYMBOL_FINDINGS.md). Do NOT extend to indices.

Interpretation printed
----------------------
  - Current regime (EMA21 vs EMA55) + WaveTrend wt1/wt2 + last tag, on the live bar.
  - "Would a long/short entry RIGHT NOW be momentum-confirmed?" + the validated
    gold base rate (confirmed trend winners 71.9% vs losers 51.5%; +12pp
    within-trend — X1_STAGE1_FINDINGS.md).
  - For each OPEN gold trade (from telemetry live_trades): was a confirming
    momentum tag present in the `lookback` bars before entry? -> CONFIRMED /
    UNCONFIRMED, with live P&L, flagging non-trend engines.

Read-only. Never sends orders. Touches no core/engine code.

Usage
-----
  # LIVE GRAPHICAL DASHBOARD (auto-refreshing chart + interpretation in browser):
  python3 x1_live.py --serve --gui-url http://185.167.119.59:7779
  #   -> opens http://localhost:8089, chart + WaveTrend + tags + read, redraws each poll
  #   history builds from the feed into a local cache; NO Dukascopy required.
  #   first-ever run warms ~55 min; to skip it once: add --seed-dukascopy

  python3 x1_live.py --loop --gui-url http://185.167.119.59:7779   # text console, real-time
"""

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
import threading
import time
import http.server
import socketserver
import urllib.request
import pandas as pd

import x1_validate as X
from x1_stage1 import family_of

SYMBOL = "XAUUSD"
DUKAS_SCALE = 1000
BASE_RATE = ("gold confirmed-trend winners 71.9% vs losers 51.5% "
             "(+12pp within-trend, ~2 SE)")


# --------------------------------------------------------------------------- #
# GUI telemetry
# --------------------------------------------------------------------------- #
def fetch_gui(gui_url, timeout=4):
    try:
        req = urllib.request.Request(gui_url.rstrip("/") + "/api/telemetry",
                                     headers={"User-Agent": "x1_live"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception as e:
        print(f"[gui] unreachable ({e})", file=sys.stderr)
        return None


def gold_mid(tele):
    """Live gold mid from top-level gold_bid/gold_ask. None if absent."""
    if not tele:
        return None
    b, a = tele.get("gold_bid"), tele.get("gold_ask")
    try:
        b, a = float(b), float(a)
    except (TypeError, ValueError):
        return None
    if b <= 0 or a <= 0:
        return None
    return (b + a) / 2.0


def gold_open_trades(tele):
    if not tele:
        return []
    return [t for t in tele.get("live_trades", [])
            if SYMBOL in str(t.get("symbol", ""))]


def all_open_trades(tele):
    """Every currently-running position (all symbols), from live_trades."""
    if not tele:
        return []
    return list(tele.get("live_trades", []))


# --------------------------------------------------------------------------- #
# Live M1 bar builder — aggregates polled mids into minute OHLC.
# Persists across loop iterations.
# --------------------------------------------------------------------------- #
class LiveBars:
    def __init__(self):
        self.bars = {}   # minute_floor_ts(UTC, sec) -> [o,h,l,c]

    def push(self, mid, now_utc):
        m = int(now_utc.timestamp() // 60 * 60)
        b = self.bars.get(m)
        if b is None:
            self.bars[m] = [mid, mid, mid, mid]
        else:
            b[1] = max(b[1], mid)
            b[2] = min(b[2], mid)
            b[3] = mid

    def frame(self):
        if not self.bars:
            return pd.DataFrame(columns=["open", "high", "low", "close"])
        idx = pd.to_datetime(sorted(self.bars), unit="s", utc=True)
        rows = [self.bars[int(t.timestamp())] for t in idx]
        df = pd.DataFrame(rows, columns=["open", "high", "low", "close"], index=idx)
        return df


# --------------------------------------------------------------------------- #
# History source — the GUI feed itself, persisted to a local cache.
# No Dukascopy by default: the app builds its own M1 history from the live feed
# and writes it to CACHE_PATH so warmup persists across restarts. Dukascopy is
# an OPTIONAL one-time bootstrap (--seed-dukascopy) to avoid the first-run warmup.
# --------------------------------------------------------------------------- #
def cache_path(here):
    return os.path.join(here, "_x1_live_cache_XAUUSD_m1.csv")


def load_seed(args, here):
    """Initial history. Priority: explicit --bars > Dukascopy bootstrap > local
    cache of previously-accumulated live bars > empty (warm up from the feed)."""
    if args.bars and os.path.exists(args.bars):
        print(f"[seed] explicit bars {args.bars}", file=sys.stderr)
        return X.load_bars(args.bars)
    if args.seed_dukascopy:
        path = cache_path(here)
        to = dt.datetime.now(dt.timezone.utc).date()
        frm = to - dt.timedelta(days=3)
        pull = os.path.join(here, "pull_dukascopy.py")
        print(f"[seed] one-time Dukascopy bootstrap {frm}..{to}", file=sys.stderr)
        subprocess.run([sys.executable, pull, "--symbol", "XAUUSD",
                        "--from", frm.isoformat(), "--to", to.isoformat(),
                        "--scale", str(DUKAS_SCALE), "--out", path, "--workers", "16"],
                       cwd=here, check=False)
        if os.path.exists(path):
            return X.load_bars(path)
    cp = cache_path(here)
    if os.path.exists(cp):
        print(f"[seed] local live-feed cache {cp}", file=sys.stderr)
        return X.load_bars(cp)
    print("[seed] no history — warming up from the live feed (no Dukascopy)",
          file=sys.stderr)
    return pd.DataFrame(columns=["open", "high", "low", "close"])


def save_cache(b, here, keep=6000):
    """Persist the merged bar history (live-feed sourced) so it survives restarts."""
    try:
        out = b[["open", "high", "low", "close"]].tail(keep).copy()
        out.insert(0, "ts_utc", out.index.strftime("%Y-%m-%dT%H:%M:%SZ"))
        out.to_csv(cache_path(here), index=False)
    except Exception as e:
        print(f"[cache] write failed: {e}", file=sys.stderr)


def merge(seed, live):
    """Concatenate seed history with live-built bars; live overrides on overlap."""
    if live.empty:
        return seed
    if seed.empty:
        return live
    cut = live.index[0]
    return pd.concat([seed[seed.index < cut], live]).sort_index()


# --------------------------------------------------------------------------- #
# Interpretation
# --------------------------------------------------------------------------- #
def confirm_at(b, entry_dt, lookback, side):
    idx = b.index
    if entry_dt < idx[0] or entry_dt > idx[-1]:
        return None
    pos = idx.searchsorted(entry_dt, side="right") - 1
    if pos < 0:
        return None
    win = b.iloc[max(0, pos - lookback):pos + 1]
    if side > 0:
        return bool(win["momentum_up"].any())
    if side < 0:
        return bool(win["momentum_down"].any())
    return None


def interpret(b, lookback):
    last = b.iloc[-1]
    tag = next((t for t in ("momentum_up", "momentum_down", "retr_down", "retr_up")
                if bool(last.get(t, False))), None)
    lb = b.iloc[-lookback:]
    return dict(regime_up=bool(last["regime_up"]),
                wt1=float(last["wt1"]), wt2=float(last["wt2"]),
                last_tag=tag, price=float(last["close"]), ts=b.index[-1],
                long_confirmed=bool(lb["momentum_up"].any()),
                short_confirmed=bool(lb["momentum_down"].any()))


def render(state, trades, b, lookback, src):
    print("=" * 72)
    print(f"X1 LIVE — XAUUSD  {state['ts']:%Y-%m-%d %H:%M}Z   "
          f"price={state['price']:.2f}   [{src}]")
    print("=" * 72)
    print(f"  Regime : {'UP (EMA21>EMA55)' if state['regime_up'] else 'DOWN (EMA21<EMA55)'}")
    print(f"  WaveTrend: wt1={state['wt1']:+6.1f} wt2={state['wt2']:+6.1f} "
          f"({'bull' if state['wt1']>=state['wt2'] else 'bear'})")
    print(f"  Last tag : {state['last_tag'] or '-'}")
    print("  --- confirm-filter read (gold-validated) ---")
    print(f"  LONG  entry now: {'CONFIRMED' if state['long_confirmed'] else 'unconfirmed'} "
          f"(momentum_up in last {lookback} bars)")
    print(f"  SHORT entry now: {'CONFIRMED' if state['short_confirmed'] else 'unconfirmed'} "
          f"(momentum_down in last {lookback} bars)")
    print(f"  base rate: {BASE_RATE}")
    print("  --- open gold trades (live_trades) ---")
    if not trades:
        print("    (none open)")
    for t in trades:
        side_s = str(t.get("side", "")).upper()
        side = 1 if side_s in ("LONG", "BUY") else (-1 if side_s in ("SHORT", "SELL") else 0)
        eng = t.get("engine", "?")
        fam = family_of(eng)
        held = float(t.get("held_sec", 0) or 0)
        pnl = float(t.get("live_pnl", 0) or 0)
        entry_dt = state["ts"] - pd.Timedelta(seconds=held)
        conf = confirm_at(b, entry_dt, lookback, side)
        verdict = ("CONFIRMED" if conf else "UNCONFIRMED") if conf is not None \
                  else "n/a (entry pre-window)"
        flag = "" if fam == "trend" else f"  [filter validated for TREND; engine is '{fam}']"
        print(f"    {eng:24} {side_s:5} held={held/60:5.0f}m pnl={pnl:+8.2f}  "
              f"entry-confirm: {verdict}{flag}")


def open_rows(open_all):
    """Normalize all-symbol open positions into display dicts (running trades)."""
    rows = []
    for t in open_all:
        try:
            entry = float(t.get("entry", 0) or 0)
            cur = float(t.get("current", 0) or 0)
            tp = float(t.get("tp", 0) or 0)
            sl = float(t.get("sl", 0) or 0)
            pnl = float(t.get("live_pnl", 0) or 0)
            held = float(t.get("held_sec", 0) or 0)
            side = str(t.get("side", "")).upper()
            rng = abs(tp - sl) if tp and sl else 0.0
            # progress toward TP (0..1) along the entry->tp path
            prog = None
            if tp and entry and (tp - entry) != 0:
                prog = max(0.0, min(1.0, (cur - entry) / (tp - entry)))
            rows.append(dict(symbol=t.get("symbol", "?"), engine=t.get("engine", "?"),
                             side=side, entry=round(entry, 4), current=round(cur, 4),
                             tp=round(tp, 4), sl=round(sl, 4), pnl=round(pnl, 2),
                             held_min=round(held / 60), to_tp=round(float(t.get("dist_tp", 0) or 0), 2),
                             to_sl=round(float(t.get("dist_sl", 0) or 0), 2),
                             prog=None if prog is None else round(prog * 100)))
        except Exception:
            continue
    return rows


def render_open(open_all):
    rows = open_rows(open_all)
    print("  --- RUNNING TRADES (all symbols) ---")
    if not rows:
        print("    (no open positions — all flat)")
    for r in rows:
        print(f"    {r['symbol']:8} {r['engine']:22} {r['side']:5} "
              f"entry={r['entry']:.2f} cur={r['current']:.2f} "
              f"pnl={r['pnl']:+8.2f} held={r['held_min']}m "
              f"tp_in={r['to_tp']} sl_in={r['to_sl']}")


# --------------------------------------------------------------------------- #
def step(args, here, live, seed_cache):
    tele = None if args.no_gui else fetch_gui(args.gui_url)
    now = dt.datetime.now(dt.timezone.utc)
    src = "offline"
    mid = gold_mid(tele)
    if mid is not None:
        live.push(mid, now)
        src = "GUI-live"

    seed = seed_cache.get("seed")
    if seed is None:
        seed = load_seed(args, here)
        seed_cache["seed"] = seed
    b = merge(seed, live.frame())
    need = max(X.DEFAULTS["ema_slow"], X.DEFAULTS["wt_n2"]) + 5
    if len(b) < need:
        save_cache(b, here)
        msg = (f"[warmup] {len(b)}/{need} bars from live feed "
               f"(~{need-len(b)} min to go; runs continuously, cache persists)")
        print(msg, file=sys.stderr)
        LATEST["err"] = msg if getattr(args, "serve", 0) else LATEST["err"]
        return
    b = X.compute_tags(b, dict(X.DEFAULTS))
    save_cache(b, here)   # persist live-feed history every poll

    trades = gold_open_trades(tele)
    if not trades and args.trades and os.path.exists(args.trades) and args.no_gui:
        tr = X.load_trades(args.trades, SYMBOL)
        tr = tr[(tr["net_pnl"] != 0) & (tr["entry_dt"] >= b.index[0])]
        trades = [dict(symbol=SYMBOL, engine=r["engine"], side=r["side"],
                       held_sec=(r["exit_dt"] - r["entry_dt"]).total_seconds(),
                       live_pnl=r["net_pnl"]) for _, r in tr.tail(8).iterrows()]

    open_all = all_open_trades(tele)
    state = interpret(b, args.lookback)
    src_lbl = src if src != "offline" else "Dukascopy"
    if not getattr(args, "serve", 0):
        render(state, trades, b, args.lookback, src_lbl)
        render_open(open_all)
    if args.chart:
        X.plot(b.iloc[-args.plot_bars:], None, dict(X.DEFAULTS), args.chart, SYMBOL)
    return state, trades, b, src_lbl, open_all


# --------------------------------------------------------------------------- #
# Live web dashboard (--serve) — the graphical output, auto-refreshing.
# A background thread polls the feed, redraws the chart PNG, and publishes the
# interpretation as JSON; the page auto-reloads both every `interval` seconds.
# --------------------------------------------------------------------------- #
LATEST = {"png": b"", "state": None, "trades": [], "open_all": [], "src": "-",
          "ts": "", "err": None, "rev": 0}
_CHART_PATH = "/tmp/_x1_live_chart.png"


def draw_chart(b, cfg, out_png, symbol, live_price, updated):
    """Live chart: candles + WaveTrend + tags PLUS a moving live-price line and a
    bright dot at the right edge that visibly shift every poll, and a clock in the
    title — so liveness is obvious even when the M1 candle barely moves."""
    plt, Rect = X.plt, X.Rectangle
    n = len(b)
    import numpy as np
    x = np.arange(n)
    fig, (ax, axo) = plt.subplots(
        2, 1, figsize=(min(34, max(12, n * 0.05)), 8), sharex=True,
        gridspec_kw={"height_ratios": [3, 1]})
    fig.patch.set_facecolor("#0b0e11")
    for a in (ax, axo):
        a.set_facecolor("#0b0e11")
        a.tick_params(colors="#aaa")
        for sp in a.spines.values():
            sp.set_color("#333")
    o, h, l, c = (b["open"].values, b["high"].values, b["low"].values, b["close"].values)
    w = 0.6
    for i in range(n):
        col = X.C_UP if c[i] >= o[i] else X.C_DOWN
        ax.vlines(x[i], l[i], h[i], color=col, linewidth=0.6)
        lo_, hi_ = min(o[i], c[i]), max(o[i], c[i])
        ax.add_patch(Rect((x[i] - w / 2, lo_), w, max(hi_ - lo_, 1e-9),
                          facecolor=col, edgecolor=col))
    rng = (np.nanmax(h) - np.nanmin(l)) or 1.0
    off = rng * 0.012

    def mark(mask, dy, marker, color, label):
        ix = np.where(b[mask].fillna(False).values)[0]
        if len(ix):
            yy = (l[ix] - off) if dy < 0 else (h[ix] + off)
            ax.scatter(ix, yy, marker=marker, c=color, s=42, zorder=5, label=label)
    mark("momentum_up", -1, "^", X.C_MOMUP, "Momentum Up")
    mark("momentum_down", +1, "v", X.C_MOMDN, "Momentum Down")
    mark("retr_down", +1, "x", X.C_RETR, "Possible Retr. Down")
    mark("retr_up", -1, "+", X.C_RETR, "Possible Retr. Up")

    # LIVE price line + moving dot (the visibly-updating-every-poll element)
    lp = float(live_price if live_price is not None else c[-1])
    ax.axhline(lp, color="#ffd54f", lw=0.8, ls="--", alpha=0.8, zorder=6)
    ax.scatter([n - 1], [lp], s=90, c="#ffd54f", edgecolors="#000",
               linewidths=0.6, zorder=7)
    ax.annotate(f"{lp:.2f}", (n - 1, lp), color="#0b0e11", fontsize=9,
                fontweight="bold", xytext=(4, 0), textcoords="offset points",
                ha="left", va="center",
                bbox=dict(boxstyle="round,pad=0.2", fc="#ffd54f", ec="none"))
    ax.set_title(f"{symbol}  {lp:.2f}   live · updated {updated}",
                 color="#ffd54f", fontsize=12)
    ax.legend(loc="upper left", facecolor="#15191e", edgecolor="#333",
              labelcolor="#ccc", fontsize=8)

    axo.plot(x, b["wt1"].values, color=X.C_WT1, lw=1.0, label="wt1")
    axo.plot(x, b["wt2"].values, color=X.C_WT2, lw=0.9, label="wt2")
    axo.fill_between(x, b["wt1"].values, b["wt2"].values,
                     where=(b["wt1"] >= b["wt2"]).values, color=X.C_UP, alpha=0.25)
    axo.fill_between(x, b["wt1"].values, b["wt2"].values,
                     where=(b["wt1"] < b["wt2"]).values, color=X.C_DOWN, alpha=0.25)
    for lvl in (cfg["wt_ob1"], cfg["wt_ob2"], cfg["wt_os1"], cfg["wt_os2"]):
        axo.axhline(lvl, color="#555", lw=0.6, ls="--")
    axo.axhline(0, color="#777", lw=0.5)
    axo.set_ylabel("WaveTrend", color="#aaa")
    ticks = np.linspace(0, n - 1, min(10, n)).astype(int)
    axo.set_xticks(ticks)
    axo.set_xticklabels([b.index[i].strftime("%m-%d %H:%M") for i in ticks],
                        rotation=30, ha="right", fontsize=8)
    plt.tight_layout()
    fig.savefig(out_png, dpi=110, facecolor=fig.get_facecolor())
    plt.close(fig)


def _trade_rows(trades, b, lookback, ts):
    rows = []
    for t in trades:
        side_s = str(t.get("side", "")).upper()
        side = 1 if side_s in ("LONG", "BUY") else (-1 if side_s in ("SHORT", "SELL") else 0)
        eng = t.get("engine", "?")
        fam = family_of(eng)
        held = float(t.get("held_sec", 0) or 0)
        pnl = float(t.get("live_pnl", 0) or 0)
        conf = confirm_at(b, ts - pd.Timedelta(seconds=held), lookback, side)
        verdict = ("CONFIRMED" if conf else "UNCONFIRMED") if conf is not None else "n/a"
        rows.append(dict(engine=eng, side=side_s, fam=fam,
                         held_min=round(held / 60), pnl=round(pnl, 2),
                         verdict=verdict, trend=(fam == "trend")))
    return rows


def serve_worker(args, here):
    live, seed_cache = LiveBars(), {}
    while True:
        try:
            res = step(args, here, live, seed_cache)
            if res:
                state, trades, b, src, open_all = res
                updated = dt.datetime.now(dt.timezone.utc).strftime("%H:%M:%S") + "Z"
                draw_chart(b.iloc[-args.plot_bars:], dict(X.DEFAULTS),
                           _CHART_PATH, SYMBOL, state["price"], updated)
                with open(_CHART_PATH, "rb") as fh:
                    png = fh.read()
                LATEST.update(png=png, state=state, src=src, err=None,
                              ts=f"{state['ts']:%Y-%m-%d %H:%M}Z",
                              updated=updated, rev=LATEST["rev"] + 1,
                              open_all=open_rows(open_all),
                              trades=_trade_rows(trades, b, args.lookback, state["ts"]))
        except Exception as e:
            LATEST["err"] = str(e)
            print(f"[serve] {e}", file=sys.stderr)
        time.sleep(max(1, args.interval))


def build_page(interval):
    return f"""<!doctype html><html><head><meta charset=utf-8>
<title>X1 Live — XAUUSD</title>
<style>
 body{{background:#0b0e11;color:#cfd3d8;font:13px/1.5 -apple-system,Menlo,monospace;margin:0;padding:14px}}
 h1{{font-size:15px;margin:0 0 8px;color:#eaecef}}
 #img{{width:100%;min-height:420px;background:#0b0e11;border:1px solid #222;border-radius:6px;display:block}}
 #wait{{color:#7f8a96;padding:8px 2px}}
 .panel{{display:flex;gap:18px;margin-top:10px;flex-wrap:wrap}}
 .card{{background:#15191e;border:1px solid #2a2f36;border-radius:6px;padding:10px 14px}}
 .k{{color:#7f8a96}} .up{{color:#26a69a}} .dn{{color:#ef5350}} .on{{color:#00e676;font-weight:600}} .off{{color:#7f8a96}}
 table{{border-collapse:collapse;font-size:12px}} td,th{{padding:3px 8px;text-align:left;border-bottom:1px solid #20242a}}
 .flag{{color:#ffb74d}} .stale{{color:#ef5350}}
</style></head><body>
<h1>X1 Live Overlay — XAUUSD <span class=k>(gold-validated · read-only)</span>
 <span id=hb class=k style="float:right;font-size:12px">connecting…</span></h1>
<div id=wait>waiting for first chart render (seed warmup)…</div>
<img id=img style="display:none" onload="this.style.display='block';document.getElementById('wait').style.display='none'">
<div id=running></div>
<div class=panel id=panel></div>
<script>
const IV={interval*1000};
let npoll=0;
async function tick(){{
 npoll++;
 const im=document.getElementById('img');
 const probe=new Image();
 probe.onload=()=>{{im.src=probe.src;}};
 probe.src='/chart.png?t='+Date.now();
 const hb=document.getElementById('hb');
 const clk=new Date().toLocaleTimeString();
 try{{
  const s=await (await fetch('/state?t='+Date.now())).json();
  hb.innerHTML='● live · client '+clk+' · server '+(s.updated||'-')+' · rev '+(s.rev||0)+' · poll '+npoll;
  hb.style.color='#26a69a';
  // RUNNING TRADES (all symbols) — what's open right now
  const oa=s.open_all||[];
  let rt='<div class=card style="width:100%"><div class=k>RUNNING TRADES (all symbols) · '+oa.length+' open</div>';
  if(oa.length){{
   rt+='<table style="width:100%"><tr><th>symbol</th><th>engine</th><th>side</th><th>entry</th><th>current</th><th>P&L</th><th>held</th><th>→TP</th><th>→SL</th></tr>';
   for(const t of oa){{
    const sd=t.side==='LONG'||t.side==='BUY'?'up':'dn';
    rt+='<tr><td>'+t.symbol+'</td><td>'+t.engine+'</td><td class='+sd+'>'+t.side+'</td><td>'+t.entry+'</td><td>'+t.current+'</td><td class='+(t.pnl>=0?'up':'dn')+'>'+(t.pnl>=0?'+':'')+t.pnl+'</td><td>'+t.held_min+'m</td><td>'+t.to_tp+'</td><td>'+t.to_sl+'</td></tr>';
   }}
   rt+='</table>';
  }} else {{ rt+='<div style="color:#7f8a96;padding:4px 0">no open positions — all flat</div>'; }}
  rt+='</div>';
  document.getElementById('running').innerHTML=rt;
  if(s.err){{document.getElementById('panel').innerHTML='<div class=card stale>error: '+s.err+'</div>';return;}}
  const st=s.state||{{}};
  const reg=st.regime_up?'<span class=up>UP</span>':'<span class=dn>DOWN</span>';
  const lc=st.long_confirmed?'<span class=on>CONFIRMED</span>':'<span class=off>unconfirmed</span>';
  const sc=st.short_confirmed?'<span class=on>CONFIRMED</span>':'<span class=off>unconfirmed</span>';
  let tr='';
  if((s.trades||[]).length){{tr='<table><tr><th>engine</th><th>side</th><th>held</th><th>pnl</th><th>entry-confirm</th></tr>';
   for(const t of s.trades){{tr+='<tr><td>'+t.engine+(t.trend?'':' <span class=flag>['+t.fam+']</span>')+'</td><td>'+t.side+'</td><td>'+t.held_min+'m</td><td class='+(t.pnl>=0?'up':'dn')+'>'+t.pnl+'</td><td>'+t.verdict+'</td></tr>';}}
   tr+='</table>';}} else {{tr='<span class=k>(none open)</span>';}}
  document.getElementById('panel').innerHTML=
   '<div class=card><div class=k>'+s.ts+' · '+s.src+'</div><div style="font-size:20px">'+(st.price||0).toFixed(2)+'</div>'
   +'<div>regime '+reg+' · WT '+(st.wt1||0).toFixed(1)+'/'+(st.wt2||0).toFixed(1)+'</div>'
   +'<div>tag: '+(st.last_tag||'-')+'</div></div>'
   +'<div class=card><div class=k>confirm-filter read</div><div>LONG now: '+lc+'</div><div>SHORT now: '+sc+'</div>'
   +'<div class=k style="margin-top:6px;max-width:320px">base rate: gold confirmed-trend winners 71.9% vs losers 51.5% (+12pp within-trend)</div></div>'
   +'<div class=card><div class=k>open gold trades</div>'+tr+'</div>';
 }}catch(e){{}}
}}
tick();setInterval(tick,IV);
</script></body></html>"""


def run_server(args, here):
    here_ref = here
    page = build_page(args.interval).encode()

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, ctype, body):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.startswith("/chart.png"):
                self._send(200, "image/png", LATEST["png"] or b"")
            elif self.path.startswith("/state"):
                st = LATEST["state"]
                payload = dict(
                    err=LATEST["err"], src=LATEST["src"], ts=LATEST["ts"],
                    updated=LATEST.get("updated", ""), rev=LATEST["rev"],
                    trades=LATEST["trades"], open_all=LATEST["open_all"],
                    state=None if st is None else dict(
                        regime_up=st["regime_up"], wt1=st["wt1"], wt2=st["wt2"],
                        last_tag=st["last_tag"], price=st["price"],
                        long_confirmed=st["long_confirmed"],
                        short_confirmed=st["short_confirmed"]))
                self._send(200, "application/json", json.dumps(payload).encode())
            else:
                self._send(200, "text/html", page)

    # HTTP server in the background thread; the poll+chart-render loop runs on the
    # MAIN thread. matplotlib's pyplot state machine (X.plot) is NOT thread-safe,
    # so it must stay on the main thread or the PNG silently fails to render.
    socketserver.TCPServer.allow_reuse_address = True
    try:
        srv = socketserver.TCPServer(("127.0.0.1", args.serve), H)
    except OSError as e:
        # Port already in use -> another instance is already serving. Exit CLEANLY
        # (0) so a launchd KeepAlive does NOT crash-loop and spam browser tabs.
        print(f"[x1_live] port {args.serve} already in use ({e}); another instance "
              f"is serving. Exiting cleanly.", file=sys.stderr)
        sys.exit(0)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://localhost:{args.serve}"
    print(f"[x1_live] dashboard at {url}  (feed {args.gui_url}, refresh {args.interval}s)",
          file=sys.stderr)
    if not args.no_open:          # the persistent agent runs with --no-open (no tab spam)
        try:
            subprocess.run(["open", url], check=False)
        except Exception:
            pass
    serve_worker(args, here_ref)   # blocks on main thread


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="Mac-local live X1 gold overlay")
    ap.add_argument("--gui-url", default="http://localhost:7779")
    ap.add_argument("--no-gui", action="store_true")
    ap.add_argument("--bars", default=None, help="explicit seed bars CSV (one-off)")
    ap.add_argument("--seed-dukascopy", action="store_true",
                    help="one-time Dukascopy bootstrap to skip first-run warmup (off by default)")
    ap.add_argument("--no-refresh", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--hours", type=int, default=0, help=argparse.SUPPRESS)
    ap.add_argument("--lookback", type=int, default=X.DEFAULTS["lookback"])
    ap.add_argument("--trades", default=os.path.expanduser("~/Downloads/omega_trade_closes.csv"))
    ap.add_argument("--chart", default=None)
    ap.add_argument("--plot-bars", dest="plot_bars", type=int, default=400)
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--serve", type=int, nargs="?", const=8089, default=0,
                    metavar="PORT", help="run live web dashboard on PORT (default 8089)")
    ap.add_argument("--no-open", action="store_true",
                    help="don't auto-open the browser (use for the persistent agent)")
    ap.add_argument("--interval", type=int, default=5, help="poll seconds (GUI pushes ~250ms)")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(line_buffering=True)  # stream cleanly when piped
    except Exception:
        pass

    if args.serve:
        run_server(args, here)
        return

    live, seed_cache = LiveBars(), {}
    if not args.loop:
        step(args, here, live, seed_cache)
        return
    print(f"[x1_live] real-time loop every {args.interval}s, GUI={args.gui_url}",
          file=sys.stderr)
    while True:
        try:
            step(args, here, live, seed_cache)
        except SystemExit:
            raise
        except Exception as e:
            print(f"[loop] error: {e}", file=sys.stderr)
        time.sleep(max(1, args.interval))


if __name__ == "__main__":
    main()
