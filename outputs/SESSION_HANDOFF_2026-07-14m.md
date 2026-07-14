# SESSION HANDOFF 2026-07-14m — verification session; FULL AUDIT QUEUED for next session

## STATE: nothing in flight. This session was read-only verification. No commits, no deploys.

## NEXT SESSION TASK (operator's explicit ask, context ran out before starting)

**Full audit of the system: "ensure we have no more staleness issues, no dangling
references, circular code, duplications etc."** Scope = BOTH systems (Omega +
ChimeraCrypto) unless operator narrows.

Suggested audit dimensions (fan out as parallel background agents per operator
standing rule feedback-parallel-agents-expedite; one message, many agents):
1. **Staleness** — VPS working tree vs origin/main (omega-new sits at 69e13b5e,
   origin/main at 0112d4ca — 4 doc/tool commits behind, harmless but should
   fast-forward on next deploy); scheduled-task states; feed producers; vault pins;
   warmup CSV ages; `check_branch_freshness.sh`; stale monitor crons pointing at
   retired boxes (omega-vps 185.167.119.59, chimera-vps 154.45.251.118 — grep ALL
   tools/crons for those aliases/IPs, draft PR #4 was the repoint).
2. **Dangling references** — engine_init.hpp entries for deleted/tombstoned engines;
   includes of removed headers; crons/scripts referencing deleted files;
   `adverse_protection_legacy.txt` vs actual files; retired-engine mentions
   (BeFloor, KILL_UPJUMP_CLIPS grid) still wired anywhere; docs pointing at
   ~/omega_repo (wrong path per memory).
3. **Circular code** — header include cycles (codegraph_* MCP is the right tool:
   codegraph_impact / callers on core headers); duplicate ODR-risk definitions in
   headers (the 2026-05-30 class of bug).
4. **Duplications** — duplicated logic across engines that should share helpers;
   duplicate cron entries; duplicate/overlapping selftests; backtest/ dir has ~100
   untracked files (see git status) — decide keep/commit/clean with operator.
5. **Standing audit checks** (CLAUDE.md): ungated-engine audit, GoldEngineStack
   chokepoint grep, mac canary + adverse_protection_audit (3 legacy backfill owed).
6. Use codegraph (code structure) + graphify-omega (docs+code) + second-brain
   tombstone index — all pre-built; don't grep-rederive.

Cautions: read-only audit — NEVER POST/mutate (memory feedback-audit-read-only-never-mutate);
minimize ad-hoc VPS ssh (RAM reaper memory); tombstoned engines hands-off (don't
"fix" or even mention-warn them per memory); ssh omega-new / chimera-direct ONLY.

## VERIFIED THIS SESSION (don't re-derive)
- **Det-ring warm-seed deploy confirmed live**: Mac ChimeraCrypto `cc84d86` on
  origin/main; box chimera-direct `/home/jo/ChimeraCrypto` HEAD `008ee4b`
  (deploy-script box-local commit, content identical), service `active`.
- **Omega GUI hash `69e13b5` is CORRECT/current**: commits since it on origin/main
  = 3 handoff docs + display_truth_selftest.py deploy-grace (0112d4ca, Mac-cron
  tool). Zero C++ touched → running binary is latest binary-relevant commit.
  No redeploy owed. VPS repo 4 commits behind origin/main (docs only).
- FEED-PATH SELFTEST recovered RED→GREEN on its own (14l's top watch item);
  CONSUMER-UP + NO-FALLBACK both pass this session. All selftests GREEN at start.
- ChimeraCrypto Mac working tree: `M include/version_generated.hpp` +
  untracked `backtest/companion_be_mimic_bt` — leftover, harmless.

## WATCH ITEMS (inherited 14l, unchanged)
- First `[CLIP-JF] <COIN>-PJ… OPEN` lines (4 PJ cells: AAVE-PJ4W1, DOGE-PJ3W12,
  ETH-PJ7W24, GRT-PJ5W1 — armed immediately post warm-seed).
- 4 bigcap ladder windows pending booking verify (NOW/CRM/ADBE/BMY).
- First `[IBKR-EXEC] PAPER BUY/SELL XAUUSD.M qty=1` → confirm MGC px.
- Parked operator decisions: SGE-premium salvage, 5m ignition study, USTEC
  bear-gate salvage, GBPUSD ladder maxDD extraction, M1 seed refresh ~Aug.

## TRAPS (carry forward)
- Live boxes: crypto = chimera-direct 143.198.89.54 (user jo, /home/jo/ChimeraCrypto);
  Omega = omega-new 45.85.3.79. NEVER omega-vps / chimera-vps.
- ChimeraCrypto deploy = `DEPLOY_MSG=... tools/deploy_to_box.sh`, working-tree
  (uncommitted) flow; commit on Mac AFTER. Never git pull on box.
- zsh eats bare `===` in Bash tool calls.
- Immediate-entry ban stands EXCEPT 4 live PJ cells (operator override).
  KILL_UPJUMP_CLIPS stays true. BeFloor stays retired.

## SUGGESTED SKILLS NEXT SESSION
- `code-review-and-quality` or `/code-review` for the code-quality dimensions.
- codegraph MCP + graphify-omega MCP (structure + docs graphs, already indexed).
- Parallel Agent fan-out (Explore agents) per audit dimension, one message.
