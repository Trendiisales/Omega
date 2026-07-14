# HANDOFF 2026-07-14j — upjump 2% spot parent NOT-VIABLE delivered; NEXT TASK = per-coin full lever sweep (percoin mode STAGED, not compiled/run)

Session ~13:24–13:4x NZ Tue 14-07. Caveman active. Resumed from 14i handoff.
Copy to `outputs/SESSION_HANDOFF_2026-07-14j.md` + commit when resumed (`git add -f`, outputs/ gitignored).

## THE PENDING TASK (operator's live ask — do this FIRST)
Operator: after the 2% verdict, **"revisit this then and pull every lever/setting and tell me
where it becomes viable for each coin please."** = full per-coin lever map of the immediate-entry
spot upjump structure (entry ON jump, BE-floor, bracket/trail/reversal exits).

State: a `percoin` mode was ADDED to `/Users/jo/Crypto/backtest/upjump2pct_be_bt.cpp` but
**NOT compiled, NOT run** (operator called handoff at context-low). Steps:
1. `cd /Users/jo/Crypto/backtest && g++ -O2 -std=c++17 upjump2pct_be_bt.cpp -o upjump2pct_be_bt`
   (pre-existing benign -Wformat warnings on the detail-mode header printf line — ignore or fix).
2. `./upjump2pct_be_bt percoin` — per coin (19), sweeps thr {2,2.5,3,3.5,4,4.5,5,5.5,6,7,8,10,12}%
   × W {1,2,3,4,6,8,12,24}h × pre-BE stop {0,1,2}% × exit g {0.3,0.5,1.0(=reversal-only)} = 936
   combos/coin. Gate = corrected long-only (net>0, PF≥1.3, WF-H1>0, WF-H2>0, 2×cost>0, n≥30,
   y2022 shown NOT gated) + **PLATEAU check** (≥75% of thr/W neighbors net-positive at 1× — kills
   isolated-ridge cells; ETH W1/2% must NOT come back as viable via this route).
   Prints best plateau-validated cell per coin or NO-CELL. Runtime maybe minutes (936×19×~3 sims,
   O(N) each over ≤48k bars); code re-runs run_full twice per cell (cache pass + gate pass) — fine.
3. Report per-coin table to operator. Cross-check against prior art: CryptoUpJumpBullCells vault
   page already certified NO-CELL (2023-26, mimic/confirm mechanism) for BTC/ETH/SOL/XRP/XLM/GRT/
   OP/BCH/AVAX/LTC and found 7 SWEET cells (TRX 8h/3.5 strongest) — DIFFERENT mechanism (this one
   = immediate entry + BE-floor), so results may differ; note overlaps/divergences in the report.
4. File verdict: update `Memory-Chimera/wiki/entities/UpJump2pctSpotParent.md` (+index one-liner
   amend, +log.md NZ-time entry), extend `Crypto/backtest/UPJUMP2PCT_SPOT_PARENT_FINDINGS.md`,
   commit Crypto repo. If any coin comes out VIABLE, loss-protection verdict is already inherent
   (floor + bracket swept) — state it per feedback-engine-loss-protection-provision.

IMPORTANT context for the sweep: operator's own standing ban "uniform +2% entry / never re-tier
per-coin" (feedback-test-operator-spec-before-verdict config bans) is EXPLICITLY overridden by
this ask — he wants the per-coin viability map. Confirmed-entry remains disproven (do NOT add a
confirm lever). Judge STANDALONE (never vs WIDE / never vs mimic). No 200DMA. C++ only for live.

## DONE THIS SESSION (do NOT redo)
1. **Handoff 14i committed** `a417ffc1` (outputs/SESSION_HANDOFF_2026-07-14i.md) + pushed.
2. **GUI screenshot check GREEN**: ALL-TIME +$56 = STOCK −$8 + GOLD +$64, COMP-BANK +$39, no
   "unclassified" → 69e13b5e classOf fix confirmed live in operator's tab.
3. **UpJump 2% spot parent BT — verdict NOT VIABLE, delivered to operator.** Full detail in:
   - `Crypto/backtest/upjump2pct_be_bt.cpp` (harness) + `UPJUMP2PCT_SPOT_PARENT_FINDINGS.md`
     (committed to Crypto repo, S-2026-07-14 commit).
   - Vault: `Memory-Chimera/wiki/entities/UpJump2pctSpotParent.md` + index + log (14-07 13.36).
   - Headline: thr=2% portfolio −219k…−711k bp across whole config space, 0/19 robust; pre-BE
     bracket stops strictly worse (50–68% churn); BE-floor can't reach the bleed (all damage
     pre-BE, worst singles −10…−32%); ETH W1/2% literal sole PASS = isolated ridge (W2/3/4 +
     thr1.5/2.5/3 all fail) → not built per BT-first gate (GoldCampaign precedent).
   - Data: 19 coins Binance 1h 2021→2026-07, integrity-gate 19/19 PASS
     (`Crypto/backtest/integrity_gate.py data/*_1h.csv`). GRX file = DAX mislabel (~24k px), excluded.
4. Faithfulness guards applied (memories): BE-floor IN harness (no floorless strawman —
   feedback-test-operator-spec-before-verdict), all 19 coins (never-half-the-symbols), y2022
   shown not gated (feedback-crypto-omit-2022-longonly), judged standalone (companion-independence).

## HARNESS MECHANICS (so next session doesn't re-derive)
`upjump2pct_be_bt.cpp` modes: `sweep` (portfolio ridge), `detail W thr s g` (per-coin table),
`percoin` (NEW, staged). Mechanism: detect close/close[i-W]−1 ≥ thr → buy next 1h open →
optional intrabar pre-BE stop s (gap-through at open) → BE-floor stop=entry×(1+RT) once close
covers cost → trail stop=max(floor, E×(1+mfe×(1−g))) updated per close (g=1.0 disables trail
= literal ride-to-reversal) → reversal exit j≤−thr next open → end flush. Re-entry requires j<thr
once. Costs 20bp RT, 2× = full 40bp re-sim. WF halves by entry bar index. Data `data/<COIN>USDT_1h.csv`.

## WATCH ITEMS (inherited 14i)
- 4 bigcap ladder windows PENDING: NOW/CRM/ADBE/BMY — first clips likely this week; verify booking
  (STOCK chip clipS path live-tested only for gold).
- First `[IBKR-EXEC] PAPER BUY/SELL XAUUSD.M qty=1` → confirm 1-contract MGC actual px.
- Pending operator decisions: SGE-premium salvage, 5m ignition study, USTEC bear-gate salvage,
  GBPUSD ladder maxDD extraction, M1 seed refresh (~Aug).

## TRAPS
- Live boxes: Omega = omega-new 45.85.3.79; crypto = josgp1/chimera-direct 143.198.89.54. Never
  omega-vps/chimera-vps (dead).
- If any coin shows VIABLE: do NOT wire anything without operator go; present table first.
  Immediate-entry class remains operator-forbidden-by-default (feedback-no-immediate-entry-upjump-
  mimic-only) — today's ask is a study, wiring needs explicit confirm.
- Vault log/index already carry the NOT-VIABLE verdict — percoin results AMEND (supersede-in-part),
  don't duplicate a second entity page.
- zsh: avoid bare `===`/`==` echo tokens (globbing error); quote them.

## SUGGESTED SKILLS NEXT SESSION
- None special for the sweep (compile + run + report). `verify`-style discipline: sanity-check one
  VIABLE cell's trade list before presenting (n, dates, worst trade) — don't ship a table off a
  fresh untested mode without eyeballing one coin's clips.
- If operator then wants live wiring: BACKTEST_TRUTH_CRYPTO.md + deploy_to_box.sh + check_box_sync.sh.
