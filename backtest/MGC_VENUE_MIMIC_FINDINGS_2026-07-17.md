# MGC-venue BE-floored mimic books — certification study (S-2026-07-17)

**Task:** certify (or kill) floored-on-open `GoldTrendMimicBook` companion books for the
three uncovered MGC-venue gold parents: **MgcTF4h**, **MgcSlowDonchian30m**, **MgcTF2h
(bull-gated variant only)**. Judged STANDALONE (companion independent-engine rule).

**Scope guard — NOT the retired books.** MgcFastDon mimic and XauTf2h SPOT mimic were
retired S-2026-07-17 with full-grid evidence (engine_init.hpp L1630 / L1648) and are NOT
re-tested here. The MgcTF2h study below is the **MGC venue port** of the 2h parent behind
the book-standard H1-SMA200 bull gate (`bull_only=true`) — a different basis, judged
fresh per the bull-gate-not-reject rule. GoldDon15m is operator-culled and excluded.

## Method / fidelity

- **Harness:** `backtest/clip_path_mgc_venue_mimic.cpp` (uncommitted). Mode `entries`
  drives the REAL parent engine classes with the EXACT production feed ordering
  (MgcFastDonchianFeed.hpp poll loop: fastdon → slowdon → TF on_tick l/h/c SL-first →
  H1 bucket → H4 bucket) over the certified MGC 30m file, LIVE deployed configs
  verbatim (omega_main.hpp: 4h mask 0xC9 LC1.5 VB0.30-0.85/0x8 IMP0.5 ADX15; 2h LC=0
  ADX25/0xB VB/0x4; slowdon Nin55/Nout27 sl3xATR retire−560 peer-dedup vs fastdon),
  incl. the `gold_regime()` brain (REGIME=1 fed at H1 boundaries). Mode `mimic` runs the
  REAL `GoldTrendMimicBook` header (read-only) floored-on-open: BE-ENTRY
  (`be_entry_pct=0.15` ≥ rt cost), `no_prebe_loss=true`, pend-bars cancel, arm→giveback
  trail per leg {T 0.08, W 0.20}, intrabar h→l→c management (the header's own SL-first
  bar path — NOT close-grade; the XauTf2h/MgcFastDon retirements were exactly
  close-grade artifacts).
- **Chassis sanity (4/4 reproduced before any new number was trusted):**
  1. XauTf4h floored book on `clip_path_noprebe_floor.cpp`: **ALL6-PASS net +142.1,
     worst-clip 0.00, PF 2.05** — exact match to engine_init.hpp L1626-1629.
  2. MgcTF4h parent parity: n291 +$4209.4 PF1.50 DD$1064 — EXACT vs registry §7 ref.
  3. MgcTF2h parent parity: n596 +$3533.3 PF1.23 DD$2390 — EXACT vs registry §7 ref.
  4. MgcSlowDon naked cell: n158 +$1520.7 PF1.79 mdd277 — matches cert n158 +1504.6pt
     PF1.78 (+16.1 = the 0.31→0.208 RT-debit delta). Naked 2022: PF0.44 vs deep-dive's
     "naked PF0.38". Gated 2022: n=0 (sits out, as designed).
- **Data (ALL gate-certified this session, `data_integrity_gate.py` → CERTIFIED CLEAN):**
  recent axis = `/Users/jo/Tick/mgc_30m_hist.csv` + `mgc_2024_2026.h1/h4.csv`
  (2024-06..2026-06); bear axis = SPOT bars at MGC config per registry §7 (MGC has no
  2022): `XAU2022_bear_h1.csv` (full year) + H4 aggregate
  (`/tmp/mgcmimic/XAU2022_bear_h4.csv`, gated CLEAN) + `XAU2022_m30.csv` (Jun–Sep, the
  slowdon 30m grain).
- **Ledger-first check:** 0 closed rows for `MgcTF4h_*` / `MgcTF2h_*` /
  `MgcSlowDonchian30m` anywhere in the omega-new trades dir — no live forward record
  exists for any of the three parents; backtest is the only basis.
- **Gate (operator-mandated, ALL must hold at base AND 2×):** net>0, PF≥1.3, WF halves
  both >0, both regimes (recent axis incl. within-dataset SMA200 bull/bear split + the
  2022 bear dataset). Cost basis mandated **15bp base / 30bp stress** (the deployed
  SPOT gold-book debit). A **venue-honest sensitivity at 5bp/10bp** is also reported:
  5bp is the deployed debit of the one LIVE MGC-venue mimic book (XAU_4h_DonchN20,
  engine_init L1711: "real MGC ~0.6bp + IBKR CFD ~3.7bp both under it") — 15bp of price
  is ~9-20× the real MGC futures round-trip.

**HONEST FRAMING (S-2026-07-17f rule).** Floored exits book AT the floor LEVEL — the
shipped GoldTrendMimicBook convention all 12 live books were certified under. This is a
model convention: a real gap through the floor fills worse than the level. worst/nNeg
below are at the NET level (gross − rt cost): every BE-floored clip books **net = −cost**
(worst −0.15% at base / −0.30% at stress), and nNeg counts those cost-bleed clips —
typically 55-75% of all clips. **nNeg=0 is NOT claimed and must never be.**

## Results — gate tables

Units: net/worst/mdd in % of clip notional, pooled T+W legs. Params column =
arm/lc/cap/pend/be/gbT/gbW (lc inert pre-arm in floored mode — the BE floor replaces it).
"regime-B" = the 2022 bear-dataset cells [net@base, net@2×].

### 1. MgcTF4h (priority 1) — parent stream n=288 entries (live-faithful REGIME=1)

| book | params | net | PF | worst | nNeg | WF-H1/H2 | regime-B | 2x [net, PF] | verdict |
|---|---|---|---|---|---|---|---|---|---|
| MgcTF4h floored mimic @ MANDATED 15/30bp | 0.25/1.5/12/6/0.15/{T.08,W.20} | +133.0 | 4.36 | −0.15 | 264/468 | +38.1/+94.9 | +9.5 PF1.31 PASS, **@2× −29.2 PF0.52 FAIL** | +62.8, 1.79 | **NO-GO at 15/30** |
| — full grid 128 configs | arm×cap×pend×be×gb | — | — | — | — | — | 2022@2×: **0/128 pass, grid ceiling −21** | — | universal bear@2× fail |
| — bull-gated variant (H1-SMA200) | same | +91.7 | 4.22 | −0.15 | 190/352 | +37.3/+54.4 | 2022: +1.9 PF1.10 FAIL / @2× −21.8 FAIL; **0/128 grid** | +38.9, 1.68 | NO-GO (gate vetoes the SHORT-side mimics that carry the 2022 book — wrong tool for a both-ways parent) |
| SENSITIVITY @ venue-honest 5/10bp (ungated) | same | +179.8 | 14.62 | −0.05 | 264/468 | +61.5/+118.3 | +35.3 PF4.49 (halves +18.2/+17.1, bull +18.4 / bear +16.9) / @2× +22.4 PF2.11 all + | +156.4, 6.92 | **ALL cells PASS; plateau 128/128 configs** |

**Verdict: NO-GO at the mandated 15/30bp basis** — the 2022 bear axis at 2× cost fails
every one of 128 configs (n≈258 clips × 0.30% drag vs a thin per-clip bear edge).
**However the kill is purely a cost-basis artifact for this venue:** at the venue-honest
5/10bp (the deployed convention of the live MGC book) the ungated book passes ALL cells
— recent + 2022, base + stress, halves, bull AND bear splits — on a **128/128 full-grid
plateau**, with the 2022 book carried by the SHORT-side trigger mimics. **Operator
decision owed:** if the MGC-venue debit convention (rt_cost_bp=5) is accepted for
MGC-venue books — as it already is for the live XAU_4h_DonchN20 MGC book — MgcTF4h is a
WIRE (SHADOW) candidate at start-params arm0.25/cap12/pend6/be0.15/{T.08,W.20},
`bull_only=false`. At the spot-book 15bp convention it stays dead.

### 2. MgcSlowDonchian30m (priority 2)

**Load-bearing wire-truth discovery:** the LIVE parent is starved by its wired
peer-dedup (`peer_holds_pos` = skip entries while MgcFastDon 40/20 holds — and the fast
sibling holds most of a trending market). Live-faithful trigger cadence over the full
2yr certified file: **n=1 entry (REGIME=1) / n=3 (REGIME=0)** vs the naked cert cell's
n=158. A mimic wired on this parent would trigger ~1-2×/year — it can never accumulate
a certifiable forward sample, independent of edge. The mimic study below therefore ran
on the NAKED mechanism stream (n=158, the certified-cell parity stream) as the upper
bound; the naked stream is NOT the live trigger stream.

| book | params | net | PF | worst | nNeg | WF-H1/H2 | regime-B | 2x [net, PF] | verdict |
|---|---|---|---|---|---|---|---|---|---|
| SlowDon floored mimic @ 15/30bp (naked stream) | 0.25/1.5/12/6/0.15/{T.08,W.20} | −0.5 | 0.98 | −0.15 | 154/212 | −4.8/+4.3 | 2022 naked: −0.2 PF0.95 / @2× −4.7 PF0.25 (gated live stream: n=0, sits out) | −32.3, 0.31 | **NO-GO** |
| — full grid 128 configs @ 15/30 | arm×cap×pend×be×gb | — | — | — | — | — | — | **0/128 pass; ceiling +3.3@15 / −22.2@30** | grid ceiling ~flat |
| SENSITIVITY @ 5/10bp | same | +20.7 | 3.69 | −0.05 | 154/212 | +5.8/+14.9 | 2022: +2.8 but H2 −0.7, bear −0.2 FAIL / @2× +1.3, H2 −0.7 FAIL | +10.1, 1.66 (H1 +0.5 thin) | still FAIL |

**Verdict: NO-GO / KILL.** Fails the mandated gate outright (0/128, stress ceiling
negative) and still fails marginally at the venue-honest cost. Decisive independent
kill: the live trigger cadence (n≈1-3 per 2yr, dedup-starved) makes any mimic on this
parent structurally unwireable. Do not revisit without a new parent basis (e.g. the
dedup removed — a parent-config decision, not a mimic decision).

### 3. MgcTF2h — BULL-GATED variant only (priority 3) — parent stream n=590

Gate = the book-standard H1-SMA200 regime gate (`bull_only=true`, same mechanism as the
live XAU_4h_DonchN20 book), fed from the certified H1 files. Per the standing rule the
2h family was NOT re-tested ungated-spot (that book is retired); this is the gated MGC
venue basis.

| book | params | net | PF | worst | nNeg | WF-H1/H2 | regime-B | 2x [net, PF] | verdict |
|---|---|---|---|---|---|---|---|---|---|
| MgcTF2h BULL-GATED floored mimic @ 15/30bp | 0.25/1.5/12/6/0.15/{T.08,W.20} | +19.2 | 1.34 | −0.15 | 376/562 | **−9.3**/+28.5 | 2022: −10.3 PF0.49 / @2× −35.5 PF0.13 | −65.1, 0.44 | **NO-GO** |
| — full grid 256 rows @ 15/30, both axes | arm×cap×pend×be×gb | — | — | — | — | — | — | **0 pass anywhere; recent stress ceiling −65.1, 2022 ceiling −3.9@15/−21@30** | structurally negative at stress |
| SENSITIVITY @ 5/10bp | same | +75.4 | 5.01 | −0.05 | 376/562 | +18.8/+56.6 | 2022: +6.5 PF1.98 but bear-split −0.2 FAIL / @2× −1.9 PF0.86 FAIL | +47.3, 2.26 | recent PASS, 2022 FAIL |

**Verdict: NO-GO / KILL.** The bull gate does not rescue the 2h family on the MGC
venue: 0 passers in the full grid at the mandated basis (WF-H1 negative, stress
structurally negative — the same n≈560-clip cost-bleed class that killed the spot 2h
book), and even at the venue-honest cost the 2022 axis stays negative at stress. The
bull-gate-not-reject obligation is now PAID for this family on this venue — do not
re-open without a new basis.

## Verdict summary

| parent | mandated 15/30bp gate | venue-honest 5/10bp sensitivity | final |
|---|---|---|---|
| **MgcTF4h** | NO-GO (bear@2× kills 128/128) | **ALL-PASS, 128/128 plateau (ungated)** | **NO-WIRE at mandated basis; WIRE-candidate iff operator accepts the MGC rt5 debit convention (precedent: live XAU_4h_DonchN20 book)** |
| **MgcSlowDonchian30m** | NO-GO (0/128; ceiling −22@2×) | still FAIL (2022 halves/bear) | **KILL — plus live trigger cadence n≈1-3/2yr (dedup-starved) is unwireable regardless** |
| **MgcTF2h (bull-gated)** | NO-GO (0 passers, both axes) | 2022 FAIL | **KILL — bull-gate obligation paid; 2h family stays dead on MGC** |

## Traps / notes for future sessions

- The live MgcSlowDonchian30m trigger stream is ~1-3 opens per 2yr — any study of "the
  slowdon book" must use the DEDUPED stream for wire decisions; the certified n158 cell
  is the naked mechanism only.
- The 4h parent is BOTH-WAYS: a bull gate on its mimic vetoes the SHORT-side clips that
  carry the bear axis (gated 2022 FAILS where ungated PASSES at venue cost). Same
  conclusion as the S-14bc `bull_only=false` rule for both-ways parents.
- Cost basis is THE decision variable on this venue: 15bp-of-price ≈ 9-20× the real MGC
  futures RT. Every kill at 15/30 in this study except SlowDon/2h-2022 flips at 5/10.
- Floored books at be0.15/rt15 are 1×-cost confirmed only; at rt5 the same be is
  2×-compliant (foundation recipe). If the operator takes the TF4h wire at rt5, be0.15
  already satisfies confirm ≥ 2× RT.
- Harness: `backtest/clip_path_mgc_venue_mimic.cpp` (modes `entries` / `mimic`, env
  REGIME / REGIME_PRESEED / NODEDUP / REGIME_H1 / BULL_ONLY / RT / SWEEP). Sweep outputs
  + entry streams under `/tmp/mgcmimic/` (session-scratch).
