# BreakBounce — chop/bear protection: validation TODO

**Status (2026-05-31):** engine live in **shadow**. Chop/bear protection is
BUILT but OFF, because it cannot be validated on the data we have.

## Why it's off
- 2yr XAUUSD tick file = **gold bull, end to end**. No chop/bear regime.
- Every entry-removing filter tested is **subtractive on this data**:
  - ADX(14) chop-gate IS/OOS sweep: every threshold drops OOS PF
    1.54 → 1.20–1.32, OOS net ~halved (IS-up/OOS-down overfit signature —
    same as the ER chop-gate dead-end).
  - Session/TOD sweep: 24h PF 1.73 > 07–18 PF 1.67 > any sub-window.
- Reason: a chop filter removes low-ADX consolidations, which in a bull
  resolve UP, so it cuts winners. The protective value only appears in
  chop/bear — which isn't in the sample.

## What IS protecting us now (free, always on)
- **D1 EMA50/200 bias** = the structural bear guard. No longs in a
  downtrend (bias≠+1); shorts arm in a bear (bias=−1). The engine is
  symmetric — it just never went short because gold never trended down.
- ATR stop + 1.5R TP bound per-trade chop damage.
- Session 07–18 UTC cuts the thin Asian tape (kept as a sensible default
  even though it was marginally subtractive in this bull).

## The plan — validate on forward data, then enable by evidence
1. Shadow runs through whatever regimes the market gives (incl. chop/bear).
2. Each trade is tagged with its regime: **`adx` (ADX-at-entry) is now in
   `outputs/breakbounce_l2_capture.csv`** alongside the L2 stream.
3. Once a chop/bear stretch is captured (≥30–50 trades through it):
   - Re-run `bb_regime` on forward ticks (or join capture `adx` with the
     ledger exits) to see if gating low-ADX entries would have helped *in
     that regime*.
   - Re-run the session sweep (`bb_backtest <ticks> <tf...> <start_h> <end_h>`).
4. **Enable `REGIME_ADX_MIN` (or a session cut) ONLY if the forward chop
   data shows it helps.** The lever is wired and live-gated; flipping it on
   is a one-line config change in `engine_init.hpp`.

## Levers (all in BreakBounceEngine.hpp, config-gated)
- `REGIME_ADX_MIN` — ADX chop-gate (0 = off).
- `SESSION_START_H` / `SESSION_END_H` / `USE_SESSION` — TOD chop cut.
- `USE_L2_PROTECT` — order-book reversal profit-lock (separate track, also
  pending live L2 validation; see capture/replay).

Do NOT force any of these on against the backtest — that repeats the ER
mistake. Evidence first.
