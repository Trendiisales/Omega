# ConnorsRSI2 faithful revalidation — NAS100 + GER40 (2026-07-21)

**Scope:** faithful re-cert of both live ConnorsRSI2 instances driving the REAL
`ConnorsRSI2Engine` class, plus a PROFIT-LOCK / giveback protection test. No wiring, no commit.

## Method (faithful, real engine)
- Baselines reproduced with the certified harnesses `connors_regime_gate_audit.cpp` (NAS) +
  `connors_ger_gate_audit.cpp` (GER) AND `connors_widestop_bt.cpp` (identical tick pattern,
  gives worst/maxDD). All three agree to the pt.
- Profit-lock overlay: NEW harness `connors_profitlock_bt.cpp` — drives the REAL engine exactly
  like the certified harnesses (g=off reproduces the baseline), overlays a giveback lock on daily
  h/l: `lock_px = avg_entry + (1-g)*(peak_high - avg_entry)`, arms only once `peak-entry >= ARM`
  pts (so it can NEVER cut an underwater trade — not the rejected cold-cut/wide-stop class),
  worse-of gap fill, re-anchored on scale-in, `force_close()` → real `_close()` → cost applied.
- Data: `/Users/jo/Tick/NDX_daily_2016_2026.csv` (2650 bars) + `GER40_daily_2016_2026.csv`
  (2727 bars). BOTH **CERTIFIED CLEAN** by `data_integrity_gate.py` (NAS 3 big-range WARN only;
  GER 6d holiday-gap WARN only). Cost = broker-real IBKR RT pts: NAS 8pt, GER 3pt (+2x stress).
- Deployed configs: NAS REGIME_GATE=1 asym bear-veto + SCALEIN (live, real money);
  GER close>SMA200 gate, no scale-in (SHADOW).

## Baselines — reproduced EXACTLY to the certified figures

| host | config | n | PF | net(pt) | worst | maxDD | 2022 bear | WF both+ | 2x-cost |
|---|---|---|---|---|---|---|---|---|---|
| **NAS100** | gate1+scalein, 8pt | 142 | **4.18** | +25,015 | −806 | 1,353 | +1,842 (PF3.01, n8) | **YES** (H1 5623 / H2 19392) | PF3.80 +23,263 both+ |
| **GER40** | close>SMA200, 3pt | 226 | **1.36** | +5,769 | −1,470 | 2,895 | +297 (n4, gate sits out) | **YES** (H1 1938 / H2 3831) | PF1.31 +5,091 both+ |

Certified basis was NAS PF4.18 / GER PF1.35 — reproduced (task figures confirmed). Both instances
pass every gate: net+, PF>1.3, both-WF-halves+, 2x-cost+, 2022 bear non-negative.

## Protection test — PROFIT-LOCK / giveback (lock ~90% of peak, armed low)

Swept g ∈ {0.10,0.20,0.30,0.50} × arm ∈ {20,50,100}pt, 1x + 2x cost, both hosts.

**NAS100 — LOCK-HURTS-EDGE (decisive).** EVERY lock cell haircuts net hard. Best cell
(g=0.20 arm=100) net +16,301 PF2.41 = **−35% net vs base**, and worst trade gets WORSE
(−1,099 vs −806) from gap-through fills + churn re-entries (locks 79–126, n 142→172–195).
maxDD improves only cosmetically (1353→1203). The lock sells the intraday pullback at 90% of
peak, but the MR exit (close>SMA5) fires at a HIGHER confirming up-close — the lock amputates
exactly the tail of the winning bounce the engine is paid to hold.

**GER40 — LOCK-HURTS-EDGE (decisive, worse).** Every lock cell haircuts net ~33% (best g=0.10
arm=100 +3,871 PF1.24) AND **breaks both-halves+** (low-arm cells drive H1 NEGATIVE; at 2x cost
nearly all cells fail both-halves), AND turns the 2022 bear NEGATIVE in low-arm cells. Worst
trade unchanged (−1,470 — the lock never binds where protection was wanted). It destroys the
certification, not just the net.

**Verdict (both hosts): LOCK-HURTS-EDGE — leave as-is, ride to the MR exit.** Consistent with the
S-2026-07-17 wide-stop rejection: the MR edge IS the ride to close>SMA5 / MAXHOLD10; the trend/
regime gate (+ MAXHOLD + BOOK_CAP) is the tail control, no in-flight price stop of any kind.

## Per-symbol verdicts

| host | verdict | rationale |
|---|---|---|
| **NAS100 (g_connors_nas)** | **KEEP-LIVE** | Strong certified edge, PF4.18 +25,015pt/10.5y, both-halves+, 2x-cost PF3.80+, 2022 bear +1,842 PF3.01. Low freq (~13.5 tr/yr) = Sharpe/diversifier leg (freq, not lot, caps $). No change. |
| **GER40 (g_connors_ger)** | **KEEP SHADOW-ONLY** | Passes every gate but MARGINAL: PF1.36, maxDD 2,895 ≈ 50% of 10y net, 2x-cost PF1.31. Real but thin edge; the 2022 "bear +297" is n=4 (gate sitting out, by design). Do NOT promote to live without more edge; current SHADOW deployment is correct. Not SCRAP. |
| **PROTECTION (both)** | **LOCK-HURTS-EDGE** | No profit-lock / giveback / stop of any width. Ride to MR exit. Tail lever = regime gate / BOOK_CAP / lot only. |

## Harnesses / evidence
- `backtest/connors_regime_gate_audit.cpp` (NAS baseline), `backtest/connors_ger_gate_audit.cpp`
  (GER baseline), `backtest/connors_widestop_bt.cpp` (worst/maxDD + wide-stop rejection).
- `backtest/connors_profitlock_bt.cpp` (NEW — profit-lock overlay, this session).
- Prior: `backtest/CONNORS_WIDESTOP_FINDINGS_2026-07-17.md` (wide catastrophic stop REJECTED).
