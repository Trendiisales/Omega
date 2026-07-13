# COMPANION-LADDER SIZING PROPOSAL — S-2026-07-14 (ANALYSIS ONLY, operator sign-off required)

**Operator go:** "resize if viable and advise." No config/code changed by this analysis.
Closes the OPEN memory item `project-revisit-lot-sizes` ("$10k placeholder; resize to real
contract value (1 MNQ etc) once stable; operator decides size").

---

## 0. Where the $10k placeholder lives (verified in source + live state)

| engine / book | file : line | notional today | live state confirms |
|---|---|---|---|
| GoldTrendMimicLadder — ALL 9 trigger books (XauTf4h 4-leg, XauTf2h 2-leg, MgcFastDon 2-leg, XauTfD1 2-leg, NAS100/US500/DJ30 Turtle 2-leg, XAU_4h_DonchN20 1-leg, USTEC_4h_ZMR 1-leg) | `include/GoldTrendMimicLadder.hpp:64` (`notional = 10000.0` class default; never overridden in `engine_init.hpp:1619-1683`) | $10,000/clip | no goldmimic state files on box yet (0 clips forward) |
| FX up-jump ladder — GBPUSD (only enabled pair) | `include/FxUpJumpLadderCompanion.hpp:102` default; wired $10k at `engine_init.hpp:1862` | $10,000/clip | `fxladder_companion_state.json` `"notional":10000`, 0 clips |
| INDEX up-jump ladder — US500/NAS100/GER40/M2K | same class default (no `c.notional` set in `engine_init.hpp:1975-2012`) | $10,000/clip | `idxladder_companion_state.json` `"notional":10000` ×4, 0 clips (reset 2026-07-13) |
| Stock day-mover ladder (39 BIGCAP names) | `StockDayMoverLadderCompanion.hpp:172` default, **already rescaled** `engine_init.hpp:2162` (`×4` whole book) + `:2175` (`×2` elite) | $40k / $80k/clip | operator-directed S-2026-07-11; NOT part of this proposal |
| BigCap 2% impulse companion / StockDip / StockTurtle | `BigCap2pctImpulseCompanion.hpp:109`, `engine_init.hpp:2295, 2422` | $10,000 | shadow stock books; out of scope (stock basket P&L fixed separately S-2026-07-14k) |

The memory-note example, re-marked at today's futures scale: a 291-pt NAS100 move at
NQ≈29,400 is ~0.99% → **$99 on the $10k placeholder vs $582 at 1 MNQ** ($2/pt). The
percentage is honest; the dollar display is ~1/6 of one micro contract.

## 1. Forward record status (read before trusting any $ figure below)

Read-only pull from **omega-new** (45.85.3.79, HEAD `cfe10947`), 2026-07-14:

- **Shadow ledger** `C:\Omega\logs\trades\omega_trade_closes.csv`: **zero GMIMIC / mimic rows**.
  No `[GMIMIC]` lines in stdout/stderr logs, no `goldmimic_*` state files anywhere on the box.
- **Ladder companions**: `fxladder_companion_state.json` GBPUSD **0 clips** (1 window active,
  1 arm); `idxladder_companion_state.json` all four index books **0 clips** (books reset
  2026-07-13; deployed forward only days).

**Therefore every performance figure in this proposal is BACKTEST-derived**, primarily
`backtest/SURVIVOR_MIMIC_INTRABAR_RECHECK_2026-07-14.md` (decision-grade REST-F/M1 model)
and the wiring-comment evidence blocks in `engine_init.hpp`. There is no realized per-leg
forward sample yet. Reference prices marked 2026-07-14 from live VPS dumps:
**XAU ≈ $4,006/oz, NQ ≈ 29,400, GBPUSD ≈ 1.335, M2K ≈ 2,964** (re-mark at flip).

## 2. Real instrument mapping + contract specs

| book symbol | instrument | multiplier | notional @ today | margin (approx, verify at flip) | RT cost real |
|---|---|---|---|---|---|
| XAUUSD | **MGC** (COMEX Micro Gold) | 10 oz → $10/pt ($1.00/oz) | ≈ **$40,060** | ~$2.5–3.5k overnight (CME, scales with vol) | ~$1.04 comm + 1–2 tick spread ≈ **$2–3 (~0.6 bp)** |
| XAUUSD (alt) | IBKR spot gold CFD, fine-grained (e.g. 5 oz) | $1/oz per oz | $4,006/oz | CFD margin ~5% | 2×0.015%×4006 = $1.20/oz + ~$0.30 spread ≈ **$1.50/oz (~3.7 bp)** |
| USTEC.F | **MNQ** (CME Micro Nasdaq) | $2/pt | ≈ **$58,800** | ~$2.5–3.5k overnight | ~$1.04 comm + 2×0.25 spread ≈ $2 (~0.3 bp) |
| US500.F | MES ($5/pt) | $5/pt | ≈ $34k | ~$2.5k | similar micro costs |
| M2K | M2K ($5/pt) | $5/pt | ≈ $14.8k | ~$1k | similar |
| GER40 | FDXM (€5/pt) | €5/pt | ≈ €120k — **too big**; smallest real increment is chunky | high | — |
| GBPUSD | IBKR IDEALPRO, min practical clip 25,000 GBP | — | ≈ **$33,400** | ~3% | 0.2 bp comm ($2 min) + ~0.3 pip |

**Cost-basis rule check (memory `project-ibkr-cost-basis`):** correct XAU RT =
`2×0.00015×price + spread` = 2×0.00015×4006 + $0.30 ≈ **$1.50/oz ≈ 3.7 bp** (the old
COST_RT_PTS=0.37 was 4× low). The XAU mimic backtest debited **5 bp RT** and also passes at
**2×cost = 10 bp** (REST-F/M1 +6.4% PF1.31 PASS). So the proposed sizing survives the
correct cost basis with ~2.7× headroom on CFD execution, and ~8× headroom on MGC futures
(0.6 bp real). No book below is being sized off an under-costed backtest.

## 3. Per-book proposal

### 3a. XAU_4h_DonchN20 survivor mimic — the ONLY book eligible now → **GO at 1 MGC, conditional**

Decision-grade figures (REST-F/M1, `SURVIVOR_MIMIC_INTRABAR_RECHECK_2026-07-14.md`):
**+11.3%/leg-notional over 4.3y, PF 1.58, maxDD −5.3%, worst leg −2.05%, worst MAE −2.18%,
n=98 legs (~23/yr), 2×cost +6.4% PF1.31 PASS.** lc=2% pre-arm backstop is live and doing
real work (6 lc exits).

| size | notional | expected net (4.3y BT) | ~ per yr | 2×cost net | **max DD** | worst single leg | lc bound/leg |
|---|---|---|---|---|---|---|---|
| $10k placeholder (today) | $10,000 | +$1,130 | ~$263 | +$640 | **−$530** | −$205 | −$200 |
| **1 MGC (proposed)** | $40,060 | **+$4,527** | ~$1,053 | +$2,564 | **−$2,123** | −$821 | −$801 |
| 2 MGC | $80,120 | +$9,054 | ~$2,106 | +$5,128 | −$4,247 | −$1,642 | −$1,602 |
| 3 MGC | $120,180 | +$13,580 | ~$3,159 | +$7,692 | −$6,370 | −$2,464 | −$2,404 |
| alt: 5 oz CFD (half-micro) | $20,030 | +$2,263 | ~$527 | +$1,282 | −$1,062 | −$410 | −$400 |

**HARD CONDITIONS (from the intrabar re-check — non-negotiable):**
1. **Resting-order execution required.** The current exec wiring (`send_live_order` market
   order at the H4-close event) FAILS WF-H1 (−1.8). The flip must place the BE-entry as a
   resting stop at the be level and trail/lc as resting stops updated at H4 closes. If the
   flip keeps market-at-close, this book is **NOT ready** at any size.
2. Size against **+11.3%/PF1.58/DD−5.3%**, never the close-grade +14.6% and never the
   shadow ledger print (level fills overstate ~3–4×).
3. Zero forward clips exist. Reasonable to require the first N shadow clips to land in the
   backtest envelope before (or in parallel with) the flip.

### 3b. USTEC_4h_ZMR survivor mimic — **NO-GO (reference only; being DISABLED today)**

Intrabar FAIL: REST-F/M1 +8.3% PF1.21, **bull leg −7.9%**, 2×cost PF1.15; 11/12 gb/arm
cells fail. For the table only, at 1 MNQ (notional ≈ $58.8k): BT net would read +$4,880,
but **maxDD −6.0% → −$3,528** and the bull-regime leg alone −$4,645. Salvage path = a
validated bear-gate study (own full validation first). **Excluded from live sizing.**

### 3c. Gold trend mimics (XauTf4h, XauTf2h, MgcFastDon, XauTfD1) — **NO-GO yet (intrabar re-check owed)**

Close-grade PASSes only (+110/+77–88/+34/+24 %/leg; `GoldTrendMimicLadder.hpp` header:
"intrabar re-check owed before LIVE sizing"). The survivor re-check just demonstrated
close-grade can overstate 25% (gold) or flip to FAIL (USTEC). **Keep $10k shadow.** If they
pass their own intrabar re-check, the same 1-MGC mapping applies — noting XauTf4h runs
**4 legs**, so 1 MGC/leg = up to 4 MGC ≈ $160k notional when all legs are armed
(DD figures must be re-derived per-book before that flip).

### 3d. Index D1 turtle mimics (NAS100/US500/DJ30) — **NO-GO yet** (close-grade only, same
re-check owed). Keep $10k shadow. Future mapping: MNQ/MES/MYM micros.

### 3e. INDEX up-jump ladders (US500/NAS100/GER40/M2K) — **NO-GO / keep $10k shadow**

Books were reset 2026-07-13 and have **0 forward clips**; NAS100 carries a 7-month source
data caveat ("forward record decides promotion", `engine_init.hpp:1927-1931`); the M2K
retune still owes the random-window control. Worst-case math that must be priced before any
promotion: per-clip pre-arm LOSS_CUT = 5×thr → NAS100 7.5% of clip = **−$4,410 at 1 MNQ**
(vs −$750 at $10k). Weekend-gap tail is capped (~$0 carry, LAYER 3). Recommendation: leave
at $10k until a forward record exists, then size micros with the LC5thr figure on the table.

### 3f. FX ladder GBPUSD — **conditional candidate, not yet**

All-6 PASS on 3y real IBKR feed (+40.5% PF1.44 n526, 2×cost +30.0% holds, weekend layers
free), but 0 forward clips since wire and no quoted maxDD in the evidence block (only the
per-clip LC bound = 5×0.75% = 3.75% → **−$1,252/clip at 25k GBP**, ≈$33.4k notional; BT net
would map to ~+$13.5k/3y ≈ $4.5k/yr). Recommendation: hold $10k shadow for a first batch of
forward clips; if promoted, smallest real increment = **25,000 GBP** (IDEALPRO practical
minimum). A maxDD extraction from the sweep artifacts is owed before sign-off.

## 4. Account context (paper 4002, read-only)

- System sizing anchor `g_cfg.account_equity` **defaults to $10,000** (`omega_types.hpp:109`)
  and no override was found in logs (no `[SIZING] account_equity=` lines in current stderr).
- Actual IBKR **paper-4002 NetLiquidation was NOT visible** in mirrored logs/telemetry
  (no order placed, no API call made — read-only per audit rule). Operator should read NLV
  off the Gateway before the flip.
- Fit check at the proposed 1 MGC: margin ~$3.5k and worst-case DD −$2.1k. Against the $10k
  internal anchor that is 21% DD — aggressive but consistent with the operator's existing
  live posture (the one real position today is 1 MGC micro). If the anchor (not the paper
  NLV) is the true risk base, the 5-oz CFD alternative halves everything.

## 5. Summary table (the ask: books × proposed size × worst-case $ DD × recommendation)

| book | proposed size | worst-case $ DD (BT) | worst single leg | recommendation |
|---|---|---|---|---|
| **XAU_4h_DonchN20 mimic** | **1 MGC** (10 oz, ≈$40k notional) | **−$2,123** (−5.3%) | −$821 | **GO — conditional on resting-order exec path; expected ~+$1,050/yr at BT rate, 2×cost-proof.** Alt: 5 oz CFD (−$1,062 DD) if sizing to the $10k anchor. |
| USTEC_4h_ZMR mimic | (1 MNQ reference ≈$58.8k) | −$3,528 (−6.0%); bull leg −$4,645 | −$1,500est | **NO-GO — intrabar FAIL, being disabled today. Reference only. Bear-gate study = salvage path.** |
| XauTf4h / XauTf2h / MgcFastDon / XauTfD1 mimics | keep $10k shadow | n/a at real size | — | **NO-GO yet — intrabar re-check owed (survivor re-check proved close-grade unreliable). Re-check first, then 1 MGC/leg mapping.** |
| NAS100/US500/DJ30 turtle mimics | keep $10k shadow | n/a | — | **NO-GO yet — same intrabar re-check owed.** |
| Index up-jump ladders (US500/NAS100/GER40/M2K) | keep $10k shadow | LC5thr at 1 MNQ = −$4,410/clip (NAS) | — | **NO-GO — 0 forward clips since 07-13 reset; NAS data caveat; forward record decides promotion.** |
| FX ladder GBPUSD | keep $10k shadow → 25k GBP if promoted | LC = −$1,252/clip @25k; maxDD extraction owed | — | **HOLD — all-6 PASS on real feed but 0 forward clips + no quoted maxDD. Quantify DD + first forward clips, then 25k GBP.** |
| Stock ladder (39 names) | already ×4/×8 (operator S-2026-07-11) | per prior op decision | — | no change proposed here |

## 6. Implementation notes for the eventual flip (NOT done now)

1. Set `c.notional` per book in `engine_init.hpp` (MGC: `10 × spot` ≈ 40,000; keep a
   comment tying it to the contract, not a magic number) and `c.lot` to the real order
   size. **Caveat:** `book_usd_real()` = Σret × notional, so changing notional re-prices
   the displayed *historical* forward record too — either reset the book state at flip
   (as done for the 07-13 ladder reset) or accept the re-priced history; do not let a
   placeholder-era % print as real-size dollars (`feedback-no-backtest-in-live-gui`).
2. XAU mimic flip additionally requires the **resting-order exec path** (recheck doc §7
   follow-up (a)) — a code change with its own validation, prerequisite to GO.
3. Re-mark notionals at flip-day prices; this doc's dollars are at XAU $4,006 / NQ 29,400.

*Sources: `include/GoldTrendMimicLadder.hpp`, `include/FxUpJumpLadderCompanion.hpp`,
`include/StockDayMoverLadderCompanion.hpp`, `include/engine_init.hpp` (wiring blocks
1611-2056), `backtest/SURVIVOR_MIMIC_INTRABAR_RECHECK_2026-07-14.md`, live-box read-only
pulls 2026-07-14 (omega-new, HEAD cfe10947). ANALYSIS ONLY — nothing changed, nothing
committed.*
