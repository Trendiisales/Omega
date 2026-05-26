#!/usr/bin/env python3
# Simulate S44 gates (regime + HTF bias) applied to today's 21 trades.
#
# Approach: today's gold range was ~4540 high to ~4495 low (clear bearish
# intraday) and regime=COMPRESSION was logged for most of the session. Use
# inferred HTF bias direction + COMPRESSION regime as the gates.
#
# Assumptions:
#   - XAUUSD HTF bias = BEARISH all day (rolled down)
#   - EURUSD HTF bias = NEUTRAL (range-bound at 1.16)
#   - GBPUSD HTF bias = BULLISH (closed higher at 1.35)
#   - AUDUSD HTF bias = NEUTRAL/SLIGHT_BULLISH (0.72)
#   - regime=COMPRESSION dominant for gold; engine-side gate blocks gold entries
#     unless 11:35-11:36 EXPANSION_BREAKOUT window (where g_gold_scalp_pyramid
#     might have fired but supervisor BREAKOUT path goes to disabled bracket).
#
# Output: per-trade prediction (blocked/allowed) and resulting PnL.

trades = [
    # (time,    sym,     side,  entry,   exit,    hold,  type,    engine,                pnl_net)
    ("20:00", "XAUUSD", "LONG",  4509.19, 4509.56, "1m33s","TR",  "GoldScalpPyramid",     +0.45),
    ("19:50", "XAUUSD", "LONG",  4507.56, 4504.93, "2m14s","FC",  "GoldScalpPyramid",    -14.55),
    ("19:50", "AUDUSD", "LONG",  0.72,    0.72,    "1s",   "FC",  "FxScalpPyramid_AUD",   -3.15),
    ("19:45", "XAUUSD", "LONG",  4505.10, 4505.70, "3m16s","FC",  "GoldScalpPyramid",     +1.60),
    ("19:20", "XAUUSD", "LONG",  4502.64, 4502.77, "1m22s","FC",  "GoldScalpPyramid",     -0.75),
    ("18:05", "XAUUSD", "SHORT", 4487.68, 4490.13, "22s",  "FC",  "GoldScalpPyramid",    -13.65),
    ("16:20", "EURUSD", "LONG",  1.16,    1.16,    "18s",  "FC",  "FxScalpPyramid_EUR",   -4.20),
    ("16:15", "XAUUSD", "SHORT", 4501.45, 4501.61, "2m11s","TR",  "GoldScalpPyramid",     -2.20),
    ("14:00", "DJ30",   "SHORT", 50674.5, 50562.7, "119m", "SL",  "OpeningRange",         +5.30),
    ("12:00", "GER40",  "LONG",  25262.9, 25172.7, "216m", "SL",  "Ger40TurtleH4",       -24.80),
    ("15:20", "GBPUSD", "SHORT", 1.34,    1.35,    "10m58s","FC", "FxScalpPyramid_GBP",   -7.45),
    ("15:05", "XAUUSD", "SHORT", 4511.45, 4513.73, "28s",  "FC",  "GoldScalpPyramid",    -12.80),
    ("15:00", "GBPUSD", "SHORT", 1.34,    1.35,    "51s",  "FC",  "FxScalpPyramid_GBP",   -4.65),
    ("14:55", "GBPUSD", "SHORT", 1.35,    1.35,    "5s",   "FC",  "FxScalpPyramid_GBP",   -5.10),
    ("14:00", "NAS100", "LONG",  29931.9, 30008.6, "47m",  "SL",  "OpeningRange",         +0.76),
    ("13:55", "EURUSD", "SHORT", 1.16,    1.16,    "1m50s","FC",  "FxScalpPyramid_EUR",   -5.60),
    ("13:50", "AUDUSD", "LONG",  0.72,    0.72,    "45s",  "FC",  "FxScalpPyramid_AUD",   -4.45),
    ("13:30", "EURUSD", "SHORT", 1.16,    1.16,    "24s",  "FC",  "FxScalpPyramid_EUR",   -4.50),
    ("11:40", "XAUUSD", "SHORT", 4511.00, 4510.94, "8s",   "TR",  "GoldScalpPyramid",     -2.50),
    ("10:35", "XAUUSD", "LONG",  4539.15, 4536.50, "5s",   "FC",  "GoldScalpPyramid",    -14.65),
    ("09:45", "XAUUSD", "SHORT", 4513.23, 4515.55, "27s",  "FC",  "GoldScalpPyramid",    -13.00),
]

# Inferred HTF bias (today)
HTF = {
    "XAUUSD": "BEARISH",
    "EURUSD": "NEUTRAL",   # ~1.16 flat
    "GBPUSD": "BULLISH",   # 1.34 -> 1.35
    "AUDUSD": "NEUTRAL",
}

# Per gold-stack regime log: COMPRESSION dominant. ~1-2 min window of
# IMPULSE/EXPANSION_BREAKOUT at 11:35-11:36 only.
def gold_in_compression(hh_mm: str) -> bool:
    # Single brief expansion window at 11:35-11:36 (operator's screenshot).
    if hh_mm in ("11:35", "11:36"):
        return False
    return True


def s44_blocks(t):
    hh_mm, sym, side = t[0], t[1], t[2]
    engine = t[7]  # tuple index: 0=time 1=sym 2=side 3=entry 4=exit 5=hold 6=type 7=engine 8=pnl
    # S44 only patches GoldScalpPyramid + FxScalpPyramid_*. Other engines pass.
    if not engine.startswith(("GoldScalpPyramid", "FxScalpPyramid")):
        return False, "S44 not wired (other engine)"
    # 1. Regime gate: gold-only — block when compressing
    if sym == "XAUUSD":
        if gold_in_compression(hh_mm):
            return True, "regime=COMPRESSION"
    # 2. HTF-bias gate
    bias = HTF.get(sym, "NEUTRAL")
    if bias == "BEARISH" and side == "LONG":
        return True, "HTF=BEARISH blocks LONG"
    if bias == "BULLISH" and side == "SHORT":
        return True, "HTF=BULLISH blocks SHORT"
    return False, "S44 allows"


total_pnl_pre = 0.0
total_pnl_post = 0.0
blocked = 0
allowed = 0
print(f"{'time':>5}  {'sym':<6} {'side':<5} {'engine':<24} {'pnl':>7}  blocked? reason")
print("-" * 90)
for t in trades:
    pnl = t[8]
    total_pnl_pre += pnl
    block, reason = s44_blocks(t)
    if block:
        blocked += 1
        post = 0.0
    else:
        allowed += 1
        post = pnl
    total_pnl_post += post
    mark = "BLOCK" if block else "PASS "
    print(f"{t[0]:>5}  {t[1]:<6} {t[2]:<5} {t[6]:<24} {pnl:>+7.2f}  {mark}  {reason}")

print()
print(f"SUMMARY")
print(f"  trades total:       {len(trades)}")
print(f"  blocked by S44:     {blocked}")
print(f"  allowed by S44:     {allowed}")
print(f"  net PnL pre-S44:    {total_pnl_pre:+.2f}")
print(f"  net PnL post-S44:   {total_pnl_post:+.2f}")
print(f"  improvement:        {total_pnl_post - total_pnl_pre:+.2f}")
