# HANDOFF 2026-07-14q — 14p queue CLOSED: rdagent P&L root-caused+fixed, git-add trap gated, P3 sweep items 9-14 shipped

## State
- main == origin/main == VPS tree == `5cc44fe1`. VPS running binary `f3cc7803` (correct:
  ar/as/at are tooling-only, no C++ — same precedent as bb3753d1). Canary green end-to-end
  with 4 NEW gates (deadbox, ungated, roster-parity, + widened mimic glob).
- Vault: 14p backfill INGESTED (pin 405a0b36→ad5eb2c8; GoldExecSpreadBasis/OmegaMacroRegime/
  AdverseProtectionMandate created; SeedFreshnessSafeguard/MondayRiskOnEngine/
  PositionPersistence/BeCascadeEngines updated). ar/as/at vault filing owed → agent running
  at session end; VERIFY it landed (pin should be 5cc44fe1).

## rdagent P&L cross-wire (14p's open bug) — ROOT CAUSE ≠ panel bug
`refresh_close_ibkr.py` pull(): ONE reqId (9000) for every symbol, no cancel on timeout,
callbacks unfiltered → timed-out symbol's late bars streamed into NEXT symbol's buffer.
CRM precedes ADBE in BIGCAP → ADBE column = CRM series for 251 rows ('1 Y' pull) in Mac
data/rdagent/sp500_long_close.csv. Desk marked ADBE at CRM's price (−$835 shown, real +$174);
paper fill booked at 171.22 (price ADBE never traded). ALL freshness guards stayed green.
- FIXED (`b37707e5` ar): unique reqId + filter + cancel; RDA_IBKR_DATA_ONLY=1 refusal
  (production-gateway bulk-pull ban now enforced in code); NEW close_csv_guard.py aliased-
  column tripwire in ALL 3 producers (ibkr/yf/vps_stockmover — writer aborts, keeps last-good);
  display_truth_selftest NEW [5] BASKET-PNL (recompute per-row from close CSV + fill ledger +
  alias signature; DTS_INJECT=pnl_crosswire negative test).
- DATA REPAIRED (untracked): 251 ADBE rows restored from yfinance (CRM column cross-validated
  <0.5c first); phantom fill re-priced 171.22→230.61 (CRLF file — edited bytes); shadow
  executor re-run + pushed: desk /api/rdagent_book serves pnl_usd +163 (verified). Backups in
  session scratchpad. VPS slim CSV verified CLEAN (independent pull — ladder engine never affected).
- Nightly qlib retrain picks up repaired history automatically (model trained on 1yr bad ADBE).

## git-add silent-partial-stage trap (14p TRAP) — 3 layers (`4ff542bd` as)
1. Claude PreToolUse hook DENIES git add/stage/commit/push with stderr→/dev/null (wired
   .claude/settings.json → scripts/git_hooks/claude_block_git_stderr_suppress.sh). Live-fire
   verified; NB it also matches the pattern inside commit-MESSAGE text (coarse, deliberate —
   reword messages).
2. .git/hooks/pre-commit prints [UNSTAGED-M] banner of tracked-modified files missing from
   every commit (source scripts/git_hooks/pre-commit; fresh clone: run scripts/git_hooks/install.sh).
3. CLAUDE.md Edit Discipline precondition.

## P3 batch (`5cc44fe1` at) — sweep items 9-14, all negative-tested
9 threshold registry: SHARED_FEED_MAX_AGE_TD in feeds_selftest.py, data_health imports it;
data_health +companion/heartbeat rows +file-level sp500 staleness. 10 scripts/
ungated_engine_audit.sh (ENTRY_RE derived from adverse audit at runtime; 31-entry allowlist;
in canary; CLAUDE.md §1 repointed). EXPOSED: ConnorsRSI2 ENABLED shadow w/o runtime cost-gate
(backfill owed before live flip); Survivor cost-gate audit owed at promotion. 11
fail_verdict_guard sleeve/boot-lambda/roster shapes: 57 enabled visible (was 22). 12
engine_state_audit thr() deleted → imports seed_freshness_audit (XAUUSD_M1 45d fixed;
seed_freshness_audit now import-safe under __main__ guard). 13 deadbox_ref_audit.sh in canary
(exact-count allowlist, above AND below expected = FAIL); omega_deploy HOST=omega-vps
escape hatch → FATAL; migrate_stall_ledgers hard-fail historical; feeds_selftest 6 stale
ssh-form docstrings repointed; PR #4 = merged IP-propagation, no overlap. 14
roster_parity_audit.py in canary (STOCKS==BIGCAP_LAD 45==45; IbkrExecutionEngine FUT ⊆
bridge INDEX_FUTURES, gold GC/MGC exec-only documented; ps1 $Symbols+comments); mimic glob
+= *Ladder*.hpp; 4 orphan seed recipes removed (zero consumers verified);
scripts/rebuild_warmups.py zombie DELETED. 4 orphan CSVs remain in phase1/signal_discovery
(data, unreferenced — delete if wanted); CLAUDE.md warm-seed prose still names GER40_H4.

## WATCH (inherited + new)
- Tonight 21:30 UTC OmegaMacroRegime first scheduled run — content ts must advance past
  seeded 07-10 bar (else dropna() row-intersection in fetch_macro_regime.py). 23:30 UTC
  OmegaSeedRefresh first run (USTEC.F_H4/GBPUSD_H1/mgc recipes).
- Next OmegaStockMoverFeed run (22:35 UTC) exercises close_csv_guard on VPS — confirm task
  stays green (guard abort would surface as file-age staleness).
- Inherited: CLIP-JF PJ opens, bigcap ladder windows, IBKR-EXEC XAUUSD.M paper fills.

## Remaining queue
- ConnorsRSI2 runtime cost-gate backfill (documented in ungated allowlist).
- Adverse-protection legacy backfill 8 owed (opportunistic).
- Vault ar/as/at filing — verify the end-of-session agent landed it (pin → 5cc44fe1).
- Optional: delete 4 orphan warmup CSVs; root-cause WHY yfinance single-row dup pairs
  appeared on 07-13 row (8 pairs were single-day coincidences — verified NOT corruption,
  but producers now guarded anyway).

## Standing traps (carry-forward)
outputs/ gitignored → git add -f handoff docs. ssh form literally `ssh omega-new "..."`.
zsh eats bare `=`-prefixed words. VPS symbols.ini locally modified (pre-existing, left).
deadbox allowlist reasons must not contain apostrophes (single-quoted shell string).
factor_basket_orders.csv is CRLF — edit bytes, not sed.
