# SESSION HANDOFF — 2026-07-18ag (ETH low-thr rollout SHIPPED; no-trades answered; watchdog 4002-loop fixed)

Caveman mode. Predecessor: outputs/SESSION_HANDOFF_2026-07-18ae.md (documents S-af).
Operator ended session with "handoff" (hard stop) right after issuing TWO NEW ASKS — see
"NEXT SESSION MUST BUILD" below. Context was low (ctx-budget warning fired).

## SHIPPED THIS SESSION (all deployed + verified)

**1. ETH low-thr rollout — ChimeraCrypto `48383b5` (Mac+box), LIVE on josgp1.**
Operator: "roll the same setting out for eth now" = BTC S-18ab/ac low-thr quads applied to ETH.
- +11 cells: ETH-UJ05-BECASC-{W1,W2,W4,W12} det=0.5% · ETH-UJ10-BECASC-{W1,W2,W4,W12} det=1.0%
  · ETH-UJ15-BECASC-{W1,W4,W12} det=1.5% fill-in (W2/W8 = live F/S, not duplicated). ETH 14 books;
  fleet 144→155 companions.
- Cert: eth_ujmimic_15_becascade_bt vs HEAD 01a30d2 (honest 17f fills), UM_COIN=ETH, confirm60
  BE-ENTRY anchored, RT=30/60bp, ETHUSDT_1h 2021→2026-07-18 (tail appended via DIRECT klines —
  fetch_coin_1h.py clobbers pre-2023, known trap), integrity CLEAN. ALL 108 cells PASS standalone
  gate (base AND 2×, omit-2022). Baseline reproduced wired F/S worst to the digit (−997.5/−1086.1).
  Picks = best 2×-net per lane, g0.20 preferred within 5% of max (PF 2–3×, sumNeg ~half; BTC
  precedent); W12 lanes g0.75. retire=2× own worst. mimic_floor, cap8, reclip0, catchup 24h.
- Verified: josgp1 build=48383b5==HEAD, RUNTIME MODE LIVE, [MIMIC-FLOOR-GATE] 155/155 floored 0
  VIOLATION, [PROFIT-LOCK-GATE] 157/2/0, 10/10 Mac suites, CC chip green on 48383b5, desk 159 legs
  incl. all 11 new ETH books. Findings: Crypto/backtest/ETH_LOWTHR_MIMIC_FINDINGS_2026-07-18.md
  (Crypto cabd985). Vault: Memory-Chimera EthLowThrMimicCells.md + index + log.

**2. gateway_watchdog 4002→4001 — Omega `9efbee1f`, VPS patched live + repo synced.**
Root cause of operator's "Start IBKR NOT up after 120s" spam: gateway logs in LIVE → API port
4001 (java listener, 4 API clients attached); watchdog hardcoded 4002 (paper) → every 5-min tick
relaunched IBC against the healthy session. Fixed $Port=4001; 06:38:43Z tick ran rc=0 with NO
WARN (healthy path) = loop dead. VPS repo ff'd to 9efbee1f. Bak: gateway_watchdog.ps1.bak_20260718.
Gateway itself was NEVER down. Memory-Omega log entry filed.

**3. OmegaSeedRefresh "never ran" RED — benign, cleared.** Task recreated 07-18, first trigger
tonight 23:30 VPS; manual kick rc=0. Feeds selftest RED clears. (--port 4001 = correct data port.)

**4. "No trades wtaf" ANSWERED (twice, w/ data).** No blocker. Tile % = ROLLING 24h change; the
pump (BTC +1.8% accrued 17 Jul 09:00–18:00Z, bulk 15–18Z; ADA/INJ +5%) fell inside the 93-restart
outage → confirms crossed while down → bounded catch-up CORRECTLY refuses backdated entries.
Since stable boot 05:39:55Z market flat: max up-tick INJ +0.47% < lowest det 0.5%. Engine healthy
whole time. Speed recipe: Binance klines 1m from boot-epoch (health json ts−uptime_s) → since-boot
move + maxUp per coin; contrast w/ /ticker/24hr.

## NEXT SESSION MUST BUILD (operator's final message, verbatim asks)

**A. "detection for loop and errors"** — the watchdog IBC-relaunch loop ran ~20min undetected
(operator saw it via RDP window, not a banner). Build the loop/error detector class:
- Started (NOT finished): plan was feeds_selftest.py new check — ssh omega-new read
  gateway_watchdog.log tail; ≥3 consecutive trailing "WARN: IBC launched but port ... not
  listening" → RED "IBC RELAUNCH LOOP"; plus IbkrGateway task Last Result nonzero → RED.
  CRITICAL_FEED_TASKS list at tools/feeds_selftest.py:49, vps-task render ~line 677.
- Generalize the class: same chimera restart-loop precedent (S-18ad) — any *_watchdog/task that
  retries N× consecutively must alarm, not just log. Candidates: gateway_watchdog, ibkr_login_watch,
  chimera_executor_watch (has it), refresh relays.

**B. "different metric for crypto — see when a symbol is moving towards a trigger, not a 24h
percentage that means nothing"** — trigger-PROXIMITY metric on the crypto tiles/mimic panel:
- Per coin: current in-window up-move vs its LOWEST live det threshold, e.g. "+0.32%/0.50% (64%)"
  + bar/color ramp; window-open state visible (pairs with carried WINDOW-chip item #1).
- Data does NOT exist in legs payload (confirmed S-af: win/det fields absent). Needs josgp1
  emit_companion_state extension: per-cell {win_open, win_move_pct, det_thr, confirm_dist_bp}
  or per-coin best-proximity roll-up → relay (120s) → :7779 → desk tiles.
- Engine det rings live in UpJumpLadderCompanion (det_w/det_thr per cell); emit side is in the
  state writer on josgp1 (grep emit/companion_state in ChimeraCrypto src). GUI edit flow:
  tools/gui/omega_desk.html → gen_index_html.py → gui_drift gate.

## CARRIED OPEN (from ae, still valid)

1. WINDOW-state chip on mimic panel (det-window-open vs idle) — now folds into ask B.
2. Protection selftest [1] weekend-skips companion freshness — wrong for 24/7 crypto; unfixed.
3. Omega-side build-truth chip (TRADING beacon precedent) — suggested, not demanded.
4. Forward-watch 16 BTC + 11 ETH low-thr books; first confirms ring entryBell.
5. make_becascade_cell rt=28 per-coin measured-cost note; lower-thr quads for OTHER coins (judge
   standalone per coin — SOL/BNB etc. still at 1.5%); UJ naming rename (needs state-key migration,
   not approved).
6. Orphan mgc_15m_hist.csv nightly refresh with no reader — consider dropping recipe.

## TRAPS (carry every session)

- ssh literal: `ssh omega-new` / `ssh chimera-direct`; journalctl needs `sudo -n`.
- chimera logs → ~/ChimeraCrypto/logs/chimera.log (systemd append:), NOT journalctl (journal has
  only start/stop lines — greps for engine lines come back EMPTY, don't misread as "no seeds").
- NEVER git reset on josgp1 (live uncommitted config/live_config.json). No force-push.
- ChimeraCrypto deploy flow that works: Mac twin commit → push origin → josgp1 `git merge
  --ff-only origin/main` → cmake --build build → systemctl restart chimera → grep boot gates.
  (Avoids the e3ed03d/43816e0 twin-commit reconcile mess.)
- fetch_coin_1h.py REWRITES csv from 2023 — never run on full-history csv; append tail via
  direct Binance klines instead.
- version_generated.hpp = build-stamped, do NOT commit.
- Mac sandbox: no `sleep N` chains, no until-loops (blocked). Single checks; task Last Run Time
  via schtasks confirms silent-clean watchdog runs (healthy path logs nothing).
- Windows ssh: `echo`/`===` unquoted breaks findstr chains — one command per ssh or powershell -c.
- IBKR: gateway=java.exe (not ibgateway.exe) — check the LISTENER (netstat 4001), never restart
  Gateway to "fix" (4 live clients attached).
- Health-chip staleness: watch 60s + relay 120s + poll 15s ≈ 3.5min worst; GUI threshold 360s.
- Desk API legs: tag/sym/clips only — no state fields; don't infer.

## VAULT STATE

Memory-Chimera: EthLowThrMimicCells filed (18-07-2026 18.36). Memory-Omega: log entries for
watchdog fix + seed-refresh + pin advanced 4e63b0c3→38e15821 (was 1-behind, clear); NOTE: repo
has since moved to 9efbee1f (watchdog commit) + this handoff commit — advance pin on next ingest.
