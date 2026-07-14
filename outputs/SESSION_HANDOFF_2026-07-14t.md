# SESSION HANDOFF 2026-07-14t (manual "handoff" hard-stop; context low)

Session S-2026-07-14 (t): resumed 14s handoff, closed its whole owed queue, then received a NEW
operator work order (see §NEXT — that is the next session's job; NOTHING of it started).

## COMPLETED THIS SESSION (all verified end-to-end)

1. **Sub-30m gold study (14s NEXT-1) COMPLETE + COMMITTED + PUSHED.**
   - Commits: `0905e4c2` (S-2026-07-14ba study) + `78160d3d` (S-2026-07-14be verification pass:
     splice+harness re-runs byte-identical, all 4 inputs re-gated CERTIFIED, two positive-cell
     counts corrected). Findings: `backtest/GOLD_SUB30M_2026H1_FINDINGS.md`. Vault:
     `GoldSub30mStudy2026H1.md` + index + log. Operator SAW the results tables.
   - Verdict: **1m/5m structurally dead** (1m gross-negative BEFORE costs — no signal, not
     friction); **10m** 16/18 positive @1× but PF-capped 1.22, zero gate passes; **15m slow-exit
     Donchian = real plateau** — fine sweep (Nin 45–70 × Nout 20–35) 25/30 GATE-PASS+LEGS, all
     fails in Nout=20 column. Peak **55/35: n=126 (4.8/wk), +$19,019 @1× / +$18,502 @2× per
     1 MGC, PF 1.83, maxDD $4,450, worst −$810, L +$5,513/S +$13,506, WF +15,350/+3,669,
     FULL 25mo +$20,330 PF 1.30**. Beats ax winners (KELT M30 PF1.33 / TF1H PF1.29) on
     net/PF/DD at half turnover.
   - Data: spot-XAUUSD 1m splice at MGC cost 0.41pt RT (NO sub-30m MGC bars exist — stated
     substitution). Splice `/Users/jo/Tick/xau_1m_spliced_2024_2026.csv` (837,302 bars,
     CERTIFIED; reproducible via `backtest/xau_1m_splice_2026h1.py`). Provenance finds:
     xau_h2026mar_jun_m1.csv clock = **Europe/London-local-as-epoch** (piecewise DST-converted,
     overlap RMSE 0.0000); tail = duka daily 1m-candle endpoint (`backtest/fetch_xau_candles_daily.py`,
     tick endpoint was server-degraded), tail csv committed, ends 2026-07-13 24:00 UTC.
   - Harness: `backtest/gold_subh30_tf_bt.cpp` (env `DON15_SWEEP=1` = fine sweep; `COST_RT`
     override; O(N) Donchian parity-verified).

2. **FEED-PATH selftest RED (14s NEXT-5) → GREEN, root-caused + hardened** (`39c3a6cc`
   S-2026-07-14bf, pushed).
   - Root cause: **zombie ibkr_dom_bridge** — a stale bridge process held the single-instance
     lock :19099 (=19000+client-id 99) with its TcpBroadcaster dead, so :9701 was unbound while
     the L1 csv stayed fresh; every 5-min OmegaIbkrBridge task refire exited via the lock guard.
     Omega consumer flapped "connected/disconnected 2s". Self-healed 07:07 UTC when the zombie
     died and a fresh fire bound 9701. Selftest verified GREEN end-to-end after.
   - Hardening shipped: `tools/feedpath_selftest.py` [CONSUMER-UP] fail now detects the zombie
     state (lock LISTENING + 9701 not) and prints the PID + `taskkill /PID <pid> /F` one-liner.
   - If it recurs: kill the zombie pythonw PID; the 5-min task refire respawns clean. Do NOT
     restart IBKR Gateway for this (memory feedback-ibkr-gateway-restart-caution).

3. **HEARTBEAT-MISS mystery solved — NOT a feed problem.** EurusdLondonOpen/GbpusdLondonOpen
   "NEVER pulsed" spam = the 5 FX session engines were REMOVED from the tick path 2026-06-29
   ("no FX" directive, see tick_fx.hpp header) but their heartbeat registrations at
   `include/engine_init.hpp:6454-6458` are still hardcoded `live_required=true` → false MISS
   every London/Asia session window since Jun-29. NO pulse call sites exist (verified: zero
   `pulse("EurusdLondonOpen")` etc. anywhere). FX ticks themselves flow fine.
   **Operator has now ORDERED the cull** (§NEXT-3).

4. **5 new gold engines (6d64d5de) watch:** boot `[SEED]`×5 hot, zero HEARTBEAT-MISS for all 5,
   no shadow-ledger closes yet (expected: DonH1 ~3/wk, KELT ~9/wk). `[STARTUP-FAIL]` for
   KeltM30/DonH1 at boot = noise (30m-bar engines can't pulse inside the 70s window;
   MgcSlowDonchian30m same). Keep watching `omega_trade_closes.csv` tags MgcTF1h_*/GoldKeltM30*/
   GoldTfBw1h_*/GoldDonH1* + `[GMIMIC]`.

5. **Vault current:** pin advanced ...→ `39c3a6cc` == origin/main (3 advances this session),
   log entries for each. Mac==origin==39c3a6cc; VPS tree 6d64d5de (behind by docs/backtest-only
   commits — binary unaffected; next deploy syncs).

## NEXT — OPERATOR ORDER (verbatim + interpretation; NOTHING started)

Operator: *"let's try on the 10 min find the sweet spot please and check all levers/settings and
then we run it, 15 min is a big go same instructions, remove the fx and cull with note please,
i am sure you have a note but ensure we always chek seeding, for the 10 min gold i accept the
lower pf"*

1. **10m gold sweet-spot sweep → WIRE.** Full lever sweep at 10m: DON Nin×Nout fine grid (mirror
   the 15m sweep; 10m coarse best was DON 40/20 +$9.6k PF1.21 / 55/27 PF1.22), stop-ATR mult
   {2.5,3,3.5}, slow-exit region emphasis (the 15m lesson: edge lives in Nout≥23 slow exits),
   optionally KELT/TF levers for completeness. **Operator ACCEPTS PF<1.3** for 10m — gate =
   net>0 @1× AND @2×, both WF halves +, both legs + (drop only the PF bar). Then wire the sweet
   spot as a live SHADOW engine (GoldBothWaysShortTfEngine pattern or new instance — see
   `backtest/ENGINE_BACKTEST_REGISTRY.md` §7b + `include/GoldBothWaysShortTfEngine.hpp`).
2. **15m DON = BIG GO, same instructions.** Extra lever pass around the certified plateau
   (Nin 45–70 × Nout 23–35 already GATE-PASS 25/30; add stop-mult {2.5,3,3.5} × maybe
   time-stop/trail variants), pick peak-region cell (55/35 or 60/35 — near-identical), WIRE it.
   Wire ONLY from the slow-exit plateau region (Nout≥23), never the 2:1-ratio family (20/10,
   40/20 — PF 1.21 different family).
3. **FX cull (ORDERED):** delete the 5 stale heartbeat registrations
   `engine_init.hpp:6454-6458` (Eurusd/Gbpusd/Usdjpy/Audusd/Nzdusd session engines) **with a
   tombstone note** in-code (pattern: the S11 P3b comment right below at :6461). This closes
   the false-MISS spam. Hands-off memory satisfied: operator explicitly ordered.
4. **Seeding — operator emphasised: ALWAYS CHECK.** Both new engines need: warm-seed CSV
   (data/mgc_15m_hist.csv + data/mgc_10m_hist.csv or resample from mgc_30m/1m path — check what
   `tools/seed_refresh.py` `_GOLD_TFS` can produce), nightly refresh recipe registered (or
   KNOWN_UNREFRESHED with owner), `seed_freshness_audit.py --registry-only` green,
   boot `[SEED]` line verified on deploy. Registry gate runs in mac canary — it will FAIL the
   commit if seeds unregistered (by design).
5. **Wiring checklist per engine** (copy 6d64d5de pattern): ExecutionCostGuard MGC row,
   ADVERSE-PROTECTION header verdict (3×ATR stop, NO loss-cut — registry §7 spot-LC trap),
   auto-retire −2× BT maxDD, heartbeat register (cadence ≥ bar span; expect boot STARTUP-FAIL
   noise), wire_cross persistence, ledger tags, row ts-dedup, boot-replay warmup guard, parity
   splice through the WIRED class (gold_bothways_engine_parity.cpp pattern) before deploy,
   mac canary GREEN, deploy omega-new, hash-verify, `[SEED]` lines, vault entity+index+log
   BEFORE declaring done.
6. **OPEN QUESTION for operator: BE-mimic books for the 2 new engines?** Not in the order this
   time. Last round each new engine got a be=0.10 mimic (validated per-parent via
   `backtest/gold_newengine_mimic_bt.cpp` + DUMP_ENTRIES on parent harness). If yes: re-run
   that validation on the NEW parents' entry streams first (never assume be transfers).
7. **Carried:** mimic M1/tick re-check before ANY live flip of the 5 existing new books;
   ConnorsRSI2 runtime cost-gate backfill before live; adverse-protection legacy backfill
   (3 headers); lot-size revisit (memory); tonight UTC 21:30/22:35/23:30 task watch (feeds
   banner covers next session start).

## TRAPS / NOTES
- 10m/15m harness levers: `gold_subh30_tf_bt.cpp` currently hardcodes DON15_SWEEP for 15m only —
  extend for 10m fine grid + stop-mult lever (env-gated, keep default output identical).
- Splice input is SPOT bars at MGC cost (substitution documented). A wired MGC engine trades the
  MGC 30m/1m FEED — for 10m/15m native bars the engine must aggregate from the live MGC feed
  (GoldBothWaysShortTfEngine aggregates 30m→1h already; 10m/15m need finer source — MGC feed is
  30m grain on VPS (mgc_live_bars.py 300s=5m bars → check actual grain!) — VERIFY feed grain
  supports 10m/15m bars before wiring; if only 30m exists, that's a blocker to surface).
- Never raise mimic be without re-running gold_newengine_mimic_bt.cpp (be=0.15 fragile).
- ssh form literally `ssh omega-new "..."`; never suppress git stderr; outputs/ gitignored →
  `git add -f` for this doc.
- clangd diagnostics on big headers = indexing artifacts; truth = cmake OmegaBacktest + canary.
- Zombie-bridge recurrence: selftest now prints the taskkill line — use it, don't restart Gateway.
