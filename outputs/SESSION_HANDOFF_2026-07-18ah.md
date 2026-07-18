# SESSION HANDOFF — 2026-07-18ah (asks A+B from ag SHIPPED: watchdog loop detector + trigger-proximity tiles)

Caveman mode. Predecessor: outputs/SESSION_HANDOFF_2026-07-18ag.md.
Operator ended with "handoff" (hard stop). Both operator asks from ag BUILT + DEPLOYED + VERIFIED.

## SHIPPED THIS SESSION (all deployed + verified)

**1. Ask A — watchdog RELAUNCH-LOOP detector. Omega `09ae4c9d`, pushed.**
- feeds_selftest.py new `[vps-wdog]` check: `WATCHDOG_LOOP_CHECKS` manifest (first entry
  task=IbkrGateway, log=C:\Omega\bracket-bot\logs\gateway_watchdog.log) →
  `vps_watchdog_loop_health()`. ONE ssh (`-EncodedCommand` — raw pipes break ssh→cmd.exe;
  trailing `exit 0` so suppressed cmdlet errors can't mask parsed verdicts) reads VPS UtcNow
  + task LastTaskResult + log tail-40.
- RED on: task MISSING · last tick rc nonzero (loop ticks exit 1) · LOOP ACTIVE = ≥3
  WARN:/ERROR: lines in 30min AND newest ≤12min old. **Density+recency BOTH required** —
  healthy ticks log nothing so a dead loop's burst sits at the tail forever; recency arm
  keeps the fixed 06:1x-06:3xZ spam PASS w/ history note.
- Verified: PASS on real log, RED on simulated-active + missing-task. Runs weekends too
  (incident was Saturday). Extend manifest for future *_watchdog logs (ISO-UTC stamps).
- Vault: Memory-Omega `WatchdogLoopDetector.md` + index + log.

**2. Ask B — trigger-PROXIMITY metric ("see when a symbol is moving towards a trigger,
not a 24h % that means nothing" + "something on the crypto symbol boxes").**
- **Engine (ChimeraCrypto `f159b94`, LIVE on josgp1):** `UpJumpLadderCompanion::
  det_proximity()` — j_pct = running close (det_close_, per-tick fresh) / ring front −1
  vs det_thr; win_open; confirm_dist_bp (bp to lowest per-tier confirm while window open
  + book flat). `emit_companion_state` adds per-leg `prox_valid/win_open/win_move_pct/
  confirm_dist_bp` on existing 5s throttle → 120s relay → :7779 (whole-file relay, zero
  relay/server change). det_w=0 parent-driven cells emit prox_valid=false (UNIFORM 4h
  parent family — never conflate). Verified: build=f159b94==HEAD, LIVE, MIMIC-FLOOR-GATE
  155/155 0 VIOLATION, PROFIT-LOCK 157/2/0, 10/10 Mac suites; 151/159 legs prox_valid,
  35 coins, win_open cells emitting.
- **Desk GUI (Omega `2c91f540`, deployed omega-new --clean, running binary VERIFIED
  2c91f540==HEAD==origin):** crypto tiles sub-line `→0.32/0.50% 64%` + bar fill = j/thr
  ratio (grey→amber 60%→red 85%); window OPEN = green `WIN +j%` (+ `conf −Xbp` while flat
  book awaits confirm). 24h% + day-range bar remain ONLY for coins w/ no self-detect cell
  (`window._proxHas` guard — WS ticker never fights drawProx, 1s interval). Mimic panel
  ccState: flat-book open window = amber `WINDOW OPEN · +Xbp to entry` (folds carried
  ask #1 window-chip); idle books show live j/thr proximity. Post-deploy verified: served
  page has drawProx, /api/crypto_companion serving prox fields.
- Vault: Memory-Chimera `TriggerProximityEmit.md` + index + log; Memory-Omega log note.

**3. Vault pin advanced** — Memory-Omega raw/code → 2c91f540 (was 2 behind at session start).

## CARRIED OPEN (from ag, minus what shipped)

1. ~~WINDOW-state chip~~ DONE (folded into ask B mimic-panel ccState).
2. Protection selftest [1] weekend-skips companion freshness — wrong for 24/7 crypto; unfixed.
3. Omega-side build-truth chip (TRADING beacon precedent) — suggested, not demanded.
4. Forward-watch 16 BTC + 11 ETH low-thr books; first confirms ring entryBell.
5. make_becascade_cell rt=28 per-coin measured-cost note; lower-thr quads for OTHER coins
   (SOL/BNB etc. still 1.5%; judge standalone per coin); UJ naming rename (not approved).
6. Orphan mgc_15m_hist.csv nightly refresh no reader — consider dropping recipe.
7. NEW: generalize WATCHDOG_LOOP_CHECKS — candidates: refresh relays, other VPS tasks
   (chimera_executor_watch + ibkr_login_watch already have own banners).
8. NEW (minor): transient RED window — loop detector can RED up to ~12min AFTER a loop is
   fixed (newest alarm still recent). Self-clears; arguably correct attention. Note only.

## TRAPS (carry every session — unchanged from ag plus new)

- ssh literal: `ssh omega-new` / `ssh chimera-direct`; journalctl needs `sudo -n`.
- chimera state file on josgp1 = /home/jo/ChimeraCrypto/data/crypto_companion_state.json
  (user is `jo`, NOT josgp1 — ~josgp1 path does not exist).
- chimera logs → ~/ChimeraCrypto/logs/chimera.log (systemd append:), NOT journalctl.
- NEVER git reset on josgp1 (live uncommitted config). No force-push.
- ChimeraCrypto deploy: Mac commit → push → josgp1 `git merge --ff-only origin/main` →
  cmake --build build → sudo -n systemctl restart chimera → grep boot gates in chimera.log.
- Omega GUI deploy: edit omega_desk.html → gen_index_html.py → gui_drift gate → commit BOTH
  → `bash tools/omega_deploy.sh --clean` (header-only change NEEDS --clean; wrapper verifies
  running-binary hash). Right after restart :7779 serves empty for ~seconds — retry, don't
  diagnose.
- feeds_selftest ssh probes w/ pipes MUST use -EncodedCommand (cmd.exe splits `|`).
- version_generated.hpp = build-stamped, do NOT commit.
- Mac sandbox: no `sleep N` chains/until-loops. Windows ssh: one command per ssh.
- IBKR: gateway=java.exe, port 4001 LIVE (4002=paper, dead); never restart Gateway to "fix".
- Desk API legs NOW carry prox fields (prox_valid/win_open/win_move_pct/confirm_dist_bp) —
  campaigns (CAMPAIGN-SELF) do NOT (absent = falsy, GUI handles).

## VAULT STATE

Memory-Chimera: TriggerProximityEmit filed [18-07-2026 19.03]. Memory-Omega:
WatchdogLoopDetector filed + proximity note [19.04]; pin advanced to 2c91f540 = repo HEAD
at handoff (this handoff commit will make it 1 behind — normal pin-lag, auto-ingest next
session).
