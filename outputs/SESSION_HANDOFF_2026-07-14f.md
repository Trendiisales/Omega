# HANDOFF 2026-07-14f — XAU DonchN20 LIVE wire deployed(?), gateway incident closed, USTEC disabled; DEPLOY badce504 IN FLIGHT (verify first!)

Session ~11:20–11:57 NZ Tue 14-07. Caveman active. Hard-stopped on /handoff at context warning.
Copy to `outputs/SESSION_HANDOFF_2026-07-14f.md` + commit when resumed (`git add -f`, outputs/ gitignored).

## FIRST ACTION ON RESUME (in-flight!)
**Deploy of `badce504` to omega-new was RUNNING at hard-stop** (background task `bpsf023y5`, output
`/private/tmp/claude-501/-Users-jo-Omega/c79d514d-3b06-431a-bd75-ec365b51b6a6/tasks/bpsf023y5.output`).
tools/omega_deploy.sh detached-deploy pattern, polls running binary up to ~14min. VERIFY:
1. Running binary hash == badce504 == origin/main (deploy-hygiene, CLAUDE.md).
2. Boot log MUST show (warm-seed mandate — absence = P1):
   - `[OMEGA-INIT][SEED] GoldTrendMimic XAU-H1 regime gate seeded: ~1100 bars, SMA200 WARM -> BULL|BEAR`
   - `[OMEGA-INIT][SEED] GoldTrendMimicLadder wired: 8 trigger books (... XAU_4h_DonchN20 1-leg LIVE resting-exec 1 MGC + H1-SMA200 bear-gate ...)`
   - `[IBKR-EXEC] qualified XAUUSD.M -> MGC <expiry>` (new spec qualifies at boot; if MISSING, orders will be BLOCKED with empty token — book still accounts, standard ladder semantic, but flag to operator)
3. THEN vault deploy mandate (owed on verify): Memory-Omega log.md deploy entry naming badce504;
   update `wiki/entities/SurvivorCellMimic.md` + `GoldTrendMimicLadder.md` (XAU book now LIVE
   resting-exec 1 MGC + gate; frontmatter commit badce504); index.md line; advance pin
   (`cd /Users/jo/Memory-Omega/raw/code && git fetch /Users/jo/Omega main && git checkout --detach FETCH_HEAD`).
4. Watch first forward lines: `[GMIMIC][XAU_4h_DonchN20] PENDING/BE-ENTER(tick)/CLIP` + first
   `[IBKR-EXEC] PAPER BUY/SELL XAUUSD.M qty=1` — confirm 1 contract, MGC, and clip booked at
   actual px not level.

## OPERATOR DECISIONS THIS SESSION (all 5 actioned)
1. USTEC_4h_ZMR mimic: DISABLED, deployed+verified `cfe10947`, vault filed. Bear-gate variant = separate study (not started).
2. XAU_4h_DonchN20: revalidated (AMBER standalone) then bear-gate study → **GREEN**; operator ordered "wire resting + size 1 MGC with bear gate proviso" → shipped in `badce504` (in flight above).
3. IBKR 5m pull: RAN, **killed the production IB Gateway** (see incident). 4/45 names salvaged. STOPPED permanently.
4. Lot sizing: proposal committed `19532b45` (backtest/LADDER_SIZING_PROPOSAL_2026-07-14.md). Only GO was XAU 1 MGC (now shipped). All other books keep $10k shadow.
5. Stock P&L −$8 floor: operator said OK. Closed.

## COMMITS THIS SESSION (do NOT redo)
| commit | what |
|---|---|
| `f891c277` | handoff doc 14e |
| `cfe10947` | USTEC_4h_ZMR disabled (intrabar FAIL tombstone), deployed+verified, vault filed+pin |
| `d99f5677` | XAU DonchN20 resting revalidation findings (AMBER: 2022 leg neg, 2x PF1.31 zero headroom; reproduces +11.3/PF1.58 exactly; slip-robust to 2bp) |
| `19532b45` | ladder sizing proposal (advise-only) |
| `badce504` | **XAU_4h_DonchN20 LIVE**: resting-exec tick path in GoldTrendMimicLadder (synthetic stops, actual-px booking, registry on_tick armed-gated, on_bar manage=armed_ fixes seed-replay leg-clipping risk), XAU-H1 SMA200 regime feed + warmup_XAUUSD_H1.csv seed, SurvivorPortfolio tick feed ABOVE zombie gate, IbkrExecutionEngine XAUUSD.M→MGC spec, engine_init config (lot=1.0 LOT-GATE-OK, notional 40k, live_book, cost gate remap XAUUSD.M→MGC row), tick_gold H1-close feed. Canary+backtest green. Commit msg has full detail. |

## KEY FIGURES (gated cell, size against THESE)
G-H1-SMA200 resting/M1: **+13.9%/leg PF2.79 DD−2.6% n67 (~15 legs/yr)**, WF +7.5/+6.4, cal-2022 +0.3,
2x-cost PF2.22, slip 3bp/fill still PF1.90. Honest slip-adjusted +12–13%/leg PF≈2.4–2.6. 1 MGC:
worst leg ≈−$820, worst DD ≈−$2,100. CAVEAT: 2022 gated n=6 — gate wins bear year by NOT trading;
bull-book with bear fuse. NEVER judge vs the (retired) level-fill shadow print. Findings:
backtest/XAU_DONCH20_RESTING_REVALIDATION_2026-07-14.md (incl BEAR-GATE VARIANT section).

## GATEWAY INCIDENT (closed — do not reopen unless RED again)
My 5m stock pull (agent, tunnel 127.0.0.1:4002→omega-new, clientId 77, paced 11s but 6-month
5m chunks) killed the production IB Gateway ~23:43-23:48 UTC → orders dead, operator alert RED.
IbkrGateway watchdog task (5-min, IBC autologin, log C:\Omega\bracket-bot\logs\gateway_watchdog.log)
revived it; exec reconnected (GC/NQ requalified). Rules saved: memory
`feedback-no-bulk-pulls-production-gateway` (+ MEMORY.md line). If gateway RED again + 4002 has
process but no listener = login dialog → operator RDP.

## 5m IGNITION STUDY (parked, awaiting data-source decision)
- Salvaged: backtest/data/stock5m/{NVDA,AMD,AVGO,MU}_5m.csv (2yr 5m RTH, CERTIFIED), _pull_state.json checkpoint, _pull.log.
- Harness READY: backtest/stock5m_ignition_mimic_bt.py (execute_basket cost model + 2x + house gate) + stock5m_integrity_batch.py. Puller backtest/ibkr_stock5m_pull.py — DO NOT run against 4002.
- Operator must choose data source: vendor files / data-only IBKR login / other. Then finish 41 names + study (never a subset — run all 45).

## MEMORIES SAVED THIS SESSION
feedback-parallel-agents-expedite (fan out background agents, operator wants speed);
feedback-no-bulk-pulls-production-gateway. Both indexed in MEMORY.md.

## CARRIED (unchanged)
M1 seed durable refresh (~early Aug); GoldCampaignD1Anch LONG watch; USTEC bear-gate salvage study
(new); crypto campaign v2 A-F backlog; stash@{0} cull-befloor-silver-lotfix (explicit ask only);
GBPUSD FX ladder maxDD extraction owed (sizing proposal HOLD item).

## TRAPS LEARNED (save re-derivation)
- GoldTrendMimicRegistry::on_bar used to hardcode bull=true → Config bull_only was a silent no-op; now real via ext_regime_/set_bull. Watch for other "documented but unfed" flags.
- IBKR exec place_order supports STP natively but send_live_order only sends MKT; resting = synthetic tick-stops (validated: penetration/slip stress PASS).
- XAUUSD → GC full-size (100oz) in IbkrExecutionEngine; micro = separate XAUUSD.M→MGC spec. Gold lot canary gate: exceptions need LOT-GATE-OK on the line.
- OmegaCostGuard has a dedicated MGC row ($10/pt, lot=contracts) — do not proxy micro gold through XAUUSD row.
- VPS powershell via ssh: pipes/braces mangle — write .ps1 to scratchpad, scp, run -File. Multi-pattern Select-String needs quoting care.
- Monitor tool (not sleep-loops) for waits; bg until-loops are BLOCKED by hook.

## SUGGESTED SKILLS NEXT SESSION
- `verify` — deploy verification + first live GMIMIC clip sanity.
- Agent fan-out — vault filing, USTEC bear-gate study, 5m study when data source decided (per feedback-parallel-agents-expedite).
- `superpowers:systematic-debugging` — if boot [SEED] lines missing or MGC fails to qualify.
