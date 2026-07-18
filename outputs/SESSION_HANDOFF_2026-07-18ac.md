# SESSION HANDOFF — 2026-07-18ac (all-3 BTC low-thr quads LIVE + crypto entry SOUND wired)

Caveman mode. Predecessor handoff: outputs/SESSION_HANDOFF_2026-07-18ab.md (copied into repo this session).
Copy THIS doc to `outputs/SESSION_HANDOFF_2026-07-18ac.md` on resume (Omega repo convention).

## STATE: ALL requested work DONE + DEPLOYED + VERIFIED both boxes. No in-flight edits.

## WHAT SHIPPED (this session)

1. **BTC UJ15 + UJ10 quads (operator: "all 3 of these") — LIVE-SHADOW on josgp1 build=01a30d2.**
   - Honest answer given first: 18ab shipped ONLY the 0.5% quad; 1.5%/1.0% were NOT added. Now they are.
   - +8 cells in `_bc_btc_lowthr_cells` (ChimeraCrypto src/main.cpp ~L5111):
     UJ15 det=1.5%: W1 g0.75 ret−1567, W2 g0.20 ret−1405, W4 g0.20 ret−965, W12 g0.75 ret−965.
     UJ10 det=1.0%: W1 g0.20 ret−1134, W2 g0.20 ret−967, W4 g0.20 ret−965, W12 g0.75 ret−1567.
     BTC 4→16 books (2.0% + 1.5% + 1.0% + 0.5% quads). Fleet 136→144 companions.
   - Figures = exact harness reruns (`UM_COIN=BTC UM_THR=0.015|0.010 UM_W=1,2,4,12 UM_G=0.2,0.5,0.75
     UM_LEGS=8 UM_CONFIRM=60 UM_ANCHOR=1 UM_RT=30 ./eth_ujmimic_15_becascade_bt` in /Users/jo/Crypto/backtest)
     — reproduce operator's transcript tables digit-for-digit. All PASS full gate base+2x, omit-2022.
   - Commits: ChimeraCrypto Mac `43816e0` → box deploy commit `e3ed03d` → **merge-reconcile `01a30d2`**
     (protected branch rejected force-push; merged origin into box lineage, trees identical, pushed,
     box rebuilt+restarted so stamp==HEAD==origin; Mac ff'd to same). **NO git reset touched josgp1**
     (feedback-josgp1-no-git-reset held); live_config.json uncommitted 41-sym LIVE pilot survived.
   - Boot verified: `[MIMIC-FLOOR-GATE] 144/144 0 VIOLATION`, `[PROFIT-LOCK-GATE] 146/2/0`, all 8
     `[CLIP-INIT]` lines, `RUNTIME MODE = LIVE` 41-sym $100/$500. Crypto repo `dd0adec` = findings addendum
     (backtest/BTC_LOWTHR_MIMIC_FINDINGS_2026-07-18.md now has both quad tables).

2. **Crypto entry/clip SOUND (operator: "confirm sound is on for trades entering?" — it was NOT) —
   omega-new build `882fb1dc` DEPLOYED-VERIFIED (running==HEAD==origin).**
   - Root cause: GUI `pollFires` `__compBooks` covered gold/idx/fx/xag/usoil/stk, NEVER `_cc`
     (josgp1 companion state) → crypto mimic leg opens (incl the $100 live-mirror BUY each triggers)
     + banked clips rang NOTHING. entryBell existed but crypto never called it.
   - Fix: crypto section in pollFires — subleg-key entryBell on new open, clips-count winBell on bank,
     same first-load/restart baselines. Bell latency ≤ ~2min (120s josgp1→desk relay).
   - Flow trap honored: edited `tools/gui/omega_desk.html` → `tools/gui/gen_index_html.py` regen →
     gui_drift gate GREEN (hand-editing OmegaIndexHtml.hpp FAILS canary). Omega commit `882fb1dc`.

3. **"Why does GUI show idle / BTC went over 1% and didn't fire" — ANSWERED, no bug.**
   - GUI +0.98% = vs 00:00 UTC daily open; leg happened overnight, topped ~64,05x, faded.
   - At check time BTC was FLAT on every det window (W1 −0.01% W2 −0.09% W4 +0.02% W12 −0.14%, px 63,963).
   - W12/W4 windows DID open on the overnight leg (DETSEED entries 64,049.86 / 64,259.73) but BE-ENTRY
     books nothing until px ≥ window entry +60bp (≈64,434 / 64,645) — never reached → flat, zero cost.
     That IS the floored-on-open foundation: declines stalls, buys extensions.
   - Header IDLE chip = real `live_trades` Binance positions ONLY (no-backtest-in-live-GUI rule);
     shadow legs never light it. Panel "idle · awaiting jump" = no OPEN leg; a det-window-open state
     currently renders same as idle (snapshot armed = post-open mfe>=arm only).
   - **OFFERED, operator hasn't answered: add "WINDOW · awaiting confirm" state chip** so an open det
     window awaiting confirm is visibly distinct from idle. Would need det_in surfaced into
     emit_companion_state (det_in already in UpJumpLadderCompanion state_json ~L310) + GUI ccState. Ask/do next.

4. **Vaults DONE**: Memory-Chimera BtcLowThrMimicQuad.md updated (16 books, 01a30d2, merge-reconcile) +
   index + log [18-07-2026 17.44]. Memory-Omega GuiAlarmLog.md + index + log (882fb1dc crypto sound).
   Handoff-ab copied to outputs/SESSION_HANDOFF_2026-07-18ab.md.

## KEY FACTS / TRAPS FOR NEXT SESSION

- **Operator FURIOUS about speed** ("we are missing this entire fucking move") — when BTC moves, check
  cell state FIRST (curl http://45.85.3.79:7779/api/crypto_companion, filter sym=BTC), answer from live
  state in seconds, do slow work after. Cells were live before the move leg; explain BE-ENTRY behavior
  (window + confirm60) rather than defending speed.
- Deploy flow that works (used twice now): Mac commit → push → tools/deploy_to_box.sh blocks (box-live
  vs Mac HEAD) → byte-verify box main.cpp == Mac PARENT commit sha → STALE_OK=1 rerun → box commits
  own deploy hash → origin push rejected (non-ff) → ssh box: git fetch + merge origin/main + push
  (protected branch = NO force-push) → box rebuild + restart (stamp==HEAD) → Mac git merge --ff-only.
  NEVER git reset on josgp1.
- Omega GUI edits: ALWAYS tools/gui/omega_desk.html → gen_index_html.py → drift gate; never .hpp direct.
- Omega deploy: `bash tools/omega_deploy.sh --no-push` (detached, polls running-binary hash; ~5-10min).
- git push/commit: NO `2>&1 | tail` suffix (PreToolUse hook blocks as stderr-suppress pattern).
- Chimera CI: `bash ci/run.sh` = 10 suites (~45s). Chimera build: `cmake --build build --target chimera`.
- `[STARTUP] ... shadow_mode=true` in the READY line = engine-graph shadow books wording; RUNTIME MODE
  line + PILOT-SCOPE line are the LIVE-mode truth. Don't panic on the former.
- josgp1 [MIMIC-LIVE] mirror ARMED: every grid leg open routes a REAL $100 BUY (pilot caps $500 gross).
  The 12 new BTC cells are grid cells → their confirms will fire real orders. Same architecture operator
  approved in 18ab/18s (mirror has own protection since b1e7c98).
- BTC now shares triggers across 4 thr families on big moves (a +2% jump fires all 16 books; multiple
  real $100 BUYs possible under gross cap) — quad sums not portfolio-independent; judged standalone per rule.

## OPEN / POSSIBLE NEXT

- **WINDOW-state chip on mimic panel** (offered, see §3) — small josgp1 emit + GUI change.
- **Forward-watch 16 BTC books**; first confirms will now RING (entryBell) ≤2min after.
- Memory-Omega was 4 commits BEHIND at session start (predecessor backfill) — now more after 882fb1dc;
  hook will report; auto-ingest per feedback-vault-backfill-auto-ingest.
- Operator may want lower-thr quads on other coins (fleet det=1.5%); same harness + gate, judge standalone.
- make_becascade_cell rt=28 per-coin measured cost note still owed (factory comment).
- Predecessor traps stand: fetch_coin_1h.py REWRITES csv from 2023 (never on full-history csv);
  josgp1 = `ssh chimera-direct`; omega = literal `ssh omega-new`; chimera log root-owned
  (`sudo` for grep/tail on ~/ChimeraCrypto/logs/chimera.log).

## SUGGESTED SKILLS NEXT SESSION
- None special for sweeps (compile+run C++). `/verify` if GUI/engine change ships. Vault discipline
  mandatory (CLAUDE.md). Second-brain pre-mine before any NEW strategy family.
