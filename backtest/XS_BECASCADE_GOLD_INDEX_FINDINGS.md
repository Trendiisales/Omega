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
