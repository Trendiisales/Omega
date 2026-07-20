# Survivor + NAS100-Ladder + RegimeAdaptor — FAITHFUL revalidation (S-2026-07-21)

Scope: g_survivor (SurvivorPortfolio), the NAS100 mimic-ladder profit-lock
(FxMimicLadderCompanion `wide_arm_pct=0.0` shipped d93cd171), and g_regime_adaptor.
Faithful only. **No wire, no commit — findings + verdicts only.**
All three are **SHADOW** books/layers (deploy-forward), so "KEEP" = keep running as shadow.

Pre-flight gates (registry §1):
- Enabled cross-check: `g_survivor.enabled=true` (engine_init L4746, GATED); index ladder wired
  SHADOW (L2192+), NAS100 `wide_arm_pct=0.0` live since d93cd171 (L2300).
- Data-integrity gate: `XAUUSD_2022_2026.h4.csv`, `NSXUSD_2022_2026.h4.csv`,
  `NSXUSD_2022_2026.h1.csv` all **CERTIFIED CLEAN**.

---

## 1. g_survivor — SurvivorPortfolio (SHADOW, bear-gated)

Harness: `backtest/survivor_gated_bt.cpp` (drives the REAL `omega::survivor::Portfolio`,
prod cells, `dedup_mode=1`, live `entry_veto` hook) over 2022-2026 XAU + USTEC(NSXUSD) H4
CERTIFIED tapes, veto seeded from NDX daily 2016+. **Reproduces the wired cert byte-close.**

Live config = **USTEC+XAU long-veto + USTEC_4h_RSI_N7 culled** (exactly engine_init L4746-4772):
- Portfolio: **n=445 PF 1.70 net +$11,065** (x1) · BULL PF 1.89 +$9,780 · **BEAR-2022 PF 1.90 +$1,694**
  · WF **both halves +** (H1 +$760 / H2 +$10,306) · **2x-cost PF 1.66 +$10,549** (holds) · top3 38%.

### Per-cell (live gated config)

| Cell | n | WR | PF (x1) | net (x1) | 2x-cost | Protection | Verdict |
|------|--:|---:|--------:|---------:|--------:|-----------|---------|
| **USTEC_4h_ZMR** | 84 | 44.0% | **1.87** | **+$9,352** | portfolio PF1.66 | ATR SL 1.0× + TP 2.0×ATR + max_hold30 + z-revert exit | **KEEP** (shadow) — workhorse, strongly + standalone |
| **XAU_4h_DonchN20** | 361 | 36.8% | **1.34** | **+$1,713** | portfolio PF1.66 | Donchian ATR SL 1.5× + TP 3.0×ATR + max_hold30 + reclaim_exit | **KEEP** (shadow) — + both cost tiers |
| USTEC_4h_RSI_N7 | (562) | 33.6% | **0.99** | **−$1,022** | negative | RSI7 ATR SL 1.0× + TP 2.0×ATR | **KEEP-DISABLED / SCRAP** — `st.enabled=false` correct; do NOT re-enable |

Worst trade: XAU_4h_DonchN20 ≈ **−143 pt = −$143** (0.01 lot); USTEC_4h_ZMR worst ≈ **−$495** (0.10 lot).
Both active cells net-positive standalone; portfolio positive in BOTH regimes and BOTH WF halves.

**PROTECTION verdict (survivor cells): profit-lock N/A — these are FIXED-TP engines, not runners.**
Each trade closes at a hard `tp_mult×ATR` TP, `sl_mult×ATR` SL, `max_hold_bars` timeout, or
reclaim (Donchian). There is no trailing runner leg, so the "gave-it-all-back / peak-giveback"
failure mode does not apply — the ATR stop IS the in-flight protection and is already backtested
(portfolio bear PF1.90+, 2x-cost holds). No giveback-stop retrofit warranted.

Note: the separate GoldTrendMimicLadder 1-leg overlay that mimics XAU_4h_DonchN20 is **PAUSED**
(engine_init L1933, S-20ae honest-recert) — that is a different companion engine, not the survivor cell.

---

## 2. NAS100 mimic-ladder profit-lock (arm0, shipped d93cd171) — SHADOW

Engine: `FxMimicLadderCompanion` via `index_mimic_ladder_book()`, NAS100 cell.
Mechanism: WIDE/STACKED/LADDER runner legs use peak-profit trail
`stop = entry + (1−wide_gb_frac)·(peak−entry)`, `wide_gb_frac=0.10` (keep 90% of peak),
engaged at `wide_arm_pct` MFE. Shipped d93cd171: **`wide_arm_pct=0.0` for NAS100** = engage the
10%-giveback lock **FROM ENTRY** (US500/GER40/M2K keep 0.5% arm).

Harness: `backtest/ladder_wide_trail_tighten_sweep.py` over `NSXUSD_2022_2026.h1.csv`
(24,407 bars, CERTIFIED CLEAN) + `NAS2022_bear_h1.csv`. **Reproduces the cert row byte-exact:**

| cell | arm | gb | net% | dNet | PF | WF | RT% | cap% | 2x | bear2022 |
|------|----:|---:|-----:|-----:|---:|:--:|----:|-----:|----:|---------:|
| baseline (2.7·thr/g50) | 4.05 | .50 | +245.7 | — | 1.20 | + | 0.20 | 62.9 | +172.4 | −12.4 |
| **shipped arm0/gb10** | **0.0** | **.10** | **+330.1** | **+34.4%** | **1.35** | **+** (H1+105/H2+225) | **0.04** | **74.9** | **+256.9** | **+0.4** |

Matches `outputs/LADDER_WIDE_TRAIL_TIGHTEN_2026-07-09.md` L54 exactly. Round-trips cut 0.20%→0.04%,
capture 62.9%→74.9% (runner legs 90% by construction), 2x-cost strongly +, bear window −12.4→+0.4.

**CONFIRM: d93cd171 IS the certified config.** `if (ic.tag=="NAS100") c.wide_arm_pct=0.0` sets
precisely the arm0/gb10 row above.

**PROTECTION verdict: certified profit-lock present and correct.** Stack on the NAS100 cell:
arm0 90%-of-peak lock **from entry** (`wide_gb_frac=0.10`) + `be_floor_on_open=true` +
BE-ENTRY (`be_entry_pct=0.08 ≥` true RT 0.03%) + `LOSS_CUT 5·thr` + weekend-gap gate
(`block_weekend_arms`, `weekend_carry_frac=0.0`). Every runner clip locks 90% of peak; no ride-back-to-BE.
Verdict: **KEEP** (shadow). *(Per the S-17f honest-framing correction, `be_floor_on_open` is a
config property — it REDUCES not eliminates a gap-through tail; the cert here is the python model.
This does not change the KEEP verdict; the edge survives at 2x-cost.)*

---

## 3. g_regime_adaptor — RegimeAdaptor: **GATE, not a book — no backtest**

`include/OmegaRegimeAdaptor.hpp` class `RegimeAdaptor` (`g_regime_adaptor.enabled=true`,
engine_init L6758). It **opens no positions** — it is a regime weighting/gating LAYER consumed by
other engines: `weight()` (per-class regime multiplier), `vol_size_scale()`, `equity_blocked()`,
`tp_vol_mult()`, `long_blocked()` (call sites: on_tick.hpp L304/1339/1430, trade_lifecycle.hpp
L1411/1483/1832/1855, tick_gold.hpp L567). No opener idiom (`.active=true`/`.open(`) exists in the
header. **No standalone book to backtest — correctly out of scope.** Verdict: **KEEP as gate (no BT applicable).**

---

## Summary verdicts
- **USTEC_4h_ZMR** — KEEP (shadow): PF1.87 +$9,352, both regimes +, protection = ATR SL (fixed-TP, no runner).
- **XAU_4h_DonchN20** — KEEP (shadow): PF1.34 +$1,713, protection = ATR SL + reclaim (fixed-TP, no runner).
- **USTEC_4h_RSI_N7** — KEEP-DISABLED/SCRAP: standalone PF0.99 −$1,022; `st.enabled=false` correct.
- **NAS100 ladder arm0 (d93cd171)** — KEEP (shadow): CONFIRMED certified (+330.1%/+34.4%, PF1.35, WF+, bear+0.4, 2x+256.9); profit-lock = arm0 90%-peak from entry + BE-floor + LC5thr + weekend gate.
- **g_regime_adaptor** — GATE not a book (no opener); no backtest; keep as gate.

Portfolio + ladder are SEPARATE INDEPENDENT SHADOW books judged standalone (never touch a real position).
