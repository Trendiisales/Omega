# SESSION HANDOFF 2026-07-15c (manual "handoff" hard-stop, context low)

Session resumed from 15b. Two fixes shipped+deployed+vaulted; crypto question from the
operator arrived at context floor — INVESTIGATION NOT STARTED, answer it first thing next
session (sketch + leads below).

## COMPLETED THIS SESSION (shipped + verified)

1. **S-2026-07-15d `a4152570` DEPLOYED** — GoldTrendMimic mfe/mae ledger telemetry fix
   (15b audit cosmetic gap: mimic clips logged mfe/mae=0). `GoldTrendMimicLadder.hpp`
   LedgerFn gains trailing `mfe_pct/mae_pct` (leg peak + NEW trough tracking, signed % of
   entry; trough NOT persisted → restart resets MAE, accepted); `engine_init.hpp` lambda
   converts to ledger convention (positive magnitude, price-units × size). Telemetry-only.
   Canary GREEN, running binary verified, boot 15 trigger books + ARMED confirmed.
2. **S-2026-07-15e `6efcd07d` DEPLOYED** — STOCK BASKET panel misread guard (operator:
   "trading ALL stocks incl negative ones?"). **Verified FALSE**: `execute_basket.py:153`
   trades only `action=="BUY"`; positions file = `{"DELL": 11}` alone. The 23-row panel =
   full-universe model ranking CONTEXT. The 07-13 CRM/NOW/ADBE buys were VALID at the
   operator's own 3% gate (S-2026-07-14 change from 5%): +4.84/+3.30/+3.12% day moves, all
   new 20d highs (verified in sp500_long_close.csv), rebalanced out next close; P&L −$19 ≈
   costs. Display fix: held/BUY rows partitioned on top, explicit "▼ ranking context only —
   NOT traded" divider, context rows dimmed 0.45, header relabelled. **TRAP: edited
   `tools/gui/omega_desk.html` then regen `tools/gui/gen_index_html.py` — OmegaIndexHtml.hpp
   is GENERATED; hand-edit trips gui_drift_gate in canary.**
3. **Session-start FEEDPATH RED resolved — deploy-restart race** (same class as 15b
   staleness RED): b420ca15 restart 07:14 NZ, binary needed ~10 min to establish IBKR 9701
   (07:25), hook scanned inside window. Rerun GREEN all 5. RULE: any staleness/feedpath RED
   within ~10 min of a deploy = rerun before alarming.
4. **Vault current**: GoldTrendMimicLadder.md + DayMoverBasket.md entities, index ×2,
   log ×2, raw/code pin advanced b420ca15 → 6efcd07d. (This handoff commit will put the pin
   1 behind again — pin-lag only, auto-advance per feedback-vault-backfill-auto-ingest.)

## OPEN — OPERATOR QUESTION UNANSWERED (do FIRST next session)

**"Why are ETH and BTC not trading? How can we be negative in INJ when it is trading up?
MOST coins are up and we are not trading them — why?"**

Investigation state: barely started. `ssh chimera-direct` home = `ChimeraCrypto/` (repo),
`stall_accountant.py`, backups. companion_state.json NOT located on josgp1 yet — the desk
reads `/api/crypto_companion` on omega-new (josgp1 `emit_companion_state` → launchd relay,
see omega_desk.html crypto panel comment); Omega VPS copy at `C:\Omega\companion_state.json`
(protection selftest reads it, age was 0.7min = relay alive).

Answer sketch (VERIFY against live state before presenting):
- **INJ negative while price up**: already explained 15a/15b (synopsis in
  SESSION_HANDOFF_2026-07-15b.md §INJ). T1 −116.8bp + T2 −125.3bp = designed REVERSAL
  clips: mimic entered at BE after +5.5%/24h jump confirm, price reversed within 2-3 bars,
  reversal exit fired (reversal exit IS the stop for this engine). Price later continuing
  up WITHOUT a position = book stays negative while chart is green. In-envelope (worst BT
  clip ≈ −770bp, self-retires cum −2300bp), shadow $0 real, PF1.50 runner-paid book whose
  losing mode is exactly many small reversal cuts. Judge STANDALONE (companion rule).
  1 leg was open (T1 armed) yesterday — check if it ran.
- **ETH/BTC/most-coins not trading**: mimic books are TRIGGER-fired, not drift-fired —
  *-REGIME-BEMIMIC mimics the 24h/+8% regime-switch parent; *-SWEET cells = validated
  sweet-spot self-detect (INJ = 24h/+5.5%). A coin being "up" 2-4% on the day fires
  NOTHING. Books book $0 until a jump trigger + BE covered (operator's own no-immediate-
  entry mandate, feedback-no-immediate-entry-upjump-mimic-only). VERIFY: which coins have
  books at all (roster from /api/crypto_companion or josgp1 emit), whether ETH/BTC have
  *-SWEET/*-BEMIMIC cells wired, and whether the 2026-07-13 kill (KILL_UPJUMP_CLIPS/
  PARENTS culled 68 immediate-entry cells) is what the operator is missing — most of the
  old always-in grid was deliberately killed 07-13, only companion-at-BE mimic class
  survived. Also memory feedback-crypto-omit-2022-longonly: 7 robust coins + ETH + 2 were
  gate-unblocked — check whether wiring for those ever landed or is still owed.
- Panel truth check while there: crypto panel rows come straight from /api/crypto_companion
  (dynamic, no baked roster) — trust it over memory.

## CARRIED (unchanged from 15b)

1. **P1 operator: rotate BlackBull FIX password** → then creds-strip commit
   (omega_types.hpp:15-20, env/ini-load, LIVE-refuse-on-missing).
2. INJ forward watch — no action unless cum → −2300bp (self-retires).
3. Mimic M1 parity re-check before ANY live flip (7 gold books); ConnorsRSI2 runtime
   cost-gate backfill; adverse-protection legacy headers (8 owed); lot-size revisit;
   GoldDon mimic cadence watch.
4. mimic mfe/mae telemetry: CLOSED this session (15d).

## TRAPS / NOTES

- `OmegaIndexHtml.hpp` = generated from `tools/gui/omega_desk.html` via
  `tools/gui/gen_index_html.py`. Edit html → regen → commit BOTH. Drift gate in canary.
- Deploy-restart race: staleness/feedpath RED inside ~10 min of a deploy = rerun scan.
- ssh: `ssh omega-new` / `ssh chimera-direct` only. Minimize ad-hoc VPS ssh (RAM reaper).
- rdagent basket gate = 3% (deliberate, S-2026-07-14). Panel ranking rows ≠ trades.
- Caveman mode active (full).
