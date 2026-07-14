# SESSION HANDOFF 2026-07-14v (manual "handoff" hard-stop — MID-TASK, crypto seed audit OPEN + operator FURIOUS order in flight)

Session S-2026-07-14 (v): resumed 14u → verified state (all green) → **operator re-open order
executed: sub-30m BE-mimics wired+deployed (bm)** → operator asked "why INJ/NEAR crypto not
firing" → diagnosis found **INJ (and ~55 other coins) have NO warm seeds on josgp1** → operator
(furious): **"CHECK EVERY SYMBOL NOW AND FIX THIS SHIT"** → hard-stopped by "handoff" mid-audit.

## ⚠️ THE OPEN ORDER (do this FIRST, it is owed and the operator is angry)

**Chimera/josgp1 seed audit + fix, EVERY symbol** (memory feedback-never-half-the-symbols applies):
- Current boot (PID 2937961, build 770b9c7, up since 06:26 UTC, NRestarts=0): **only 5 symbols
  got SEED lines: nearusdt/thetausdt/sushiusdt/adausdt/dotusdt** (= the REGIME_SWITCH D1
  parents; 008ee4b "det-ring warm-seed" covers det_w companion cells only).
- **~60 coins have ARMED engines this boot with ZERO seed** (SUI 22, XRP 19, ETH 19, NEAR 18*,
  SOL/LINK/BTC/BNB 17, DOGE 16, JTO 15, TIA/JUP 14, … INJ 9, etc — *NEAR's 18 armed engines
  include many beyond its 1 seeded book). Unseeded engine with lookback 20 on H8/H12/D1 =
  silent for 7–20+ days = the exact Omega 2026-05-20 cold-start class.
- BUT before "fixing" blindly: many ARMED lines are LEGACY-EDGE objects (registry state
  DISABLED, 285 culled per-symbol EdgeEngines — constructed + print ARMED but NOT on the
  g_slots tick path). **Step 1 = determine which armed engines are actually LIVE-PATH**
  (EDGE-SLOTS after the 13-07 cull = ~5 REGIME_SWITCH parents; plus XSEC sleeves, RIPRIDER,
  campaign, jump-floor/sweet clip cells). Seeding culled zombies = wasted work; the fix scope
  = live-path engines/cells missing seeds. THEN extend the 008ee4b ring-seed (or equivalent)
  to every live-path symbol, deploy, verify SEED lines per symbol at boot.
- Box: `ssh chimera-direct` (143.198.89.54, josgp1). Repo ~/ChimeraCrypto, service `chimera`
  (systemd). Journal: `journalctl -u chimera --no-pager -S today | grep -a <PID>`.
  NOTE: ~/ChimeraCrypto/logs/chimera.log is a STALE May-16 file (build 7fa3c45) — do NOT read
  it as current; journald is the live log. Mac mirror repo = /Users/jo/Crypto.

## INJ/NEAR "not firing" — diagnosis so far (report to operator, fold into fix)

- **Feeds fine:** both injusdt + nearusdt WS-subscribed (bookTicker/aggTrade/depth5) every boot.
- **NEAR:** genuinely wired + seeded live books = NEAR-REGIME_SWITCH parent (D1, fires on regime
  flip — rare by design) + NEAR-REGIME-BEMIMIC clip (fires only when parent opens) +
  NEAR-UJ4-SWEET (self-detect 1h/+4% jump — no such jump since 13-07 wiring). No fires ≈
  trigger scarcity, not breakage — BUT its other 17 armed engines are unseeded legacy/edge
  objects (see audit).
- **INJ:** NO live-path cell exists — INJ absent from every current validated family
  (sweet-spot 7, jump-floor 4 = ETH/AAVE/GRT/(DOGE culled 770b9c7), REGIME_SWITCH 5, campaign).
  Its 9 armed engines = TSMOM/ICHI/BOLL legacy family (TSMOM proven DEAD 0/54, culled 13-07
  PHASE3). Also 0 seeds. So INJ "not firing" = nothing valid wired + cold anyway. Operator
  needs told this straight: if he wants INJ firing it needs a validated cell first (sweep) —
  check whether INJ was in the 2432-cell 19-coin sweep (ec81011) or genuinely never swept.
- Registry (config/engine_registry.json): EDGE-SLOTS SHADOW, LEGACY-EDGE DISABLED,
  XSEC-BTC/XSEC-BR/RIPRIDER SHADOW, UPJUMP-GRID (note truncated — re-read; 15fba20 made it 32
  jump-floor/sweet cells with explicit operator immediate-entry override for jump-floor,
  518ab3c+770b9c7 culled to ETH/AAVE/GRT).

## COMPLETED THIS SESSION (all shipped, nothing owed)

1. **S-2026-07-14bm `f09518fc` DEPLOYED+VERIFIED (omega-new, binary hash-verified, boot
   "15 trigger books"):** GoldDon15mMimic (be0.10/arm1.0/lc0.5/cap96, T PF1.80
   +$10,789/+$8,415@2×, DD 2.2%, worst −0.55%) + GoldDon10mMimic (be0.10/arm1.0/lc1.0/cap144,
   T PF1.37 +$14,028/+$9,227) — SHADOW 2-leg T gb8/W gb20. Operator re-opened the 14u
   tombstone; NEW basis = untested arm>0.5 dimension; validated at **1m intrabar truth ONLY**
   (no close-grade column) via new `backtest/gold_sub30_mimic_1m_bt.cpp` on real DUMP_ENTRIES
   streams. **arm≥2/arm99 family REJECTED** (+$47k bait = no-trail time-stop trend clone) —
   recorded so nobody re-adds it. Findings `backtest/GOLD_SUB30M_MIMIC_RECHECK_2026-07-14.md`.
   Canary GREEN. Vault: GoldTrendMimicLadder.md + GoldSub30mStudy2026H1.md (re-open section) +
   index.md + log.md updated, pin advanced → f09518fc.
2. Vault pin-advance housekeeping (1616358e handoff-doc commit).
3. State verify: OmegaSeedRefresh Next Run tonight **23:30 UTC** (verify Last Result 0 +
   mgc_*_hist timestamps advance — STILL PENDING); shadow ledger alive, zero closes yet from
   the 7 gold engines (expected cadence).

## CARRIED (unchanged from 14u + this session)

1. **Chimera seed audit+fix (the open furious order above) — P0 next session.**
2. Tonight 23:30 UTC OmegaSeedRefresh first scheduled run verify.
3. Shadow-ledger watch: GoldDon15m/10m parents + the 2 NEW mimic books (tags
   GoldDon15mMimic/GoldDon10mMimic) + the 6d64d5de five.
4. Mimic M1/tick-vs-live parity re-check before ANY live flip (now applies to all 7 mimic books).
5. ConnorsRSI2 runtime cost-gate backfill before live; adverse-protection legacy backfill
   (8 headers); lot-size revisit (memory project-revisit-lot-sizes).

## TRAPS / NOTES for the next session

- Chimera log trap: logs/chimera.log = May-era corpse; journald is truth. journalctl `-S 'YYYY-MM-DD HH:MM'` needs the quoted form (unquoted 06:26 → "Failed to add match").
- ARMED ≠ live-path on Chimera (legacy objects print ARMED). Wiring truth = g_slots pushes +
  CLIP-INIT lines + registry states. Don't seed zombies; don't report zombies as "engines".
- NEAR OVERLAY warm-start warning at boot: `cannot open data/klines_spot/NEARUSDT_1h_extended.csv`
  → overlay rank=-1 mult=1.00x (inert, but part of the seed-gap picture).
- Omega side all green/deployed — binary f09518fc == origin/main == VPS tree; no Omega work owed
  except the 23:30 verify.
- ssh forms: `ssh omega-new "..."` (Omega VPS) / `ssh chimera-direct "..."` (crypto live box);
  chimera-vps alias = dead box, never use.
- Operator ordered mimics despite prior NO — pattern: he re-opens tombstones explicitly; honor
  by finding genuinely NEW basis + truth-grain validation, never by re-running the dead space.
- Caveman mode active (full). Context-low was flagged; this doc is the hard-stop deliverable.

## Suggested skills next session
- None required upfront; `superpowers:systematic-debugging` if the seed-fix regresses boot;
  vault mandate applies (Memory-Chimera for the seed fix, Memory-Omega already current).
