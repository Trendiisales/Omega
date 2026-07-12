# BE-Cascade Mimic Port — GOLD + STOCK INDICES (S-2026-07-12b)

Operator task (SESSION_HANDOFF_2026-07-12a): port the crypto up-jump **BE-cascade mimic**
(enter on up-jump → arm companion → each leg reaching BE stacks the next arm → exit ALL on
reversal giveback) to gold + indices and backtest faithfully.

## Fidelity

- Driven through the **REAL live header** `ChimeraCrypto/include/core/UpJumpLadderCompanion.hpp`
  via `engine_book_stagger` (the byte-exact-validated path) — not a re-implementation.
- Harness: `Crypto/backtest/upjump_earlyarm_bt.cpp` modes `xsgrid` + `xsrandom` (commit `4d743e0`).
  Run: `cd /Users/jo/Crypto/backtest && UJW_TF=1d ./upjump_earlyarm_bt xsgrid`
- Arms BE-N6 `{0.2,2,3,4,6,8}%`, giveback g50 rev-only, reclip OFF, cap=#tiers, long-only,
  **no DMA gates**.
- Data (all `data_integrity_gate.py` certified): XAU daily aggregated from
  `XAUUSD_2022_2026.h1.csv` (2022–2026); SPX/DJ30/NDX daily 2016–2026 from `/Users/jo/Tick`.
  NDX gate "REJECTED" was a false positive — flagged rows are genuine Feb-2016 lows (~3888)
  vs a decade-growth median; continuity verified, overridden with rationale.
- **Real costs per symbol**: XAU 5bp RT (IBKR 2×0.015%+spread), indices 3bp RT.
  Crypto's 20bp would have strawman-killed the port. 2×-cost column = full re-sim at double.
- Gate = OOS episodes entering ≥2023 (fair long-only window): net+ ∧ PF≥1.3 ∧ both WF halves+
  ∧ 2×cost+. Full 2016-26 period + y2022 bleed printed alongside (bleed shown, not hidden).

## Result grid (candidate cells; full 48-cell table in bt output)

| sym | W | thr | parent OOS net% | mimic OOS net% | mimic PF | 2×cost | y2022 bleed | gate |
|-----|---|-----|-----------------|----------------|----------|--------|-------------|------|
| XAU  | 10 | 2% | +49.1 | **+397.4** | 8.0  | +391.5 | +28.1 | PASS |
| SPX  | 10 | 3% | +49.7 | **+329.3** | 16.5 | +327.9 | +19.8 | PASS |
| DJ30 | 10 | 4% | +30.8 | **+220.1** | 5.5  | +218.8 | −1.9  | PASS |
| NDX  | 10 | 2% | +91.0 | **+553.1** | 7.7  | +550.1 | +5.2  | PASS |

net% = Σ per-leg net returns (one contract per leg, same convention as the crypto book).
**All 48 evaluable (sym,W,thr) cells PASS** — parameter-insensitive plateau, not a tuned cell.
Parent and mimic are ADDITIVE independent books (CompanionDominanceError rule) — the parent
column is context, never a dominance test.

## Beta control (xsrandom — the honest caveat)

Random-entry control: same episode count+durations, random placement in the SAME OOS window,
20 seeds, same book:

| sym | actual% | random μ% | σ | z |
|-----|---------|-----------|-----|-----|
| XAU  | +397 | +308 | 82 | **1.1** |
| SPX  | +329 | +164 | 60 | **2.8** |
| DJ30 | +220 | +97  | 53 | **2.3** |
| NDX  | +553 | +314 | 75 | **3.2** |

- **Indices: real timing edge** — up-jump entry beats random placement by 2.3–3.2σ.
- **Gold: mostly beta** — z=1.1; in the 2023-26 gold mega-bull the BE-cascade book prints
  whenever it's long; the up-jump trigger adds +89% over random mean but within noise.
  Standalone the gold book is still strongly net-positive/robust — but the *entry signal*
  is not proven on gold, the *mechanism* is.

## Verdict

- **NDX, SPX, DJ30: VIABLE** — gate PASS across the whole parameter plateau, 2×cost robust,
  both WF halves positive, y2022 bleed small vs net, timing edge real vs random.
- **XAU: VIABLE-AS-BOOK / UNPROVEN-AS-SIGNAL** — passes every gate but the beta control says
  the up-jump timing itself adds little on gold in this window. Deploy only with the position
  that it's a bull-regime harvest book, or wait for the index engines' forward record first.

## Known caveats

1. **Daily-close fills** — giveback/BE checks evaluate on bar close (same fidelity the crypto
   daily book accepted). Intrabar the clip would execute earlier/worse. Shadow-forward record
   is the arbiter before any live sizing (standing rule: shadow ledger first).
2. Sum-of-legs return convention — sizing/notional decision is the operator's
   (project-revisit-lot-sizes).
3. XAU history only 2022+ (h1-derived); indices 2016+.

## UPDATE S-2026-07-12c — gold TWO-SIDED BRACKET variant (operator spec) + WIRED

Operator answer to the gold beta caveat: **OCO bracket entry** — movement trigger
(|10d move| ≥ 2%) places buy-stop +0.3% / sell-stop −0.3%; first fill wins, other side
cancelled; filled side = parent (rides to reversal); same BE-cascade mimic stack on the
activated direction (shorts too); exit-all on opposite reversal.
BT `backtest/xau_bracket_becascade_bt.cpp` (H1, W=240≈10d, cost 5bp RT/leg):

| dataset | bracket mimic net% | PF | 2×cost | long-only control |
|---------|-----------------|------|--------|-------------------|
| 2022-26 (bull) | **+140.3** | 1.53 | +127.9 | +194.4 (PF 2.21) |
| 2013 bear | **+39.5** | 1.59 | +36.1 | **−18.5** |
| 2015 bear | **+28.7** | 1.53 | +25.7 | **−18.7** |
| 2022 chop | −1.5 (b=0.5%: +6.5) | 0.98 | −4.6 | −4.7 |

- Bears: short side turns long-only bleed into profit — the protection the operator asked for.
- Cost of the insurance: bull-year shorts bleed (−56%), bull net +140 vs +194 long-only.
- 2022-style chop ≈ breakeven (protected, not profitable). Ambiguous both-side bars: 2/112 (skipped).
- W-sensitivity: W=240 anchor; W=120 collapses (chop whipsaw), W=360 degrades. thr=2% robust, 3% fragile.
- dirstop variant (movement direction picks side) tested — NOT better; OCO kept.
- SimBook replica (live long-only crypto header can't drive shorts); parity treatment owed if live-sized.

**WIRED (this session): all four SHADOW** — `include/BeCascadeEngines.hpp`:
`XsBeCascade_USTEC.F/US500.F/DJ30.F` (D1 long, thr 2/3/4%) + `XauBracketCascade` (H1 OCO
bracket 2%/0.3%). Warm-seeded (index D1 fresh; gold H1 from MGC to 2026-06-03, REBASE-on-first-tick
kills the venue offset, honest after 240 live H1 bars). Persistence wired (multicell + whole-book
closer). Cost-gated. Judged on the shadow ledger forward record.

## Proposed engine spec (if operator wants to wire — SHADOW first)

- New Omega engine `XsUpJumpBeCascadeEngine` (or per-symbol instances): parent detect
  W=10d, thr XAU/NDX 2%, SPX 3%, DJ30 4%; BE-N6 arms {0.2,2,3,4,6,8}%, g50 rev-only,
  reclip OFF, cap 6; exit-all on parent reversal (−thr over W); long-only; no DMA gate.
- **ADVERSE-PROTECTION verdict (mandate)**: per-leg 50%-of-peak giveback + whole-book
  reversal exit is the backtested protection; a cold-cut was NOT added (crypto precedent:
  tiers arm from +0.2%, worst path is window-end flush; 2×cost + maxDD columns above are
  the backtested envelope). maxDD 1.8–4.5k bp on the candidate cells.
- Warm-seed mandate applies (D1 buffers, W=10 → tiny seed CSV need).
- Shadow ledger first; judge on `omega_trade_closes.csv` forward record.

## UPDATE S-2026-07-12d — INTRADAY XauBracket variants (M5/M10/M15) — TESTED, NOT VIABLE

Operator asked for 5/10/15-min bracket-cascade engines. Data: M1 2024-26 (bull) + M1 2022
bear leg, integrity-gated, aggregated. Same harness (`xau_bracket_becascade_bt` argv/env form),
5bp RT/leg, 2x-cost re-sim, sweep W(4h/12h/24h-equivalent) × thr{0.5,1,2%} × b{0.1,0.3%}.

Best cells (all at ~12h window, thr 0.5%, b 0.1%):
| TF | bull 24-26 net% | PF | 2x | 2022 bear net% |
|----|------|------|------|------|
| M5  (W=144) | +221.5 | 1.46 | +152 | **−15.3** |
| M10 (W=72)  | +222.1 | 1.50 | +159 | **−12.0** |
| M15 (W=48)  | +229.8 | 1.58 | +172 | **−14.8** |

- Bull plateau real (all 3 TFs, thr 0.5-1%, both halves +, 2x-robust).
- **2022 bear: NEGATIVE at every tradeable cell** (PF 0.5-0.8; the few positive bear cells are
  n≤9 noise). Intraday whipsaw defeats the bracket's short-side insurance — unlike the H1
  W=240 (10-day) engine, which was the point of the bracket.
- Long-only control beats the bracket in bull at every TF (shorts pure drag intraday).
- Ambiguous both-touch bars grow at b=0.1% (up to 99/237 windows skipped conservatively).

VERDICT: intraday bracket-cascade = bull-harvest only, fails the operator's bear-protection
requirement → NOT wired. The deployed H1 W=240 XauBracketCascade remains the protected form.
Re-open only with a regime gate (changes the thesis — operator decision).

## UPDATE S-2026-07-12e — intraday M5/M10/M15 WIRED with gold_regime bull gate (M30 dropped)

Operator: "wire this in for a bull move — when gold goes long we MUST trade" + drop M30
(PF 1.24 / 2x +31 too thin). Gate = `gold_regime().long_blocked()` on bracket PLACE and FILL,
replicated exactly in BT (H1 EMA200/50, persist-100 falling check; macro layer omitted = live
strictly more protective). Gated results (thr 0.5%, b 0.1%, 12h window):

| engine | W | bull 24-26 | PF | 2x | 2022 bear gated (ungated) |
|---|---|---|---|---|---|
| XauBracketCascade_M5  | 144 | +203.3 | 1.56 | +146.5 | −1.7 (−15.3) |
| XauBracketCascade_M10 | 72  | +173.3 | 1.50 | +121.4 | −2.5 (−12.0) |
| XauBracketCascade_M15 | 48  | +163.6 | 1.50 | +115.1 | −1.2 (−14.8) |

Cascade = tested BE-N6 (6 tiers max, spawn-on-BE — averages ~1.6 legs/window intraday).
Engine gains `tf_secs` + `entry_blocked` hook; H1 flagship stays ungated (its 10-day window
IS the protection). Seeds: auto-refreshed warmup_XAUUSD_M5/M10/M15 (M10 added to
seed_refresh._GOLD_TFS). Persistence: multicell + closers (M-instances BEFORE the bare
XauBracketCascade prefix in the closer table). All shadow.
