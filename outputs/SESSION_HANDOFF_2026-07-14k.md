# HANDOFF 2026-07-14k — 17 PJ jump-floor cells WIRED+DEPLOYED (box 15fba20); NEXT = display-truth RED + trigger table + last-month BT (operator's live ask, interrupted)

Session ~13:42–14:1x NZ Tue 14-07. Caveman active. Resumed from 14j handoff.
Copy to `outputs/SESSION_HANDOFF_2026-07-14k.md` + commit (`git add -f`, outputs/ gitignored).

## THE PENDING TASK (operator's live ask — do FIRST, was interrupted by handoff call)
Operator (with screenshot of Mac notification "DESK SHOWS WRONG DATA / DISPLAY-TRUTH RED …
(ros… symbols); see /tmp/display_truth…"):
**"more warnings and show me this tble with triggers and run a backtest on these engines using
the last months data please and show me table results"**

Three parts:
1. **DISPLAY-TRUTH RED**: notification is NEWER than log tail I read. I read
   `/tmp/display_truth_selftest.log` — entries from 07-12 are all GREEN. Grep for the RED:
   `grep -n "RED\|FAIL" /tmp/display_truth_selftest.log | tail`, and read newest entries at
   file END. Notification text mentions "(ros… symbols)" — probably ROSTER symbols mismatch —
   likely cause: **today's deploy grew UPJUMP-GRID 15→32** (17 new PJ cells); the selftest/
   desk copy may expect the old roster ("32/32 cells" in the GREEN logs was the OLD total =
   15 grid + 4 campaign + 13 other; new total larger) or desk hasn't re-synced new tags.
   If so: fix = update guard's expected roster / desk sync, NOT the engine. Also "more
   warnings" = surface any other active warnings (check /tmp/*selftest* logs + session-start
   selftest outputs).
2. **"show me this tble with triggers"**: present the 17 wired PJ cells table WITH trigger
   mechanics (detect W/thr on H1 closes, entry=next tick after detect close, pre-BE stop,
   BE-floor at +20bp close, trail g, reversal exit j≤−thr, fresh-jump re-arm). Table lives in
   `Memory-Chimera/wiki/entities/UpJump2pctSpotParent.md` (percoin table) + cell configs in
   ChimeraCrypto `src/main.cpp` `_pj_cells` block (search "PER-COIN JUMP-FLOOR CELLS").
3. **Last-month BT (2026-06-14→07-14), per-cell, table results**:
   - Local 1h CSVs: `/Users/jo/Crypto/backtest/data/<COIN>USDT_1h.csv`, ~48,426 rows; ts
     SECONDS col1, header row. Freshness UNKNOWN (check `date -r $(tail -1 f | cut -d, -f1)`;
     BSD awk has no strftime). If stale, refresh from Binance public REST klines (data-only
     source fine; NEVER via omega-new:4002 — feedback-no-bulk-pulls).
   - Method: filter each CSV to last-month rows PLUS ≥W+1 warmup bars before cutoff, write to
     scratch `data/` dir with a COPY of `/Users/jo/Crypto/backtest/upjump2pct_be_bt` (binary
     reads `data/<COIN>USDT_1h.csv` relative cwd; COPY, never symlink —
     feedback-scratch-symlink-clobber-live). Run `./upjump2pct_be_bt trades COIN W thr s g`
     per cell → n, net, trade list. Table: coin, cell, n, net bp, BE-scratches, negs, worst.
     Expect many coins n=0-2 (one month, high thr) — honest, show it.
   - Cell configs (coin W thr s g): AAVE 1 4 0 1.0 · ADA 8 8 0 1.0 · AVAX 2 8 0 1.0 ·
     BCH 1 5.5 0 0.5 · BNB 12 7 0 1.0 · BTC 1 4 0 1.0 · DOGE 12 3 0 1.0 · ETH 24 7 0 1.0 ·
     GRT 1 5 0 1.0 · LINK 1 6 2 1.0 · NEAR 1 5.5 0 1.0 · OP 1 4 0 0.3 · SOL 24 8 0 1.0 ·
     TRX 2 4.5 0 1.0 · UNI 3 8 0 0.3 · XLM 1 5 0 1.0 · XRP 8 12 0 0.3

## DONE THIS SESSION (do NOT redo)
1. **Handoff 14j committed** Omega `b3ea5514`; Omega vault pin advanced to b3ea5514.
2. **Percoin lever sweep run + delivered**: 17/19 VIABLE (LDO+LTC NO-CELL). Full table +
   concentration tiers in `Crypto/backtest/UPJUMP2PCT_SPOT_PARENT_FINDINGS.md` (Crypto repo
   `cfe8ac2`) + vault `Memory-Chimera/wiki/entities/UpJump2pctSpotParent.md`. `trades` dump
   mode added to harness. Robust tier: OP/XRP/TRX/ADA/UNI/SOL/AAVE/AVAX; fragile lottery:
   BTC(90% net = ONE 2024-08-05 trade)/LINK/BNB/DOGE/BCH/XLM.
3. **2022 question answered**: y2022 display-only per operator's own rule; NOT in gate. He
   called it irrelevant → optionally drop y22 column from future tables.
4. **OPERATOR GO ("add these to our trades as you have them here") → 17 cells WIRED + DEPLOYED
   LIVE (shadow)**:
   - New `jump_floor` mode (3rd mode) in `ChimeraCrypto/include/core/UpJumpLadderCompanion.hpp`
     — immediate entry at first tick after detect close, optional pre-BE stop, BE-floor once a
     close covers +20bp, trail g<1, reversal exit, fresh-jump re-arm. det_in_/det_entry_
     mirrored for det-state persistence; single display leg "J1" (arm=0.2% → armed chip ==
     floored). NOT the retired be_floor family (that waits for +BE before opening).
   - 17 `_pj_cells` in `src/main.cpp` (tags `<COIN>-PJ<thr>W<W>`: BTC-PJ4W1, XRP-PJ12W8 …);
     feeds = holder EdgeEngines (SWEET pattern, never wired). retire_bp = −2× BT maxDD/cell.
   - **Parity test** `ChimeraCrypto/backtest/jf_parity_test.cpp` (binary /tmp/jf_parity):
     BTC 30 clips +7953 vs BT +7974 (n exact); LINK 33/+19215 vs 32/+19257; TRX 103/+17649 vs
     100/+18252; ETH 231/+17299 vs 223/+18498; OP 205/+14336 vs 196/+16507. n-skew = BT
     re-arm-skip loop artifact (BT skips exit-bar close for re-arm; live processes every close).
   - **Deployed** via `tools/deploy_to_box.sh` (guard passed): josgp1 build `15fba20` verified
     active; all 17 `[CLIP-INIT] … JUMP-FLOOR … shadow=1` confirmed; UPJUMP-GRID 15→32; 17 PJ
     tags flowing in box `data/crypto_companion_state.json`.
   - Mac ChimeraCrypto `7cde149` pushed. Vault entity+index+log updated (WIRED-SHADOW).
5. **Mimic question answered**: 15 mimic books (5 REGIME-BEMIMIC + 10 SWEET) all confirm=20bp
   (BE-entry, never open underwater) + 4 campaign cells (confirmed entry, mimic lots OFF);
   0 retired, all ×1, all shadow. PJ cells = opposite class by design (immediate, floor-at-BE).

## WATCH ITEMS
- First `[CLIP-JF] <COIN>-PJ… OPEN` lines on next qualifying jump; W24 cells (ETH/SOL) warm
  ~25h post-deploy (deployed 14-07 02:08 UTC).
- DISPLAY-TRUTH RED = the pending task (likely roster-count drift from today's deploy).
- 4 bigcap ladder windows PENDING: NOW/CRM/ADBE/BMY (booking verification).
- First `[IBKR-EXEC] PAPER BUY/SELL XAUUSD.M qty=1` → confirm MGC px.
- Pending operator decisions: SGE-premium salvage, 5m ignition study, USTEC bear-gate salvage,
  GBPUSD ladder maxDD extraction, M1 seed refresh (~Aug).

## TRAPS
- Live boxes: crypto = chimera-direct/josgp1 143.198.89.54; Omega = omega-new 45.85.3.79.
  Never chimera-vps/omega-vps (dead).
- ChimeraCrypto deploy = scp-based `tools/deploy_to_box.sh` (box carries intentional local
  state; NEVER git-pull on box). Guard check_box_sync.sh compares Mac COMMITTED base vs box.
- Mac awk = BSD (no strftime); zsh doesn't word-split unquoted vars (use `bash -c` for loops
  with `set -- $spec`); avoid bare `===` tokens.
- upjump2pct_be_bt binary reads `data/<COIN>USDT_1h.csv` relative to cwd.
- Immediate-entry ban stands EXCEPT the 17 wired PJ cells (operator override 14-07).
  KILL_UPJUMP_CLIPS stays true. BeFloor family stays retired — jump_floor is NOT it.
- Any reweighting/disabling of fragile-tier cells = operator decision, present first.

## SUGGESTED SKILLS NEXT SESSION
- None mandatory. For display-truth RED: read the guard script FIRST
  (`ls /Users/jo/Omega/tools/*display*`) — the RED may be CORRECT behavior flagging today's
  roster growth → update the guard's expected roster / desk sync, don't touch the engine.
- File display-truth outcome to the matching vault if config/guard changes.
