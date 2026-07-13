# XAU_4h_DonchN20 MIMIC — RESTING-ORDER REVALIDATION (S-2026-07-14, independent re-run)

**Operator ask:** "retest and validate new backtest" — independently reproduce the
resting-order (REST-F/M1) result of `SURVIVOR_MIMIC_INTRABAR_RECHECK_2026-07-14.md`
for the XAU_4h_DonchN20 mimic book, then stress it harder, BEFORE any live wiring.
No engine/live code touched. Judged STANDALONE (companion rule respected).

**Harness:** repo `backtest/survivor_mimic_intrabar_bt.cpp` rebuilt clean
(`clang++ -std=c++17 -O2 -I include`); stress runs on a scratchpad-only extended
copy (`surv_mimic_rest_stress.cpp` — adds fill-assumption knobs slip/thru,
calendar 2022-vs-2023+ split, REST parameter neighborhood; eval logic otherwise
byte-identical, verified by exact row-for-row parity with the repo binary).

**Data (all integrity-gated this session, no exceptions):**
- `/Users/jo/Tick/XAUUSD_2022_2026.h4.csv` — CERTIFIED CLEAN
- `/Users/jo/Tick/NSXUSD_2022_2026.h4.csv` — CERTIFIED CLEAN
- `/Users/jo/Tick/NDX_daily_2016_2026.csv` — CERTIFIED CLEAN
- XAU M1 stitch (1,545,661 bars, 2022-01-02..2026-06-26) re-staged via
  `stage_certified_data.sh` (sha e5bae8a3…) — CERTIFIED CLEAN, stamp written.

## 1. Reproduction — EXACT

Parent parity n=445 closed trades (target 445). Every prior-study row reproduces
to the decimal:

| model | n | net%/leg | PF | DD | WF H1/H2 | SMA bull/bear | verdict |
|---|---|---|---|---|---|---|---|
| CLOSE-M (study parity) | 60 | +14.6 | 2.05 | −3.2 | +3.9/+10.7 | +10.5/+4.1 | PASS (parity ✓) |
| LIVE-F (shadow will print) | 98 | +38.0 | 2.78 | −5.5 | +17.2/+20.9 | +27.8/+10.2 | overstated 3–4× ✓ |
| MKT-F (market-at-close) | 98 | +10.1 | 1.47 | −4.5 | **−1.8**/+11.9 | +7.2/+2.8 | **FAIL WF-H1 ✓ (confirmed)** |
| **REST-F/M1 (decision-grade)** | 98 | **+11.3** | **1.58** | **−5.3** | +6.6/+4.7 | +9.1/+2.2 | **PASS all-6 ✓ (reproduced)** |
| REST-F/M1 2× cost | 98 | +6.4 | 1.31 | | | | PASS (marginal) ✓ |

Prior claim (+11.3%/leg PF1.58 DD−5.3, all-6 + 2×-cost PASS; MKT WF-H1 −1.8 FAIL)
is **confirmed exactly**. Resting-order execution requirement stands: market-at-close
still fails.

## 2. Validation battery beyond reproduction

| battery item | result | PASS/FAIL |
|---|---|---|
| 2× cost (fills-only, rt 5→10bp) | +6.4%, PF **1.31** — right at the 1.30 line | **PASS (marginal, no headroom)** |
| WF halves | H1 +6.6 / H2 +4.7 | PASS |
| Regime, SMA200 split (study convention) | bull +9.1 (n73) / bear +2.2 (n25) | PASS (bear thin) |
| **Regime, calendar 2022 vs 2023+** | **2022 = −1.9% (n17, PF 0.68) / 2023+ = +13.2% (n81, PF 1.96)** | **FAIL (2022 leg negative)** |
| Fill trigger: require 1–2bp penetration through level (thru) | +11.2 / +11.1 (vs +11.3); n 98→96 | PASS — touch assumption NOT load-bearing |
| Fill price: adverse slip 1bp/fill | +10.1 PF1.50 (2× +5.2 PF1.24) | PASS |
| Fill price: adverse slip 2bp/fill | +8.2 PF1.40 (2× +3.3 PF1.15) | PASS at 1×; 2× PF degrades |
| Fill price: adverse slip 3bp/fill | +4.9 PF1.22, all-6 FAIL; 2× net −0.0 | **FAIL** |
| Combined slip2bp + thru1bp | +7.2 PF1.35 (2× +2.4 PF1.11) | PASS at 1× |
| Param: gb ±20% (0.08/0.12) | +11.6 / +11.3 — inert | PASS (plateau) |
| Param: lc −20% (1.6) | +6.8–7.0, SMA-bear leg −1.2..−1.4 | **FAIL corner** |
| Param: lc +20% (2.4) | +8.9–9.2 PF1.41 (2× PF1.17) | PASS at 1× |
| Param: arm −20% (0.20) | +7.1 PF1.35 (2× PF1.10) | PASS (degraded) |
| Param: arm +20% (0.30) | +12.7 PF1.65 (2× PF1.37) | PASS (better) |
| Param: cap 24 / 36 | +10.5 / +13.3 | PASS |

Notes:
- **Slip context:** tape median spread 0.28 on median px 2317 ≈ 1.2bp. The 5bp rt
  cost already covers commission+spread; slip here is EXTRA per level fill (stop
  orders go market on trigger). Realistic 0.5–1.5bp/fill → honest expectation
  **~+9–10%/leg PF≈1.5**. The classic resting-order lie (fill-at-touch) is NOT the
  weak point here — triggers survive penetration requirements untouched; only fill
  PRICE slippage bites, and the edge dies at 3bp/fill.
- **Parameter surface:** plateau, not cliff — but asymmetric. Deployed config is
  not the local optimum (arm 0.30 and cap 36 both dominate it incl. at 2×-cost);
  the lc 1.6 corner fails. No re-tune proposed (would need its own full study).

## 3. NEW adverse finding — the 2022 calendar leg is negative in every executable model

| model | cal-2022 net (n17*) | cal-2023+ net |
|---|---|---|
| CLOSE-M (close-grade study) | +0.6 (n10) | +14.0 |
| LIVE-F (level-fill fantasy / shadow print) | +4.0 | +34.0 |
| MKT-F | −2.3 | +12.4 |
| FINE-F/M1 | −0.3 | +9.7 |
| **REST-F/M1** | **−1.9 (PF 0.68)** | **+13.2 (PF 1.96)** |
| every slip/thru/param variant | −1.1 .. −2.8 | +7.7 .. +15.2 |

(*n14 for matched-window rows.) Only the level-fill models (which the shadow ledger
mimics and which overstate 3–4×) show 2022 positive. At executable truth the entire
edge is 2023+ gold-bull concentrated; the 2022 chop-bear year loses ~−2%. The
all-6 "bear+" pass rides on the SMA200 regime definition (+2.2 on n25, itself thin);
under the calendar 2022-vs-2023+ reading the house both-regimes gate **fails**.
Sample is thin either way (n17) — this is a concentration caveat, not a kill.

## 4. VERDICT — **AMBER (shadow with resting-order exec; NOT green for live sizing yet)**

One line: the +11.3/PF1.58 resting-order figure reproduces exactly and survives
realistic fill stress, but the edge is 100% 2023+-concentrated (2022 leg −1.9%),
and 2×-cost PF sits exactly at 1.30 with zero slippage headroom (1bp/fill → 2× PF
1.24; 3bp/fill → dead) — too fragile for a GREEN.

Specifically:
1. **Confirmed:** if the flip ships, it MUST be resting-order exec (market-at-close
   fails WF-H1 −1.8, confirmed). Size against +9–10%/leg PF≈1.5 (slip-adjusted),
   worst leg −2.05%, DD −5.3%, NOT the shadow ledger's ~+38 level-fill print.
2. **Blocking for GREEN:** (a) calendar-2022 leg negative in every executable model
   → both-regimes gate fails on the calendar reading; (b) joint stress (2×-cost +
   ≥1bp slip) drops PF below 1.3 — the gate pass has no margin.
3. **Path to GREEN:** run it SHADOW with the resting-order exec path wired and judged
   on real fills for a meaningful forward sample, and/or operator explicitly accepts
   the SMA-split regime definition + 2023+ concentration as a sized-down bull-book.
   (Diagnostic only, needs own study: arm 0.30 / cap 36 dominate the deployed cell
   incl. 2×-cost — consistent with the prior study's arm=0.5 probe.)

*Runs: scratchpad `repro_m1.txt` (repo binary, parity) + `stress_m1_v2.txt`
(extended battery). Session c845acd7, 2026-07-14. Not committed per instruction.*
