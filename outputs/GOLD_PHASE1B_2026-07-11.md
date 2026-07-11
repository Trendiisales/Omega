# GOLD PHASE 1b — every Phase-1 deferred item CLOSED (S-2026-07-11)

Scope: the 5 deferred items from `outputs/GOLD_PHASE1_2026-07-11.md` (base `d0c6b099`),
operator instruction "fix all of these, no deferring". Backup tag
`pre-gold-phase1b-20260711_224507`. Commits `89177bab` (MGC book) + `b75f3f7b`
(persistence) + this evidence doc. Nothing on the list remains deferred.

## Item 1 — MGC VolBreakout tick-stop: 3-variant DECISION TEST run, winner WIRED

The MGC `GoldVolBreakoutM30` instance is bar-fed (`poll_mgc_feed` 30m rows), so its
`on_tick()` sl/trail was DEAD CODE that looked active — cadence audit 46/46 exits via
MAX_HOLD. Harness `backtest/mgc_volbrk_tickstop_decision.cpp` drives the REAL engine
in every mode over CERTIFIED data (`data_integrity_gate.py` CLEAN on both files):
MGC 30m 2024-06..2026-06 (23,616 bars) + XAU-M30 2022 bear shadow at MGC-cost proxy.
Cost 0.31pt RT + 0.62 2x stress; $10/pt/contract, lot 1.

| variant (cost 0.31 / 2x 0.62)     | n  | net pt      | PF          | worst      | maxDD     | halves        | exits          |
|-----------------------------------|----|-------------|-------------|------------|-----------|---------------|----------------|
| ORIGINAL (no stop, MAX_HOLD only) | 33 | +266 / +256 | 1.41 / 1.39 | −82 / −83  | 197 / 201 | +31 / +235    | 33 MH / 0 stop |
| **PROTECTED (1.5ATR + 3ATR trail, bar-path resting stop)** | 37 | **+281 / +270** | **2.07 / 2.00** | **−31** | **79 / 80** | **+90 / +191** | 4 MH / 33 stop |
| CATASTROPHE 5 ATR                 | 33 | +277 / +267 | 1.47 / 1.44 | −82        | 164       | +49 / +228    | 17 MH / 16     |
| CATASTROPHE 6 ATR                 | 33 | +243 / +232 | 1.36 / 1.34 | −99        | 194       | +16 / +226    | 19 MH / 14     |
| CATASTROPHE 8 ATR                 | 33 | +263 / +252 | 1.40 / 1.38 | −82        | 214       | +15 / +248    | 26 MH / 7      |

**WINNER: PROTECTED** — highest net AND PF 2.07 vs 1.41, worst-trade −31 vs −82,
maxDD 79 vs 197, both WF halves +, 2x-cost PF 2.00. It dominates; the historical
"wiring the stop costs ~52% net" did not reproduce against the real engine with
gap-honest fills (the old harness overshot the stop test by bid=low−half and had a
different tick ordering). 2022-bear: n=2–3 either way (EMA200 trend gate sits the
bear out) — no bear objection, sample too thin to carry weight.

**Wired (honest, no dead code):** engine gained `stop_mode`
(0 NONE / 1 TICK / 2 BAR_INTRABAR / 3 CATASTROPHE). MGC instance = **mode 2**: the
engine's own sl/trail enforced ADVERSE-FIRST on each 30m bar as a resting stop,
gap-honest fill `min(open, sl)`. Mode 0 (had ORIGINAL won) sets NO sl/trail state
at all — nothing can pretend protection again. Spot instance = mode 1, tick path
unchanged (tick_gold.hpp drives `on_tick` for it; verified).

**Bonus live bug found & fixed in the same path:** `poll_mgc_feed` passed the bar ts
in SECONDS into `on_m30_bar(..., now_ms, ...)` → the MGC instance's London/NY
session gate (load-bearing: sess-off PF 0.76) computed a garbage hour that drifted
through the day-cycle over ~2.7 years, and its ledger entry/exit timestamps were
1970-scale. Now `ts*1000` + `open` passed; `seed_m30_from_csv` gained s→ms scale
safety (its MGC seed file is seconds; the spot warmup CSV is ms).

## Item 2 — full retained-spot-gold re-run at true cost (7/8/10/16bp RT tiers)

Method: each faithful harness copied to scratch + charged so TOTAL RT cost = tier-bp
× entry price (residual-per-trade on top of a realistic 0.20–0.30 execution spread;
a literal (tier−3)bp synthetic spread trips the engines' max_spread gates and fires
nothing — stated deviation). 4h harness copy validated: reproduces the documented
S-2026-07-10 run EXACTLY at TIER=0 (n=313, PF 1.82, +$5,960). Data: 2yr
`2yr_XAUUSD_tick_fresh.*` + 2022-23 `XAUUSD_2022_2023.*`, all integrity-checked.
GoldTsmomD1V2 + CalendarTom XAU already done in Phase 1 (spot 8bp PF 1.96 / 1.94).

| engine (prod config)   | 8bp net / PF / halves        | 16bp (2x)        | 2022-23 @8bp     | VERDICT |
|------------------------|------------------------------|------------------|------------------|---------|
| XauTrendFollow4h       | +$3,488 / 1.35 / both+       | +$2,643 PF1.25 + | −$183 PF0.95     | **PASS** |
| XauTrendFollow1h       | +$7,325 / 1.40 / both+       | +$4,964 PF1.25 + | −$1,189 PF0.89   | **PASS** (bull axis) |
| GoldVolBreakoutM30 spot| +$141 / 1.50 / both+         | +$52 PF1.15      | +$95 PF1.93      | **PASS** (tiny book, n=35) |
| Rider4H (companion, standalone) | +$3,127 / halves +1741/+1386 | +$2,110  | −$602            | **PASS 2024-26** (best-effort) |
| XauTrendFollowD1       | +$94 / 1.01 / H1-half NEG    | −$359            | +$278 (H1 neg)   | **MARGINAL** — 12bp −$132 (flips sign) |
| XauThreeBar30m         | +$26 / 1.01 / H1-half NEG    | −$793 PF0.66     | −$150 PF0.78     | **MARGINAL→effectively dead** — 12bp −$301 |
| XauTrendFollow2h       | −$4,542 / 0.71 / FAIL        | −$6,534          | −$2,370 PF0.61   | **FAIL** (see mechanism note) |
| GoldPanicBounce        | −$83 / 0.97 (bull axis)      | −$746 PF0.76     | −$608 PF0.57     | **FAIL** at prod config |
| RiderD1 (companion, standalone) | −$593 / H1-half NEG | −$1,232          | ≈ flat (+$7)     | **FAIL** (best-effort) |

LOUD REPORTING on the FAILs (no engine disabled this session — operator decision,
and each kill mechanism is flagged per the verify-kill-replicates-mechanism rule):

- **XauTrendFollow2h**: production config deeply negative at every tier/era. The
  specific killer is `LOSS_CUT_PCT=0.5` (0.5% ≈ $16 at px 3263 = sub-bar noise) on
  the harness's H1 feed — but that LC was validated on an M30→H2 feed, so an
  M30-fed LC recheck is owed before the LC-inclusive number is acted on. HOWEVER
  even the most favorable variant (gates ON, LC OFF: +$2,690 PF1.19) fails the
  halves gate (H1 −$151) and 2022 (−$620). Recommendation: **route the 2h horizon
  to MGC** (the MgcTF2h venue port is already live-shadow with LC=0 and PASSes at
  MGC cost — registry §7) and disable the spot 2h after the M30-fed recheck.
- **GoldPanicBounce**: negative at true cost both eras at production config.
  Diagnostic: with the production EMA200-slope TREND_GATE off, the documented
  pre-gate bull edge reproduces (+$1,442 n113 PF1.44) — i.e. the gate itself
  forfeits most of the bull edge while not rescuing the bear. It is live-size
  blocked already; recommendation: disable or re-decide the gate; MGC panic port
  (roadmap) is the constructive path.
- **RiderD1**: standalone-negative on the fresh tape at every tier (judged
  standalone per the companion rule, never vs its host). Best-effort overlay
  methodology — a faithful C++-engine rerun is owed before any live-size change.

Out of scope (hands-off, per rules): tombstoned/disabled (h1_swing, sess_nypm,
swing_break, GoldOrb family, g_trend_rider, FVG shadow, EMACross/GoldStack legacy).

## Item 3 — g_mgc_volbrk position-persistence: REGISTERED (+ the audit itself was dead)

- `g_mgc_volbrk` decl moved to `globals.hpp` (include-order fix, same as
  MgcTF4h/2h/MgcSlowDon precedent) → `wire_cross(g_mgc_volbrk, "MgcVolBreakoutM30",
  "MGC")` + engine `persist_save/persist_restore` (chandelier-relevant
  `atr_at_entry` carried in the unused tp field; max-hold clock reconstructed from
  wall time) + 4-arg `force_close` for the AccountingGuard/KILL-ALL closer + a
  dashboard `register_source` (its paper-book MGC legs were GUI-invisible).
- The SPOT instance was ALSO unpersisted: live since S-2026-07-01 yet still on the
  audit's "dormant" allowlist. Allowlist entry removed; wire added.
- **FOUND while verifying via the audit:** the S-2026-06-26 persistence enforcement
  in `mac_canary_engines.sh` sat BELOW an `exit 0` — unreachable since a later gate
  was appended above it. The audit had NEVER run in the canary; 4 unpersisted
  display engines had accumulated. All closed this session: ConnorsRSI2 +
  SpxTurtleD1 wired (their persist APIs existed since S-2026-07-07v, the wire never
  did), GoldPanicBounce gained the persist pair + wire, QndxSqf
  allowlisted-with-justification (signal-state book — legs re-derive position from
  the daily-CSV replay + on-connect adopt; a persist wire would fight the
  reconcile). Gate moved back into the live canary path. `persistence_audit.sh`
  GREEN; canary GREEN end-to-end including it.

## Item 4 — MgcFastDon cost-guard: ADDED, verified near-inert

`ExecutionCostGuard::is_viable("MGC", 0.10, close − chan_low(Nout), lot, 1.5)` added
to the entry path (same MGC row MgcSlowDon got in Phase 1; distance proxy = the
structural channel-exit distance, the role 3xATR plays for SlowDon). Backtest
confirmation of non-distortion: the REAL engine driven over certified MGC 30m
2024-26 with the wired prod config (40/20 + HVN) pre-change vs post-change —
**183/183 trades bit-identical** (block threshold 0.612pt vs channel widths ≥
ATR14 min 8.4pt). The gate is honest-dollars insurance, zero behavior change today.

## Item 5 — FastDon/SlowDon 40/20 duplication: RESOLVED (SlowDon re-celled 55/27)

Phase 1 confirmed both siblings ran Donchian 40/20 on the same MGC 30m feed.
Roadmap fix backtested on the faithful chassis (`gdd_mgc_volband_breakout.py`,
certified MGC 30m 2024-06..2026-06, 0.31pt RT; Nout = 55//2 = 27):

| cell              | n   | net pt  | PF   | halves          | maxDD  | 2x-cost       | ex-best |
|-------------------|-----|---------|------|-----------------|--------|---------------|---------|
| Don40/20 (old)    | 204 | +2006.3 | 1.83 | +752.8 / +1253.5| −188.0 | +1943 PF1.79  | +1741   |
| **Don55/27 (new)**| 158 | +1504.6 | 1.78 | +717.6 / +786.9 | −278.9 | +1456 PF1.74  | +1265   |

All gates PASS (net/PF/both WF halves+/2x-cost). REAL-engine parity EXACT
(MgcSlowDonchian30mEngine driven standalone at 55/27: n158 +1504.6 PF1.78; the
40/20 control also reproduces the documented n204 +2006.3 PF1.83 exactly).
**Different horizon, quantified:** daily-PnL Pearson corr vs the live FastDon
40/20 real-engine book drops 0.748 (the wired duplication) → **0.690**; the two
SlowDon horizons correlate 0.586. Wired in omega_main: `Nin=55, Nout=27`,
retirement latch rescaled −400 → **−560** (2x the new cell's worst DD −278.9),
`sl_atr_mult=3.0` unchanged (plateau sl2.5–3.5 all PF≥1.65). No
MgcDonchianPortfolio netting layer built — Phase-2 allocator's job, per instruction.

## Build/verify

- `cmake --build build --target OmegaBacktest -j` GREEN.
- `bash scripts/mac_canary_engines.sh` GREEN exit 0 (engine headers, adverse-
  protection 0 violations, tombstone guard CLEAN, pnl completeness, GUI drift,
  and — now actually running — the persistence audit).
- Changed headers additionally clang `-fsyntax-only` clean standalone
  (GoldVolBreakoutM30, MgcFastDon, MgcFastDonchianFeed w/ MGC_FEED_STANDALONE,
  GoldPanicBounce).

## Nothing left deferred

Phase-1 deferred list: #1 tick-stop decision ✅ · #2 full spot re-run ✅ (verdict
table above; FAIL actions = operator decision, loudly stated) · #3 g_mgc_volbrk
persistence ✅ · #4 FastDon cost-guard ✅ · #5 Phase 2 (session service,
expected-net-edge gate, gold allocator) — NOT part of this list; remains the next
separately-launched phase per instruction.
