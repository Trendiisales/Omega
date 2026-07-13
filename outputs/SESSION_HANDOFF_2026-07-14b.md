# HANDOFF 2026-07-14b — resumed 14a: 4 deploys verified, survivor mimic TESTED (awaiting wire order), bigcap 2%-only WIRED, desk P&L WIRED, display-truth guard fixed

Session start ~09:09 NZ Tue 14-07, hard-stopped on operator `/handoff` ~10:00 NZ.
Caveman active. Copy this doc to `outputs/SESSION_HANDOFF_2026-07-14b.md` + commit when resumed
(hard-stop forbade writing it there directly).

## ⚡ FIRST ACTIONS NEXT SESSION

1. **XAUUSD feed resume check (only unfinished item).** A ScheduleWakeup was pending at
   hard-stop: confirm on omega-new that `[TICK] XAUUSD` lines are fresh and `[STALE-RESUB]`
   stopped after 22:00 UTC (10:00 NZ). All silence 21:00-22:00 UTC was the daily settlement
   break — benign, diagnosed twice this session. If ticks did NOT resume by ~22:05 UTC,
   THEN it's real (IBKR bridge / BlackBull sub) — feeds_selftest.py first.
2. **Operator decision owed: wire the survivor mimic?** Test PASSED both live cells (see
   §survivor below). If ordered: wiring plan is written in
   `backtest/SURVIVOR_MIMIC_FINDINGS_2026-07-14.md` (one-way hook in SurvivorPortfolio::Cell
   open path, 2 GoldTrendMimicLadder-pattern books, shadow first, native-H4 feed).
3. Vault pin currently `b956baff` == origin/main at hard-stop. 5 commits this session all
   pushed + vault-logged; nothing unfiled.

## DEPLOYS THIS SESSION (all verified running-binary == HEAD == origin, box = omega-new 45.85.3.79)

| commit | what |
|---|---|
| `8d933cfc` | (prev session's fix) verified live + `[SEED] GoldCampaignD1Anch 32877 M1 bars days_seen=28 slots_warm=46 -- hot`. GoldCampaignD1Anch deploy saga CLOSED. |
| `7877c459` | S-2026-07-14d bigcap 2%-only + S-14e survivor mimic research files |
| `50291324` | S-2026-07-14f desk STOCK BASKET P&L column |
| `b956baff` | S-2026-07-14g display_truth_selftest false-RED fixes (Mac tool, no binary change but deployed anyway harmless) — NOTE: b956baff was pushed; whether a 4th box deploy ran: NO — tool is Mac-side cron, no deploy needed. Box binary = 50291324. |

## DONE — details live in commits/vault, don't re-derive

- **S-2026-07-14d (`7877c459`)**: BigCap2pctImpulseCompanion 20d-high DROPPED → 2%-only
  trigger (operator order). `hi_window=1` degenerates the high check. BT
  `backtest/bigcap_2pct_only_bt.py`: parent +4885%/PF1.91, wired mimic cell +6761%/PF1.69
  worst −28.5%, all-6 + 2× + ex-best(MU) PASS. Vault entity updated.
- **S-2026-07-14e (`7877c459`)**: SURVIVOR MIMIC STUDY (operator: "mimic as soon as trade
  profitable, rides till reversal, independent — test"). Engine in the companion-clips
  panel = SurvivorPortfolio cell XAU_4h_DonchN20 (NOT covered by GoldTrendMimicLadder).
  Harness `backtest/clip_path_survivor.cpp` drives REAL Portfolio live-faithful (parity
  n=445 == audited survivor_gated_bt). Overlay = shipped BE-ENTRY mechanism.
  **XAU_4h_DonchN20: default be0.10 FAILS 540/540; PROFIT-CONFIRMED be1.0/arm0.25/lc2/
  cap30/gb10 PASSES all-6** (+14.6%/leg PF2.05 DD−3.2% n=60, 2× PF1.81, plateau 21/27,
  lc near-inert=free backstop). **USTEC_4h_ZMR: be0.15/arm1.0/lc2/cap20/gb8 +27.2%
  PF3.61 n=51 PASS** (189/540 plateau). NOT WIRED (test-only ask). Vault
  `SurvivorCellMimic.md`. Caveat: close-grade H4 — intrabar re-check owed before LIVE sizing.
- **S-2026-07-14f (`50291324`)**: STOCK BASKET desk panel per-name "pos P&L" column.
  execute_basket.py result gains positions[] (avg cost replayed from
  factor_basket_orders.csv). Paper book VERIFIED FIRING at 3% gate: CRM 19sh/NOW 29sh/
  ADBE 14sh, equity $9,824; box copies verified (positions + 3 BUY rows). GUI regen +
  gui-drift OK. **Root-cause note: /tmp/rda_basket.json refreshes ONLY via qlib_refresh 4h
  retrain path (no dedicated cron) — was stale "no BUY names" after the 08:55 3% re-export;
  ran manually this session.** Consider a :05 hourly cron for run_basket_daily.sh if
  operator wants faster book turnover (crontab-edit-via-script rule).
- **S-2026-07-14g (`b956baff`)**: operator "DESK SHOWS WRONG DATA" RED = 2 guard false
  positives, both fixed, re-run GREEN: (1) campaign books log `[CAMP-INIT]` not
  `[CLIP-INIT]` → 4 LIVE CAMP cells (UNI-CAMP-W1/W2, TRX-CAMP-W8, LDO-CAMP-W8; CAMPAIGN-MGR
  wired=4 on josgp1) were flagged desk ghosts; probe parses both now. (2) engines-dark
  check had no 21-22 UTC settlement-break awareness (us_index_market_open hr==21 → False;
  ger30 window 07→21).
- **Omega warnings diagnosed (twice)**: STALE-RESUB XAUUSD / BRACKET_STALLED /
  41-silent-engines STARTUP-SELFTEST at boot = deploys landing inside the 21-22 UTC break.
  Benign class; the selftest fix (14g) stops the guard-RED recurrence.

## OPEN / CARRIED

1. **Survivor mimic wire decision** (operator) — see First Actions 2.
2. **Chimera DESK_EXPORT divergence (real, pre-existing WARN):** journal
   `[DESK_EXPORT]=12` vs inbound csv rows=0 in 24h — export hook writes journal but csv
   empty. Investigate on josgp1 (chimera-direct 143.198.89.54).
3. **rda_basket 4h-only refresh** (see 14f note) — cron decision for operator.
4. Carried from 14a: M1 seed durable refresh (histdata July ~early Aug); GoldCampaignD1Anch
   LONG-side PF1.39 watch; mimic lot $10k placeholder (project-revisit-lot-sizes); sizing
   decision worst −352bp; crypto campaign v2 A-F BT-gated; stash@{0} cull-befloor-silver-lotfix.
5. Intrabar re-check owed on SurvivorCellMimic + GoldTrendMimicLadder legs before LIVE sizing.

## TRAPS (verified again this session)

- Deploy ONLY `bash tools/omega_deploy.sh` → omega-new. omega-vps = DEAD box.
- `[SEED]`/boot lines are in **omega_service_stdout.log** (stderr has only Git-hash line) —
  greps on stderr for SEED return nothing, not a P1.
- zsh: `echo ===X===` breaks (`=X` expansion) — quote it.
- mimic_ladder_overlay `--cfg` units: arm/lc/be in PERCENT (0.25 → 0.25%), gb as FRACTION
  (0.10 = 10%). Passing gb as 10 = 1000%, silently kills everything.
- Local Bash background wait-loops BLOCKED by hook — use bg task notifications; box-side
  deploy is detached, judge by BOX state.
- run_basket_daily.sh takes ~60-90s (yfinance refresh) — /tmp/rda_basket.json reads mid-run
  show the PREVIOUS content.
- display_truth probe roster: any NEW chimera book class with its own init tag must be added
  to the `[(?:CLIP|CAMP)-INIT]` regex or it becomes a false ghost.

## SUGGESTED SKILLS NEXT SESSION
- `verify` — if wiring the survivor mimic (drive the hook + ledger tags end-to-end).
- `superpowers:systematic-debugging` — for the chimera DESK_EXPORT csv divergence.
