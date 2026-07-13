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

---

# BEAR-GATE VARIANT (operator go: wire resting-order + 1 MGC live WITH a regime gate)

## 0. The live mechanic, read from code (what the backtest replicates)

`include/GoldTrendMimicLadder.hpp`:
- **Gate point = trigger/spawn time ONLY.** `Config.bull_only` is checked once, in
  `on_trend_open` (L97: `if (cfg_.bull_only && !bull_) return;`) — a bear trigger
  spawns NO legs. Once spawned, **nothing cancels a pending or entered leg on a
  regime flip** — there is no re-check anywhere in `on_h1_bar`. Faithful model =
  gate-at-trigger, holds-through. (Variant (b) answered from code: NO pending-cancel.)
- **`bull_` state feed:** set from the `bull` argument of
  `on_h1_bar(h,l,c,ts,bull)` (L121). Config comment documents the semantic as
  "H1 close above SMA200".
- **WIRING GAP (must fix at the flip):** the survivor books are fed their NATIVE H4
  bar via `GoldTrendMimicRegistry::on_bar` (L300–304), which **hardcodes
  `bull=true`**. Setting `bull_only=true` today is a NO-OP on this path. The wire
  must deliver a real regime flag to the book (see §3).
- Gate timing convention: `bull_` at trigger = the flag from the last feed bar at or
  before the parent's H4-close trigger, SMA inclusive of the just-closed bar. The
  backtest gates at exactly that point.

## 1. Gated results — REST-F/M1, XAU_4h_DonchN20 (same harness, certified data, 1× = 5bp rt)

| variant | n | net%/leg | PF | DD | WF H1/H2 | cal-2022 | cal-2023+ | 2× net/PF | worst leg |
|---|---|---|---|---|---|---|---|---|---|
| ungated (part 1 ref) | 98 | +11.3 | 1.58 | −5.3 | +6.6/+4.7 | **−1.9** (PF0.68) | +13.2 | +6.4/1.31 | −2.05 |
| G-H4-SMA200 (native-feed grade) | 73 | +9.1 | 1.63 | −3.9 | +8.1/**+1.1** | −0.0 (n6, PF0.99) | +9.1 | +5.5/1.36 | −2.05 |
| **G-H1-SMA200 (Config-documented) — RECOMMENDED** | **67** | **+13.9** | **2.79** | **−2.6** | **+7.5/+6.4** | **+0.3** (n6, PF1.14) | **+13.6** | **+10.5/2.22** | −2.05 |
| G-H4-SMA100 (plateau check) | 70 | +13.5 | 2.29 | −2.6 | +8.5/+5.0 | +0.3 | +13.2 | +10.0/1.88 | −2.05 |
| G-H4-SMA300 (plateau check) | 74 | +7.0 | 1.46 | −4.1 | +6.3/+0.7 | −2.0 | +8.9 | +3.3/1.20 | −2.05 |
| G-H4-200 + pending-cancel (variant b) | 72 | +8.8 | 1.61 | −3.9 | +8.1/+0.8 | −0.0 | +8.8 | +5.2/1.34 | −2.05 |
| G-H1-200 + slip 1bp/fill | 67 | +13.2 | 2.63 | −2.7 | +6.9/+6.3 | +0.2 | +13.0 | +9.8/2.10 | −2.06 |
| G-H1-200 + slip 2bp/fill | 67 | +11.9 | 2.42 | −2.8 | +6.2/+5.7 | +0.1 | +11.8 | +8.5/1.91 | −2.07 |
| G-H1-200 + slip 3bp/fill | 67 | +9.1 | 1.90 | −2.8 | +4.1/+5.0 | −0.0 | +9.2 | +5.8/1.52 | −2.08 |

(The harness "all-6" flag is N/A on a gated book — the SMA-bear leg is empty/near-empty
by construction (n0–6); judged instead on the four criteria in §4.)

**Does the gate fix the −1.9% 2022 leg without gutting the 2023+ edge? YES.**
The H1-SMA200 gate turns 2022 from −1.9 (PF0.68) to +0.3 (PF1.14) while the 2023+
leg is fully retained (+13.6 vs +13.2) — the blocked triggers were net losers, so
full-period net RISES (+13.9 vs +11.3), PF nearly doubles (2.79 vs 1.58), DD halves
(−2.6 vs −5.3). It also fixes part 1's two blockers: 2×-cost PF 2.22 (was 1.31, at
the line) and slip robustness (3bp/fill now +9.1 PF1.90 / 2× +5.8 PF1.52 — the
ungated book was DEAD there).

**Plateau, not a spike:** the response is monotone in filter responsiveness —
H1-SMA200 (~8d) ≈ H4-SMA100 (~17d) both strong; the faithful-but-slow H4-SMA200
(~33d) still neutralizes 2022 (−0.0) at +9.1/PF1.63 (its WF-H2 +1.1 is the weak
spot); H4-SMA300 (~50d) is too slow and lets 2022 back in (−2.0). Pending-cancel
adds nothing (−0.3 net vs plain gate) — live's holds-through semantics are fine.

## 2. Caveats

- 2022 gated sample is n6 — the gate WINS 2022 by mostly not trading it, not by
  trading it well. That is exactly the proviso's intent (block bear-negative
  exposure), but the bear-regime edge remains unproven.
- The tiny SMA-bear residue on the H1 gate (bear −0.2, n6) is H1-vs-H4
  regime-definition mismatch trades; immaterial.
- H1 gate series here was aggregated from the certified M1 tape; live computes it
  from the H1 feed (same closes). First ~200 H1 (~9 days, Jan-2022) forced-bear in
  the backtest; live warm-seeds so day-1 is well-defined.

## 3. Recommended live wire config (exact fields)

1. `engine_init.hpp` XAU_4h_DonchN20 book — add ONE field to the existing Config
   block (all other fields unchanged: legs {{"T",0.10}}, arm_pct 0.25, lc_pct 2.0,
   cap_bars 30, rt_cost_bp 5.0, be_entry_pct 1.0, pend_bars 6):
   `c.bull_only = true;`
2. **REQUIRED plumbing (without it the gate is inert):** `GoldTrendMimicRegistry::on_bar`
   hardcodes `bull=true` (GoldTrendMimicLadder.hpp L303). Deliver the real flag per
   the Config's documented semantic — **H1 close > SMA200(H1 closes, inclusive of the
   just-closed bar)** — e.g. a `set_bull(tag, bool)` / regime call on the registry fed
   from the shared XAUUSD H1 stream, updating the book's `bull_` each H1 close. Gate
   evaluation stays where it is (on_trend_open); no pending-cancel needed (§1).
   Warm-seed 200 H1 closes so the flag is defined at boot ([SEED] mandate).
3. **Resting-order execution** (part 1, unchanged): entry stop at the be level,
   trail/lc as resting stops updated at H4 closes. Market-at-close remains FAIL.

## 4. PASS/FAIL vs house gate (gated decision-grade = G-H1-SMA200 REST-F/M1)

| criterion | figure | verdict |
|---|---|---|
| net positive | +13.9%/leg (n67, 4.5yr) | PASS |
| PF ≥ 1.3 at 2×-cost | 2× +10.5%, PF 2.22 (≥1.5 even at 3bp/fill slip) | PASS |
| both WF halves + | +7.5 / +6.4 (positive at every slip level) | PASS |
| 2022 leg not materially negative | +0.3 (PF 1.14, n6) vs ungated −1.9 | PASS |

**VERDICT: PASS — GREEN for the live wire as specified (resting-order exec +
bull_only H1-SMA200 gate + the registry bull-flag fix).**
Size 1 MGC against the gated decision-grade figures: **+13.9%/leg PF2.79 DD−2.6%,
worst leg −2.05% (lc-bounded), n67 ≈ 15 legs/yr; slip-adjusted honest expectation
+12–13%/leg PF≈2.4–2.6**. Never against the shadow ledger's level-fill print.
MGC venue note (registry §7): MGC commission ≈0.31pt RT is BELOW the 5bp (~1.6pt)
this backtest debits — venue cost is margin in our favor; stop-fill slip is the
honest residual risk and is covered to 3bp/fill above.

*Gate runs: scratchpad `gate_m1_v2.txt` (extended harness `surv_mimic_rest_stress.cpp`,
parity-locked to the repo binary; gate replicates on_trend_open/bull_ semantics
read from GoldTrendMimicLadder.hpp this session). Not committed per instruction.*
