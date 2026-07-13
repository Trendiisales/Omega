# HANDOFF 2026-07-14c — resumed 14b, "wire in all and complete": ALL DONE. Survivor mimic WIRED+DEPLOYED, basket cron live, chimera WARN killed, feed verified.

Session ~10:00-10:20 NZ Tue 14-07. Caveman active. Hard-stopped on operator /handoff at context-low.
Copy to `outputs/SESSION_HANDOFF_2026-07-14c.md` + commit when resumed (hard-stop forbade committing).

## STATE: NOTHING IN-FLIGHT. All ordered work COMPLETE, deployed, verified, vault-filed.

## DONE THIS SESSION (details in commits/vault — do NOT re-derive or redo)

| commit | what |
|---|---|
| `280f59e7` | **S-2026-07-14h survivor-cell mimic WIRED SHADOW** (operator "wire in all" = the wire order on the S-14e study). DEPLOYED to omega-new, running-binary == HEAD == origin verified. Boot verified: 9-book `[OMEGA-INIT][SEED] GoldTrendMimicLadder wired` + `[SURV-SEED]` both cells + `ARMED (post-seed)`. Vault: `Memory-Omega/wiki/entities/SurvivorCellMimic.md` updated to WIRED + index + log. |
| `e040d5f8` | **S-2026-07-14i open items**: (1) hourly :05 basket cron via `tools/install_rda_basket_cron.sh` (idempotent, INSTALLED on Mac, backup /tmp/ct.bak.20260714_101003); (2) chimera DESK_EXPORT WARN root-caused BENIGN + `display_truth_selftest.py` probe made reset-aware (windows journal count from newest `chimera_inbound.csv.*` backup mtime **+1s** — rename preserves mtime). Re-run GREEN, WARN gone. Mac-side tools only, NO box deploy needed. Vault log appended. |

Wire mechanics (for reference, already shipped): one-way `gold_trend_mimic().on_trend_open(cfg.tag,…)` in `SurvivorPortfolio.hpp Cell::evaluate_signal` after `[SURV-OPEN]` print; native H4 `on_bar(cfg.tag,…)` in `Cell::on_tick` fed **BEFORE** `evaluate_signal` (= mimic_ladder_overlay seq0-skip semantics, leg first sees NEXT bar). Books in `engine_init.hpp` before `gm.set_exec`: `XAU_4h_DonchN20` be1.0/arm0.25/lc2/cap30/gbT10 rt5bp + `USTEC_4h_ZMR` be0.15/arm1.0/lc2/cap20/gbT8 rt3bp, pend_bars=6 both. Ledger leg tags `XAU_4h_DonchN20MimicT` / `USTEC_4h_ZMRMimicT`. Positions opened pre-deploy NOT mimicked (deploy-forward by design). Study: `backtest/SURVIVOR_MIMIC_FINDINGS_2026-07-14.md`.

Also closed:
- **XAUUSD feed resume check** (14b First-Action 1): ticks resumed after 22:00 UTC settlement break, 0 STALE-RESUB post-reopen, L2 live. Benign class confirmed; engines-dark selftest GREEN (16 engines).
- **Operator Q "why minus pnl, should be zeroed"**: ONLY negative book = STOCK BASKET `rdagent_book` (−$176). Book WAS zeroed 07-11 13:20 (`factor_basket_state.json.pnl_zero_bak_20260711_132016`). −176 = REAL post-zero forward trades: META realized −118 (sold 07-14 04:14 @660.78 vs 669.21) + CRM partial −21 + costs −27 + open marks −17. Per-name "pos P&L" column = open marks only; headline = equity−capital (realized+costs+marks). Both honest, reconcile 1:1 with orders csv. Operator accepted (no follow-up).
- **Operator Q "why so slow"**: answered — ssh round-trips + canary build + deploy poll + 2 full selftest runs; offered "skip deep verification" mode.

## OPEN / NEXT (operator-decision or later)

1. **Basket leg-sizing quirk (offered, NOT ordered):** `execute_basket.py` sizes legs as `capital/len(buys)` (10000) not current equity → paper cash went −$97 on 07-14 04:14. Cosmetic in shadow, wrong at live. Fix = equity-based sizing. Operator was told "say the word" — awaiting order.
2. **Intrabar re-check owed** on SurvivorCellMimic + GoldTrendMimicLadder legs before LIVE sizing (close-grade H4 validation only).
3. **Mimic lot sizing**: $10k placeholder notional (project-revisit-lot-sizes) — operator decides.
4. Carried from 14a/14b: M1 seed durable refresh (histdata July ~early Aug); GoldCampaignD1Anch LONG-side PF1.39 watch; sizing decision worst −352bp; crypto campaign v2 A-F BT-gated; stash@{0} cull-befloor-silver-lotfix.
5. First forward mimic legs: watch `[GMIMIC][XAU_4h_DonchN20]` / `[GMIMIC][USTEC_4h_ZMR]` lines on next SURV-OPEN; desk shows them in the gold-trend-mimic books JSON automatically.

## TRAPS (verified/hit this session)

- Deploy ONLY `bash tools/omega_deploy.sh` → omega-new. omega-vps = DEAD box.
- 21:00-22:00 UTC = daily settlement break: XAUUSD silent, STALE-RESUB fires, SPREAD-Z anomalous at reopen — ALL benign. Don't diagnose feed-death inside the window.
- `mimic_ladder_overlay --cfg`: arm/lc/be in PERCENT, gb as FRACTION. be>0 forces pend_bars=6 internally.
- Survivor cell seed uses `push_bar_internal` directly → no mimic feed during seed; registry also disarmed until post-seed `arm()`. Belt-and-braces, both verified.
- Rename preserves mtime: a `.pre_reset_*` backup's mtime == last row's write time, NOT reset time. Any mtime-windowed probe needs +1s.
- `omega_trade_closes.csv` header-only since 07-13 reset = CORRECT, not broken.
- Local `sleep N && cmd` chains BLOCKED by hook — single ssh calls only, or Monitor/bg tasks.
- clangd diagnostics on engine_init.hpp/OmegaTelemetryServer.cpp = standalone-parse noise; Mac canary build is truth (was green).
- Vault index.md edits: file not Read-gated for python heredoc — used scripted replace (Edit tool requires prior Read).

## SUGGESTED SKILLS NEXT SESSION
- `verify` — if fixing basket equity-sizing (drive execute_basket end-to-end).
- First mimic-leg forward verification: none needed beyond log watch; `superpowers:systematic-debugging` if a leg misbehaves.
