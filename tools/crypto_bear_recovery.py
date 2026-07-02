#!/usr/bin/env python3
# =============================================================================
# crypto_bear_recovery.py -- BearRecovery long-only signal engine for BTC/ETH
# (S-2026-07-03, operator: "trade the bounces in the current downtrend, max
#  protection, max aggression, without impacting the edges if found").
#
# WHAT IT IS: the deployable half of backtest/crypto_bear_bounce/ (READ
# FINDINGS.md FIRST). It emits the full regime ladder + entry/exit/protection
# state per symbol as JSON, for the ~/Crypto book (stall_accountant /
# refresh_shadow cycle) or manual use. It does NOT place orders itself --
# execution stays with the book / omega_crypto_bridge.py (paper 4002).
#
# THE LADDER (daily closes, completed UTC bars only):
#   KNIFE     close < SMA200 and NOT (close > SMA50 and SMA50 rising)
#             -> FLAT. No long-only entry family survives here (7 families
#                backtested 2017-2026, all PF<1 in 2018/2022/2025-26 knives).
#                The protection IS the flatness.
#   RECOVERY  close < SMA200 AND close > SMA50 AND SMA50 > SMA50[5d ago]
#             -> this engine trades: LONG on daily close crossing above EMA9
#                after >=3 closes below. Ride to first daily close < EMA9.
#   BULL      close > SMA200 -> hand off to the Luke daily system (gate open).
#
# ADVERSE-PROTECTION: backtested verdict (FINDINGS.md sect.4-5, 2026-07-03) --
#   initial stop = entry-day low - 0.5*ATR14(D) (intrabar, hard);
#   BE-AND-RIDE floor: after MFE >= +2%, floor = entry (lock 0); exit only
#     through the floor -- net UP, PF 5.16->8.85, worst -8.3%->-4.8%;
#   COLD CUT: no-op at all values (EMA9-flip already bounds never-green) -- OFF;
#   GIVEBACK clips: proven harmful (net -62..-90%) -- MUST NOT be added.
# Sizing: risk-based off the structural stop; ship 2% equity risk (3% = max
# aggressive tier; PF holds >=4.2 to 5%). ~2 signals/yr -- aggression comes
# from size, not frequency.
#
# Usage:
#   python3 crypto_bear_recovery.py                    # one-shot, both symbols
#   python3 crypto_bear_recovery.py --symbols BTC-USD  # subset
#   python3 crypto_bear_recovery.py --equity 50000 --risk-pct 0.02
# Output: one JSON line per symbol:
#   {"sym","regime","signal","entry_ref","stop","be_arm_pct","be_lock",
#    "qty_usd","exit_flag","asof","detail"}
#   signal: "ENTER" only on the day the reclaim cross fires; "HOLD-RULES" when
#   a position (if any) should keep running; "EXIT" when close < EMA9; "NONE".
# =============================================================================
import argparse, json, urllib.request, datetime as dt

CB = "https://api.exchange.coinbase.com/products/{sym}/candles?granularity=86400&start={s}&end={e}"
UA = {"User-Agent": "Mozilla/5.0"}


def fetch_daily(sym, days=320):
    """Completed daily candles, ascending [(ts,o,h,l,c)]. Coinbase caps 300/req."""
    now = dt.datetime.now(dt.timezone.utc)
    end = now.replace(hour=0, minute=0, second=0, microsecond=0)  # exclude today (incomplete)
    rows = {}
    t1 = end
    while days > 0:
        take = min(days, 300)
        t0 = t1 - dt.timedelta(days=take)
        u = CB.format(sym=sym, s=t0.isoformat(), e=t1.isoformat())
        data = json.loads(urllib.request.urlopen(
            urllib.request.Request(u, headers=UA), timeout=30).read())
        for ts, lo, hi, op, cl, vol in data:
            if ts < int(end.timestamp()):          # completed bars only
                rows[int(ts)] = (op, hi, lo, cl)
        days -= take
        t1 = t0
    ts_sorted = sorted(rows)
    return ts_sorted, [rows[t] for t in ts_sorted]


def sma(x, w, i):
    return sum(x[i - w + 1:i + 1]) / w if i >= w - 1 else None


def ema_series(x, span):
    k = 2.0 / (span + 1)
    out, e = [], None
    for v in x:
        e = v if e is None else v * k + e * (1 - k)
        out.append(e)
    return out


def atr14(h, l, c, i):
    if i < 14:
        return None
    trs = [max(h[j] - l[j], abs(h[j] - c[j - 1]), abs(l[j] - c[j - 1]))
           for j in range(i - 13, i + 1)]
    return sum(trs) / 14


def evaluate(sym, equity, risk_pct):
    ts, bars = fetch_daily(sym)
    o = [b[0] for b in bars]; h = [b[1] for b in bars]
    l = [b[2] for b in bars]; c = [b[3] for b in bars]
    i = len(c) - 1                                   # last COMPLETED daily bar
    if i < 205:
        return {"sym": sym, "error": f"only {i+1} daily bars; need 206"}
    e9 = ema_series(c, 9)
    s200 = sma(c, 200, i); s50 = sma(c, 50, i); s50p = sma(c, 50, i - 5)
    bear = c[i] < s200
    recovery = bear and c[i] > s50 and s50 > s50p
    regime = "BULL" if not bear else ("RECOVERY" if recovery else "KNIFE")

    # consecutive closes below EMA9 up to yesterday
    below = 0
    j = i - 1
    while j >= 0 and c[j] < e9[j]:
        below += 1; j -= 1
    crossed_up = c[i] > e9[i] and below >= 3
    exit_flag = c[i] < e9[i]

    out = {
        "sym": sym, "regime": regime, "signal": "NONE",
        "asof": dt.datetime.fromtimestamp(ts[i], dt.timezone.utc).strftime("%Y-%m-%d"),
        "close": c[i], "ema9": round(e9[i], 2),
        "sma50": round(s50, 2), "sma200": round(s200, 2),
        "exit_flag": bool(exit_flag),
        "be_arm_pct": 0.02, "be_lock": 0.0,
    }
    if regime == "RECOVERY" and crossed_up:
        a = atr14(h, l, c, i)
        stop = l[i] - 0.5 * (a or 0)
        risk_ps = c[i] - stop
        if risk_ps > 0:
            out.update(signal="ENTER", entry_ref=c[i], stop=round(stop, 2),
                       qty_usd=round(equity * risk_pct / risk_ps * c[i], 2),
                       detail=f"EMA9 reclaim after {below}d below; stop=day-low-0.5*ATR; "
                              f"BE floor arms at +2% MFE; exit first daily close<EMA9")
    elif regime == "RECOVERY":
        out["signal"] = "HOLD-RULES"
        out["detail"] = "in recovery sub-regime; waiting for EMA9 reclaim cross (>=3d below)"
    elif regime == "KNIFE":
        out["detail"] = ("KNIFE phase: FLAT by design -- no long-only entry survives here "
                         "(backtest/crypto_bear_bounce/FINDINGS.md sect.3)")
    else:
        out["detail"] = "BULL regime: Luke daily system owns this phase"
    if exit_flag:
        out["signal"] = "EXIT" if out["signal"] in ("NONE", "HOLD-RULES") else out["signal"]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbols", default="BTC-USD,ETH-USD")
    ap.add_argument("--equity", type=float, default=100_000.0)
    ap.add_argument("--risk-pct", type=float, default=0.02,
                    help="2%% ship tier; 3%% max-aggressive (FINDINGS.md sect.6)")
    a = ap.parse_args()
    for sym in a.symbols.split(","):
        try:
            print(json.dumps(evaluate(sym.strip(), a.equity, a.risk_pct)))
        except Exception as e:
            print(json.dumps({"sym": sym.strip(), "error": str(e)}))


if __name__ == "__main__":
    main()
