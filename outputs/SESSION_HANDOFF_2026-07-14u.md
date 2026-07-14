# SESSION HANDOFF 2026-07-14u (manual "handoff" hard-stop; context low)

Session S-2026-07-14 (u): resumed 14t handoff, executed the ENTIRE operator order queue
(10m sweep+wire, 15m BIG GO sweep+wire, FX cull, seeding verified), answered the mid-session
mimic question with a backtested NO, caught+fixed a latent seed-task P1, deployed + verified.
**Nothing owed from the 14t order. All commits pushed, deploy verified.**

## STATE (verified end-to-end)
- Mac == origin/main == VPS tree = `009b20b6`. Running binary = `3d33ebf4` hash-verified
  (bk `009b20b6` is data-only seed snapshots — binary unaffected, correct).
- Commits this session: `0eeb3d4a` bh (FX heartbeat cull) · `a042d453` bi (sub-30m wire) ·
  `3d33ebf4` bj (OmegaSeedRefresh task fix) · `009b20b6` bk (seed snapshots, CERTIFIED).
- Vault current: entity GoldBothWaysShortTfEngine + GoldSub30mStudy2026H1 updated, index.md,
  log.md, pin advanced 39c3a6cc → 009b20b6.

## COMPLETED THIS SESSION

1. **10m sweet-spot sweep → WIRED** (`DON10_SWEEP=1` in `backtest/gold_subh30_tf_bt.cpp`,
   grid Nin 20–80 × Nout 15–45 × stop 2.5/3.0/3.5 + KELT ext). Slow-exit plateau (Nout≥31)
   the coarse study missed: **DON 30/35 stop3.0 = +$21,281 @1× / +$20,207 @2×, PF 1.52,
   WF +10,359/+10,922 (most balanced cell in grid), maxDD $4,891, FULL 25mo PF 1.29** — full
   GATE-PASS, operator's PF<1.3 waiver not needed. Plateau robust: Nin 20–50 × all stops pass;
   KELT extension all dead at 10m. Wired `GoldDon10m_30_35_stop3ATR` SHADOW 1 MGC,
   retire −980pt.
2. **15m BIG GO sweep → WIRED** (`DON15_STOP=1`, plateau Nin 45–70 × Nout 23–45 × stop
   2.5–4.0). **144/144 GATE-PASS.** Peak **DON 60/35 stop3.5 = +$20,131/+$19,656 PF 1.93,
   WF +15,412/+4,719, maxDD $3,794, FULL PF 1.37**; stop3.5 interior peak (4.0 declines);
   Nout45 (PF 2.04) REJECTED — WF2 thins to +$2,509. Wired `GoldDon15m_60_35_stop3.5ATR`
   SHADOW 1 MGC, retire −760pt.
3. **Mimic question (operator, mid-session) → NO, both engines, backtested.** Real entry
   streams via DUMP_ENTRIES (added to sub-30m harness), `gold_newengine_mimic_bt.cpp`,
   be=0.10, 216 cells each: 10m 0/216 pass even close-grade; 15m 12/216 close-grade pass but
   the 1m intrabar-truth check collapses the best cell +$14,161/+$7,155 → +$2,475/**−$4,799**
   WF1-neg (USTEC_4h_ZMR coarse-bar artifact). `mimic_tag=""` on both; tombstoned in vault —
   re-open only with NEW mechanism basis.
4. **FX heartbeat cull (ordered)** `0eeb3d4a`: 5 stale registrations deleted
   (engine_init.hpp, tombstone note in place). False HEARTBEAT-MISS spam closed.
5. **Latent P1 found+fixed** `3d33ebf4`: OmegaSeedRefresh task pointed at DELETED
   `refresh_warmup_seeds.py` zombie (removed S-14an) with BLANK WorkingDirectory — tonight's
   23:30 run would have failed silently. Durable fix `tools/register_omega_seed_refresh.ps1`
   (re-registered on VPS, cwd C:\Omega load-bearing, audit rc = task result). First manual run
   wrote ALL 5 mgc hist seeds incl. the 2 new.
6. **Wiring mechanics (bi):** engine class gained `row_secs` (pass-through = per-row
   level-stop == harness intrabar EXACTLY); `poll_mgc_fine_feed` (MgcFastDonchianFeed.hpp)
   with boot-replay entry-block; wire_cross persistence ×2; heartbeats GoldDon15m/GoldDon10m
   (1800s, 0–24); ADVERSE-PROTECTION verdicts in header; feed `tools/mgc_live_bars.py` now
   3-grain (30m/15m/10m, one-shot "2 W" backfill on empty fine CSV); seeds
   `data/mgc_15m_hist.csv`+`mgc_10m_hist.csv` nightly via seed_refresh recipes (M10 pull does
   NOT recreate warmup orphan); seed_freshness_audit registered (live CSVs KNOWN_UNREFRESHED,
   hist in extras set); registry audit + mac canary GREEN.
7. **Parity** `backtest/gold_subh30_engine_parity.cpp`: certified splice through WIRED class —
   LONG legs match sweep TO THE DOLLAR (15m +$4,658.4 vs +$4,658; 10m +$7,238.8 vs +$7,239);
   n−1 + S-leg delta = final still-open trade only. PASS.
8. **Deploy + boot verify:** `[SEED] GoldDon15m 1940 x15m -- hot`, `[SEED] GoldDon10m 2105
   x10m -- hot`, HEARTBEAT-INIT ×2, `[MGC-FINE-FEED]` boot replay complete + polls live,
   entries armed. VPS seed CSVs byte-identical to committed snapshots (fc verified).

## CURRENT GOLD ENGINE BOOK (the "new engines" + mimics table, given to operator)

| Engine | Mech/TF | 6mo @1×/@2× | PF | Mimic |
|---|---|---|---|---|
| MgcTf1h (port) | TF 1h LC=0 | registry §7b | — | BE-mimic be0.10 arm0.50/lc2.0/cap12 SHADOW |
| GoldKeltM30_k1.25_trail2.5 | Kelt M30 | +$12,225/+$11,241 | 1.33 | BE-mimic be0.10 arm0.25/lc2.0/cap96 SHADOW |
| GoldTfBw1h_ema10_40_t2.0 | H1 EMA TF | +$11,464/+$10,623 | 1.29 | BE-mimic be0.10 arm0.15/lc1.0/cap48 SHADOW |
| GoldTfBw1h_ema20_100_t2.0 | H1 EMA TF | +$11,367/+$10,646 | 1.33 | BE-mimic be0.10 arm0.15/lc2.0/cap48 SHADOW |
| GoldDonH1_20_10_stop3ATR | H1 DON 20/10 | +$7,403/+$7,088 | 1.33 | BE-mimic be0.10 arm0.50/lc2.0/cap12 SHADOW |
| **GoldDon15m_60_35_stop3.5ATR** (NEW) | 15m DON 60/35 | +$20,131/+$19,656 | 1.93 | **NONE — backtested not viable** |
| **GoldDon10m_30_35_stop3ATR** (NEW) | 10m DON 30/35 | +$21,281/+$20,207 | 1.52 | **NONE — backtested not viable** |

All SHADOW, 1 MGC, no loss-cut (registry §7), auto-retire −2× BT maxDD, judged via shadow ledger.

## NEXT / CARRIED (nothing new ordered)
1. **Watch shadow ledger** for `GoldDon15m_*` / `GoldDon10m_*` closes (~4.5 + ~10/wk expected)
   alongside the 6d64d5de five (DonH1 ~3/wk, KELT ~9/wk still zero closes as of 14t).
2. **Tonight 23:30 OmegaSeedRefresh**: first scheduled run of the re-registered task — verify
   Last Result 0 and mgc_*_hist.csv timestamps advance (feeds banner next session).
3. Carried from 14t: mimic M1/tick re-check before ANY live flip of the 5 existing BE-mimic
   books; ConnorsRSI2 runtime cost-gate backfill before live; adverse-protection legacy
   backfill (now 8 headers listed by canary); lot-size revisit (memory).
4. Optional: collate the warmup_* seed snapshots the manual seed_refresh run touched on VPS
   (nightly job owns freshness; [snapshot-lag] warnings only).

## TRAPS / NOTES
- Fine feeds: engine `row_secs` MUST equal file grain; `hist_` deque cap 160 bounds don_in.
- Never wire 15m/10m cells outside the slow-exit plateau (Nout≥31); never the 2:1 family.
- Mimic tombstone: do NOT re-run be-mimic on these parents without a new mechanism basis —
  close-grade figures are known-fake at sub-H1 grain (1m truth check mandatory).
- mgc_live_bars.py backfill: an EMPTY fine csv triggers a "2 W" pull once — deleting a fine
  csv on VPS is self-healing but costs one bigger request; don't delete casually.
- OmegaMgcLiveBars task was stop/started this session (new 3-grain producer live). If fine
  CSVs go stale: check that task first, then the zombie-bridge lock pattern (14t note).
- ssh form literally `ssh omega-new "..."`; VPS git pull needed untracked-file move-aside for
  the seed snapshots once (done); outputs/ gitignored → `git add -f` for this doc.
- Deploy hash discipline: binary 3d33ebf4 vs tree 009b20b6 is CORRECT (data-only delta).
