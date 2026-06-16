#!/usr/bin/env python3
"""aurora_flow.py -- Aurora "Order-Flow Liquidity Heatshelves", ported from
the TradingView Pine v6 indicator to Omega / IBKR data.

WHAT THIS IS
------------
A faithful port of Aurora's SHELF ENGINE (the part that has analytical value):
absorption walls, initiative nodes, and the defend/break/flip lifecycle that
turns a stream of per-bar volume footprints into a ranked set of live S/R
liquidity levels. The TradingView-only rendering (boxes, glow, dashboard
table) is intentionally dropped -- Omega consumes LEVELS + EVENTS, not
pixels.

THE VOLUME SOURCE (read this before anything else)
--------------------------------------------------
Aurora is built on FOOTPRINT data: per-price-row volume split into buy vs
sell (delta). That requires a real centralized tape.

  * Spot XAUUSD (CFD/FX) has NO tape on IBKR -> footprint is IMPOSSIBLE there.
  * MGC (COMEX micro-gold future) HAS a real tape. Omega already records it:
      tools/ibkr_dom_bridge.py
        TradesRecorder -> ibkr_trades_<SYM>_<DATE>.csv  (ts_ms,price,size,exch,spec)
        DomRecorder    -> ibkr_l2_<SYM>_<DATE>.csv       (the order book)
    THAT MGC TAPE IS THE REAL VOLUME. Run this on MGC, never on spot gold.

THE DELTA RECONSTRUCTION (what Pine got for free, we compute)
------------------------------------------------------------
request.footprint() hands Pine the per-row delta. IBKR AllLast gives
price+size with NO aggressor side. We reconstruct it with the QUOTE RULE,
falling back to the TICK RULE:
    trade price >= best_ask  -> BUY  (+size)   lifted the offer
    trade price <= best_bid  -> SELL (-size)   hit the bid
    strictly between         -> tick rule: uptick=BUY, downtick=SELL,
                                else carry the previous sign
best_bid/best_ask come from the recorded L2 stream, time-aligned to the trade.

usage:
  # real (pull a day's CSVs off the VPS first):
  python ibkr/aurora_flow.py --trades ibkr_trades_MGC_2026-06-16.csv \
        --l2 ibkr_l2_MGC_2026-06-16.csv --tf-min 30 --out aurora_MGC.json
  # self-test on synthetic order flow (no data needed):
  python ibkr/aurora_flow.py --selftest
"""
from __future__ import annotations
import argparse, csv, json, math, os, sys
from bisect import bisect_right
from dataclasses import dataclass, field, asdict

MGC_MINTICK = 0.10   # COMEX micro-gold tick


# ─────────────────────────────  CONFIG  ─────────────────────────────────────
@dataclass
class AuroraCfg:
    mintick: float    = MGC_MINTICK
    ticks_per_row: int = 25      # price height of a footprint row, in ticks
    tf_min: int       = 30       # footprint bar timeframe (minutes)
    vol_frac: float   = 0.55     # row must hold >= this frac of the bar's busiest row
    absorb_ratio: float = 0.25   # |delta|/vol <= this -> balanced flow -> absorption
    break_buf_atr: float = 0.10  # close must clear band edge by this*ATR to break
    lookback: int     = 180      # bars of INACTIVITY before a shelf is pruned
    max_shelves: int  = 250
    allow_flip: bool  = True
    show_absorb: bool = True
    show_init: bool   = True
    key_per_side: int = 2
    min_gap_atr: float = 0.6     # min vertical gap between spotlighted walls
    max_dist_pct: float = 3.0    # hide walls beyond this % from price (0=all)
    atr_n: int        = 14


# ─────────────────────────────  SHELF  ──────────────────────────────────────
@dataclass
class Shelf:
    orig: int            # birth bar
    last: int            # last activity bar (prune clock)
    top: float
    bot: float
    vol: float
    dele: float          # accumulated delta
    is_buy: bool         # demand side?
    is_abs: bool         # absorption (True) vs initiative (False)
    broken: bool = False
    end: int = 0
    touch: int = 0       # defenses + merges (reinforcement)
    flipped: bool = False

    @property
    def mid(self) -> float:
        return (self.top + self.bot) / 2.0

    def strength(self) -> float:
        return self.vol * (1.0 + 0.25 * self.touch)


# ───────────────────────  FOOTPRINT (delta) BUILD  ──────────────────────────
def load_l2_quotes(path):
    """Return (ts_ms[], best_bid[], best_ask[]) sorted by ts. The DOM recorder
    schema varies; we autodetect bid/ask columns. Missing file -> empty (the
    classifier then falls back to the pure tick rule)."""
    if not path or not os.path.exists(path):
        return [], [], []
    ts, bid, ask = [], [], []
    with open(path, newline="") as fh:
        r = csv.DictReader(fh)
        cols = {c.lower(): c for c in (r.fieldnames or [])}
        tcol = next((cols[c] for c in ("ts_ms", "ts", "time") if c in cols), None)
        bcol = next((cols[c] for c in ("best_bid", "bid", "bid_px", "bid_price") if c in cols), None)
        acol = next((cols[c] for c in ("best_ask", "ask", "ask_px", "ask_price") if c in cols), None)
        if not (tcol and bcol and acol):
            return [], [], []
        for row in r:
            try:
                t = int(float(row[tcol])); b = float(row[bcol]); a = float(row[acol])
            except (ValueError, TypeError, KeyError):
                continue
            if b > 0 and a > 0:
                ts.append(t); bid.append(b); ask.append(a)
    order = sorted(range(len(ts)), key=lambda i: ts[i])
    return [ts[i] for i in order], [bid[i] for i in order], [ask[i] for i in order]


def classify_trades(trades, q_ts, q_bid, q_ask):
    """trades: list of (ts_ms, price, size). Returns same list with signed size
    via quote rule (-> tick-rule fallback). prevailing quote = last quote at or
    before the trade ts."""
    out = []
    last_px = None
    last_sign = 1
    for ts, px, sz in trades:
        sign = 0
        if q_ts:
            i = bisect_right(q_ts, ts) - 1
            if i >= 0:
                if px >= q_ask[i]:   sign = 1
                elif px <= q_bid[i]: sign = -1
        if sign == 0:  # between quotes, or no L2 -> tick rule
            if last_px is not None:
                if px > last_px:   sign = 1
                elif px < last_px: sign = -1
                else:              sign = last_sign
            else:
                sign = 1
        last_px = px; last_sign = sign
        out.append((ts, px, sz, sign))
    return out


def build_bar_footprints(signed_trades, cfg: AuroraCfg):
    """Bucket signed trades into TF bars, then into price rows. Returns a list
    of bars in time order, each a dict:
       {bar_ms, high, low, close, rows: {row_key: [vol, delta, up, dn]}}
    row_key = floor(price / row_height); up/dn = row's price band."""
    row_h = cfg.ticks_per_row * cfg.mintick
    bar_ms = cfg.tf_min * 60 * 1000
    bars = {}
    order = []
    for ts, px, sz, sign in signed_trades:
        bkey = ts // bar_ms
        if bkey not in bars:
            bars[bkey] = {"bar_ms": bkey * bar_ms, "high": px, "low": px,
                          "close": px, "rows": {}}
            order.append(bkey)
        b = bars[bkey]
        b["high"] = max(b["high"], px); b["low"] = min(b["low"], px); b["close"] = px
        rk = math.floor(px / row_h)
        row = b["rows"].get(rk)
        if row is None:
            dn = rk * row_h
            b["rows"][rk] = [sz, sign * sz, dn + row_h, dn]
        else:
            row[0] += sz; row[1] += sign * sz
    return [bars[k] for k in sorted(order)]


# ─────────────────────────────  ENGINE  ─────────────────────────────────────
class AuroraEngine:
    """Port of the Pine shelf engine. feed_bar() per confirmed footprint bar;
    snapshot() returns the live ranked levels + the bar's events."""

    def __init__(self, cfg: AuroraCfg = AuroraCfg()):
        self.cfg = cfg
        self.shelves: list[Shelf] = []
        self.bar = 0
        self.tr_window: list[float] = []   # true ranges for ATR
        self._prev_close = None
        self.last_poc = None
        self.win_delta: list[float] = []   # per-bar delta history (dashboard)
        self.last_events: list[dict] = []

    # --- ATR over bar high/low/close ---
    def _atr(self):
        if not self.tr_window:
            return 10 * self.cfg.mintick
        w = self.tr_window[-self.cfg.atr_n:]
        return sum(w) / len(w)

    def _push_tr(self, hi, lo, cl):
        tr = hi - lo
        if self._prev_close is not None:
            tr = max(tr, abs(hi - self._prev_close), abs(lo - self._prev_close))
        self.tr_window.append(tr)
        self._prev_close = cl

    def _overlap_idx(self, top, bot, is_buy, is_abs):
        for i, s in enumerate(self.shelves):
            if (not s.broken and s.is_buy == is_buy and s.is_abs == is_abs
                    and bot <= s.top and top >= s.bot):
                return i
        return -1

    def _add_or_merge(self, top, bot, vol, dele, is_buy, is_abs):
        j = self._overlap_idx(top, bot, is_buy, is_abs)
        if j < 0:
            self.shelves.append(Shelf(self.bar, self.bar, top, bot, vol, dele,
                                      is_buy, is_abs, end=self.bar))
            return True
        s = self.shelves[j]
        s.vol += vol; s.dele += dele
        s.top = max(s.top, top); s.bot = min(s.bot, bot)
        s.touch += 1; s.last = self.bar
        return False

    def feed_bar(self, bar):
        cfg = self.cfg
        hi, lo, cl = bar["high"], bar["low"], bar["close"]
        self._push_tr(hi, lo, cl)
        atr = self._atr()
        buf = atr * cfg.break_buf_atr
        events = []

        # POC = busiest row this bar
        if bar["rows"]:
            poc_rk = max(bar["rows"], key=lambda k: bar["rows"][k][0])
            pr = bar["rows"][poc_rk]
            self.last_poc = (pr[2] + pr[3]) / 2.0
            self.win_delta.append(sum(r[1] for r in bar["rows"].values()))

        # 1) DEFEND / BREAK / FLIP over existing shelves
        for s in self.shelves:
            touched = lo <= s.top and hi >= s.bot
            if not s.broken:
                if s.is_buy and cl < s.bot - buf:
                    s.broken = True; s.end = self.bar
                    events.append(self._evt("broken", s))
                elif (not s.is_buy) and cl > s.top + buf:
                    s.broken = True; s.end = self.bar
                    events.append(self._evt("broken", s))
                elif touched and (cl > s.top if s.is_buy else cl < s.bot):
                    s.touch += 1; s.last = self.bar
                    events.append(self._evt("defended", s))
            elif cfg.allow_flip and not s.flipped:
                if s.is_buy and touched and cl < s.bot:
                    s.is_buy = False; s.broken = False; s.flipped = True
                    s.touch = 1; s.dele = 0.0; s.orig = self.bar; s.last = self.bar
                    events.append(self._evt("flipped", s))
                elif (not s.is_buy) and touched and cl > s.top:
                    s.is_buy = True; s.broken = False; s.flipped = True
                    s.touch = 1; s.dele = 0.0; s.orig = self.bar; s.last = self.bar
                    events.append(self._evt("flipped", s))

        # 2) NODE DETECTION — volume-gate first, then candidates
        rows = list(bar["rows"].values())   # [vol, delta, up, dn]
        if rows:
            b_max = max(r[0] for r in rows)
            vol_gate = b_max * cfg.vol_frac
            third = (hi - lo) / 3.0
            best = {"absB": (0.0, None), "absS": (0.0, None),
                    "iniB": (0.0, None), "iniS": (0.0, None)}
            for r in rows:
                tv, dd, up, dn = r
                if tv < vol_gate:
                    continue
                mid = (up + dn) / 2.0
                ratio = abs(dd) / max(tv, 1e-10)
                if ratio <= cfg.absorb_ratio:
                    if mid <= lo + third and tv > best["absB"][0]:
                        best["absB"] = (tv, r)
                    elif mid >= hi - third and tv > best["absS"][0]:
                        best["absS"] = (tv, r)
                else:
                    if dd > 0 and dd > best["iniB"][0]:
                        best["iniB"] = (dd, r)
                    elif dd < 0 and -dd > best["iniS"][0]:
                        best["iniS"] = (-dd, r)

            def _try(slot, is_buy, is_abs):
                _, r = best[slot]
                if r is not None and self._add_or_merge(r[2], r[3], r[0], r[1], is_buy, is_abs):
                    events.append(self._evt("new_wall", self.shelves[-1]))

            if cfg.show_absorb:
                _try("absB", True, True); _try("absS", False, True)
            if cfg.show_init:
                _try("iniB", True, False); _try("iniS", False, False)

        # 3) PRUNE by inactivity (last, not birth)
        cut = self.bar - cfg.lookback
        self.shelves = [s for s in self.shelves if s.last >= cut]

        # 4) HARD CAP — evict weakest broken first, else weakest overall
        while len(self.shelves) > cfg.max_shelves:
            broken = [s for s in self.shelves if s.broken]
            pool = broken if broken else self.shelves
            victim = min(pool, key=lambda s: s.strength())
            self.shelves.remove(victim)

        # trim dashboard window
        while len(self.win_delta) > cfg.lookback:
            self.win_delta.pop(0)

        self.last_events = events
        self.bar += 1
        return events

    def _evt(self, ev, s: Shelf):
        return {"event": ev, "side": "buy" if s.is_buy else "sell",
                "kind": "absorption" if s.is_abs else "initiative",
                "top": round(s.top, 4), "bot": round(s.bot, 4),
                "vol": round(s.vol, 2), "delta": round(s.dele, 2),
                "touches": s.touch}

    # --- key-wall selection: score = strength / (1 + ATR-distance), de-clustered ---
    def _score(self, s: Shelf, price, atr):
        return s.strength() / (1.0 + abs(s.mid - price) / atr)

    def _select_keys(self, pool, cnt, min_gap, price, atr):
        out = []
        used = set()
        for _ in range(min(cnt, len(pool))):
            best_sc, best = -1.0, None
            for s in pool:
                if id(s) in used:
                    continue
                if min_gap > 0 and any(abs(s.mid - o.mid) < min_gap for o in out):
                    continue
                sc = self._score(s, price, atr)
                if sc > best_sc:
                    best_sc, best = sc, s
            if best is not None:
                used.add(id(best)); out.append(best)
        return out

    def snapshot(self, price, sym="MGC", tf=None):
        cfg = self.cfg
        atr = self._atr()
        in_range = lambda s: (cfg.max_dist_pct <= 0
                              or abs(s.mid - price) / price <= cfg.max_dist_pct / 100.0)
        sup_pool = [s for s in self.shelves if not s.broken and in_range(s)
                    and not s.is_buy and s.mid > price]
        dem_pool = [s for s in self.shelves if not s.broken and in_range(s)
                    and s.is_buy and s.mid < price]
        min_gap = cfg.min_gap_atr * atr
        sup_keys = self._select_keys(sup_pool, cfg.key_per_side, min_gap, price, atr)
        dem_keys = self._select_keys(dem_pool, cfg.key_per_side, min_gap, price, atr)

        def _lvl(s):
            return {"side": "supply" if not s.is_buy else "demand",
                    "kind": "absorption" if s.is_abs else "initiative",
                    "mid": round(s.mid, 4), "top": round(s.top, 4), "bot": round(s.bot, 4),
                    "vol": round(s.vol, 2), "delta": round(s.dele, 2),
                    "touches": s.touch, "flipped": s.flipped,
                    "strength": round(s.strength(), 2),
                    "dist_atr": round((s.mid - price) / atr, 2)}

        win_cum = sum(self.win_delta)
        nearest_sup = min((s for s in sup_keys), key=lambda s: s.mid - price, default=None)
        nearest_dem = max((s for s in dem_keys), key=lambda s: s.mid - price, default=None)
        return {
            "sym": sym, "tf_min": tf or cfg.tf_min, "price": round(price, 4),
            "atr": round(atr, 4), "poc": round(self.last_poc, 4) if self.last_poc else None,
            "window_delta": round(win_cum, 2),
            "bias": "buyers" if win_cum >= 0 else "sellers",
            "n_shelves": len(self.shelves),
            "nearest_supply": _lvl(nearest_sup) if nearest_sup else None,
            "nearest_demand": _lvl(nearest_dem) if nearest_dem else None,
            "key_supply": [_lvl(s) for s in sup_keys],
            "key_demand": [_lvl(s) for s in dem_keys],
            "events": self.last_events,
        }


# ───────────────────────────  DRIVER  ──────────────────────────────────────
def load_trades(path):
    """ibkr_trades_*.csv -> [(ts_ms, price, size)] in time order."""
    out = []
    with open(path, newline="") as fh:
        r = csv.DictReader(fh)
        for row in r:
            try:
                out.append((int(float(row["ts_ms"])), float(row["price"]), float(row["size"])))
            except (ValueError, TypeError, KeyError):
                continue
    out.sort(key=lambda x: x[0])
    return out


def run_file(trades_path, l2_path, cfg: AuroraCfg, sym):
    trades = load_trades(trades_path)
    q_ts, q_bid, q_ask = load_l2_quotes(l2_path)
    signed = classify_trades(trades, q_ts, q_bid, q_ask)
    bars = build_bar_footprints(signed, cfg)
    eng = AuroraEngine(cfg)
    total_events = 0
    for b in bars:
        total_events += len(eng.feed_bar(b))
    price = bars[-1]["close"] if bars else 0.0
    snap = eng.snapshot(price, sym=sym)
    quote_cov = sum(1 for _, _, _, s in signed if s != 0) and len(q_ts) > 0
    snap["_meta"] = {"trades": len(trades), "bars": len(bars),
                     "l2_quotes": len(q_ts), "total_events": total_events,
                     "delta_via": "quote_rule+L2" if q_ts else "tick_rule_only(no L2)"}
    return snap


# ───────────────────────────  SELF-TEST  ────────────────────────────────────
def selftest():
    """Synthetic order flow: an absorption demand shelf gets defended twice then
    broken. No data files needed -- proves the engine wiring end-to-end."""
    cfg = AuroraCfg(tf_min=30, ticks_per_row=25, lookback=50)
    base = 4300.0
    bar_ms = cfg.tf_min * 60 * 1000
    row_h = cfg.ticks_per_row * cfg.mintick
    trades = []
    t = 0
    def emit(bar_i, px, n, sign):
        nonlocal t
        for _ in range(n):
            trades.append((bar_i * bar_ms + t % bar_ms, px, 1.0 * sign if False else 1.0))
            t += 1000
    # bar 0-2: heavy balanced volume at the LOW (absorption demand wall) + light above
    for bi in range(3):
        for _ in range(60): trades.append((bi*bar_ms + len(trades)%bar_ms, base, 5.0))   # big @ base (low)
        for _ in range(10): trades.append((bi*bar_ms + len(trades)%bar_ms, base + 8*row_h, 2.0))
    # bars 3-4: price holds above base, dips and rejects (defends)
    for bi in range(3, 5):
        for _ in range(20): trades.append((bi*bar_ms + len(trades)%bar_ms, base + 3*row_h, 2.0))
        trades.append((bi*bar_ms + len(trades)%bar_ms, base, 1.0))            # wick to band
        for _ in range(20): trades.append((bi*bar_ms + len(trades)%bar_ms, base + 4*row_h, 2.0))  # close above
    # bars 5-6: break below the band
    for bi in range(5, 7):
        for _ in range(30): trades.append((bi*bar_ms + len(trades)%bar_ms, base - 10*row_h, 3.0))

    trades.sort(key=lambda x: x[0])
    signed = classify_trades(trades, [], [], [])   # tick-rule only (no L2 in test)
    bars = build_bar_footprints(signed, cfg)
    eng = AuroraEngine(cfg)
    all_ev = []
    for b in bars:
        all_ev += eng.feed_bar(b)
    snap = eng.snapshot(bars[-1]["close"], sym="TEST")
    print(f"bars={len(bars)} shelves={len(eng.shelves)} events={len(all_ev)}")
    kinds = [e["event"] for e in all_ev]
    print("event sequence:", kinds)
    assert "new_wall" in kinds, "no wall formed"
    assert any(k in kinds for k in ("defended", "broken")), "no lifecycle event"
    print("nearest_demand:", json.dumps(snap.get("nearest_demand")))
    print("nearest_supply:", json.dumps(snap.get("nearest_supply")))
    print("SELFTEST OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trades", help="ibkr_trades_<SYM>_<DATE>.csv (AllLast tape)")
    ap.add_argument("--l2", help="ibkr_l2_<SYM>_<DATE>.csv (for buy/sell classification)")
    ap.add_argument("--sym", default="MGC")
    ap.add_argument("--tf-min", type=int, default=30)
    ap.add_argument("--ticks-per-row", type=int, default=25)
    ap.add_argument("--out", default=None)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        selftest(); return
    if not a.trades:
        ap.error("--trades is required (or use --selftest). Run on MGC futures tape, NOT spot XAUUSD.")

    cfg = AuroraCfg(tf_min=a.tf_min, ticks_per_row=a.ticks_per_row)
    snap = run_file(a.trades, a.l2, cfg, a.sym)
    print(json.dumps(snap, indent=2))
    if a.out:
        with open(a.out, "w") as fh:
            json.dump(snap, fh, indent=2)
        print(f"[aurora] wrote {a.out}", flush=True)


if __name__ == "__main__":
    main()
