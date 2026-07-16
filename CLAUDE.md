# Omega Repo — Operating Rules for AI Sessions

These rules apply to ANY AI session (Claude Cowork, Claude Code, etc.)
working in this repository. They supplement the operator's global
user-preferences (full-file edits, 70% chat warning, never modify
core code without instruction) with project-specific safeguards.

> **START HERE: read `OMEGA.md`** — the master system reference (data inventory
> in /Users/jo/Tick, backtest recipe, real costs, deploy, shadow ledger, engine
> status). It exists so sessions stop re-deriving where data lives / how engines
> wire / what costs are. Backtest specifics: `backtest/ENGINE_BACKTEST_REGISTRY.md`.

## MANDATORY: update the vault on EVERY deploy (operator rule, 2026-06-21)

A deploy (`OMEGA.ps1 deploy` / VPS rebuild+restart) is **NOT complete** until the Memory-Omega
vault (`/Users/jo/Memory-Omega`) reflects what shipped. The vault update is part of the deploy
checklist, not optional cleanup. After verifying the running VPS hash, BEFORE declaring the deploy
done, you MUST:
1. **Entity page** — create/update `wiki/entities/<Engine>.md` (YAML frontmatter + `[[wikilinks]]`):
   what changed, faithful/live figures, status, **commit hash** (tied to the verified running hash).
2. **index.md** — add/update the one-line pointer.
3. **log.md** — append `## [DD-MM-YYYY HH.MM] <op> | <target>` (NZ time:
   `TZ='Pacific/Auckland' date '+%d-%m-%Y %H.%M'`) naming the commit + what deployed.

Why: the vault is the system of record and went 1106 commits stale once (separate cwd-gated session);
deploys are exactly when drift creeps in. Tying vault-update to deploy keeps it current by construction.
See memory `feedback-vault-update-on-deploy` + the SessionStart memory-wiki mandate.

## Edit Discipline

**Commits permitted.** Relaxed by operator instruction 2026-05-14a.
AI sessions may run `git commit` and `git push` for work the operator
has asked for, without re-asking each time. The operator can still
override per-session at any time by saying "don't commit yet" or
similar.

Preconditions that still apply before any commit:
- Mac canary build green (see §"Build Verification"). Sandbox-side
  `g++ -fsyntax-only` is necessary but NOT sufficient.
- `git diff` reviewed: only the intended changes, no whitespace drift,
  no accidental other-file edits.
- Commit messages reference the standing `S<N>` numbering scheme.
- Unrelated changes are NOT bundled into one commit — split via
  `git add -p` when a working tree contains independent chunks.
- For engine_init.hpp settings touching `LOSS_CUT_PCT` / `BE_ARM_PCT` /
  `BE_BUFFER_PCT` / `enabled`, the comment block directly above the
  line has been read and the change is consistent with it (or
  knowingly overrides it with explicit new evidence).
- For S63 management-path additions, the call-site activation is in
  the same commit. No more "fields exist, check never runs" commits.
- **Never suppress git add/commit/push stderr** (no `2>/dev/null`).
  After staging, `git status --short` must show no unexpected `M`
  lines; after committing, `git show --stat` must list every intended
  file. History (S-2026-07-14ap): `git add <list> 2>/dev/null` errored
  silently and the commit captured only pre-staged content (1 of 6
  files). Enforced structurally: a Claude PreToolUse hook
  (`scripts/git_hooks/claude_block_git_stderr_suppress.sh`, wired in
  `.claude/settings.json`) DENIES such commands, and the pre-commit
  hook (`scripts/git_hooks/pre-commit`, installed via
  `scripts/git_hooks/install.sh` — run once per fresh clone) prints an
  `[UNSTAGED-M]` banner listing tracked-modified files missing from
  the commit.

History: this rule originally said "no commits without explicit
go-ahead", added after a part-G session (2026-05-13) edit was
committed without checking with the operator. Relaxed 2026-05-14a
after the operator decided the friction of re-asking each commit
outweighed the risk of a misplaced auto-commit, given the
preconditions above.

**Targeted edits on engine files, full-file on short files.** The
operator's global user-preference says "always full file." For
multi-thousand-line files in this repo (engine_init.hpp at ~3,500
lines, CrossAssetEngines.hpp, GoldEngineStack.hpp, the larger
*Engine.hpp files, etc.), full-file rewrites are slow, error-prone,
and waste context. Default to `Edit` with surgical
`old_string`/`new_string` blocks on those.

Use full-file Write when:
- The file is short (under ~300 lines).
- The operator explicitly asks for "full file" for a given task.
- The change is a wholesale restructure that's easier to read as a
  fresh write than a diff.

No upfront confirmation needed — proceed with targeted on big files
unless the operator says otherwise. Operator can flip to full-file
per-session at any time by saying so. Project-specific override of
the global "always full file" preference, agreed 2026-05-14a.

**Never modify core code without an explicit instruction.** Core files
include but are not limited to:

- `include/OmegaCostGuard.hpp` — cost gate (entry filter)
- `include/OmegaTradeLedger.hpp` — trade ledger
- `include/SymbolConfig.hpp` — symbol configuration
- `include/OmegaFIX.hpp` — FIX infrastructure (marked IMMUTABLE)
- `src/api/OmegaApiServer.hpp` — read-API for omega-terminal
- `include/GoldPositionManager.hpp` (TICK_SIZE etc.)
- Any file with `IMMUTABLE` in its header

Engine files (`*Engine.hpp`, `GoldEngineStack.hpp`, `CrossAssetEngines.hpp`)
and configuration (`engine_init.hpp`) are NOT core — engine logic and
per-instance config are normal edit targets.

## Build Verification

The Linux sandbox cannot run `cmake` (no `cmake` binary; the project
pulls in `winsock2.h` which Linux doesn't ship). Use sandbox-side
`g++ -fsyntax-only -Iinclude -x c++ include/<file>.hpp` for a
necessary-not-sufficient check on individual headers. The sufficient
check is the Mac canary build (operator-side):

```bash
cd ~/Omega    # the ONE working tree/build dir (operator 2026-07-02: there is
              # no separate ~/omega_repo -- only ~/Omega. mac fs is case-insensitive.)
cmake --build build --target OmegaBacktest -j
bash scripts/mac_canary_engines.sh
```

**Do NOT** use the bare `cmake --build build -j` — that target needs
Windows-only headers and always fails on macOS, even though the
surrounding green "Built target X" lines can make it look like a pass.

### Mac canary engine-header check (added 2026-05-30)

`scripts/mac_canary_engines.sh` syntax-checks engine headers that
`OmegaBacktest` does NOT include (DonchianEngine, EmaPullbackEngine,
IndexFlowEngine, XauTrendFollow4hEngine, SurvivorPortfolio, L2Globals).
Catches the MSVC-class C++ errors that slipped through OmegaBacktest in
the 2026-05-30 deploy: `AtomicL2` undeclared (no `L2Globals.hpp`
include), mixed designated/positional aggregate-init, ODR-violating
header definitions.

Run BEFORE any commit that touches an engine header. Adds ~5 seconds.

History: added after the 2026-05-30 VPS build failed at MSVC main.cpp
with 30+ errors across DonchianEngine/EmaPullbackEngine/
XauTrendFollow4hEngine/IndexFlowEngine/SurvivorPortfolio. None caught
by the OmegaBacktest target because it doesn't include those headers
in its TU. The canary script compiles each header in isolation under
`-fsyntax-only` so the type-resolution path matches MSVC's main.cpp
compile without needing Windows headers.

## Engine Backtesting — MANDATORY READ (added 2026-06-15)

Before backtesting ANY engine, read **`backtest/ENGINE_BACKTEST_REGISTRY.md`**.
It exists because sessions kept re-deriving engine wiring by trial-and-error and
making the same catastrophic mistakes (testing tombstoned engines, missing
`init_default_cells()`/`.init()` → 0 trades that look "dead", ×1000 data glitches,
column-order swaps, pnl-unit double-multiply). The registry documents the faithful
recipe + every known trap.

Two non-negotiable rules:
1. **The live SHADOW LEDGER is the primary record of engine performance** —
   `<log_root>/trades/omega_trade_closes.csv` (+ daily, + `shadow/omega_shadow.csv`;
   VPS `C:\Omega\logs\trades\`). Every enabled engine writes its closed trades there
   with the engine tag. To judge "is this engine viable", READ THIS FIRST — it's the
   real forward record. Don't rebuild a fragile tick-replay when the answer is logged.
2. **Two pre-flight gates on every backtest:** (a) cross-check each engine is
   `enabled=true` in `engine_init.hpp` (the harness runner list is independent and
   will test disabled engines); (b) run `backtest/data_integrity_gate.py` on every
   tick file (catches ×1000 glitches / column swaps / gaps). A REJECTED file is not used.

History: 2026-06-15 — a full-book "backtest" tested 6 tombstoned + a whole graveyard
of disabled index engines, on DJ30 data that was 25% ×1000-corrupted (fake +$2.4M),
and reported 0 trades for SurvivorPortfolio because `init_default_cells()` was never
called. All preventable with the above. The registry + these rules close that gap.

## Branch Freshness (added 2026-05-20)

A 2026-05-20 session worked for hours on `s44-bt-validation`, unaware
that origin/main had moved 201 commits ahead since the branch was
created (audit-fixes-32-42, S50 X3 engine purge, MacroCrash re-enable,
FX cohort wiring, ACTIVE_SYMBOLS_GATE, heartbeat infra). The session
added wiring that conflicted with main and resurrected engines main
had already retired (TSMomGold, PullbackCont). Recovery required
cherry-pick + manual conflict resolution onto current main.

**Run at session start:**

```bash
bash tools/check_branch_freshness.sh
```

It fetches origin/main read-only and reports how far HEAD has drifted.
Default threshold: blocks (exit 1) when HEAD is >= 25 commits behind
origin/main. Override with `STALE_OK=1` or `--force` if intentional.

**Workflow rules:**
- No long-lived feature branches. Feature work goes into short-lived
  branches (≤ 1 day) that merge back to main same session. Long
  branches accumulate drift unobserved.
- Before adding new engine wiring, verify the target engine still
  exists on main (`ls include/<EngineName>.hpp`). If main has retired
  it (search commit log for "purge" or "retire" near that file),
  do not resurrect — build a new engine instead.

## Engine Warm-Seed Mandate (added 2026-05-20)

A 2026-05-20 deploy went 2h18min with zero signals because 15 newly-
added engines had no warm-restart mechanism. Engines with EMA100 /
Donchian40 / z-window=120 buffers cold-start in 14-100 days. Without
historical seeding they sit idle waiting for the bar buffer to fill —
unacceptable on a production deploy.

**Every new D1/H4/H1 engine MUST ship with a warm-seed path before
merge.** Three accepted patterns:

1. **on_h4_bar engines (preferred):** if the engine's signal-evaluation
   path is reachable via `on_h4_bar(h, l, c, bid, ask, ts_ms, cb)` and
   the engine has a public `bool enabled` field, do nothing custom —
   call `omega::seed_h4_engine(eng, csv_path, "EngineName")` from
   `engine_init.hpp` after configuring the engine. The shared template
   in `include/engine_seed_helpers.hpp` replays the CSV while
   `enabled=false` so no entries fire on historical bars.

2. **Tick-driven engines (own H4/D1 aggregator):** add a
   `seed_from_h4_csv(path)` method that directly populates the
   engine's deques (h4_highs_/lows_/closes_ + ATR state) without going
   through the tick → bar aggregator. Pattern: see
   `Ger40TurtleH4Engine::seed_from_h4_csv()`.

3. **Multi-leg engines (e.g. pairs):** add a `seed_from_*_csvs(...)`
   method that reads each leg's CSV, builds a time-sorted stream of
   (ts, leg, close), and replays via the engine's per-leg tick
   methods at H1 boundaries. Pattern: see
   `EurGbpPairsEngine::seed_from_h1_csvs()`.

**Bundled warmup CSVs live in `phase1/signal_discovery/`** and are
committed to the repo so they ship with every deploy:

```
phase1/signal_discovery/warmup_XAUUSD_H4.csv  (3216 bars / 134 days)
phase1/signal_discovery/warmup_GER40_H4.csv   (1600 bars / 11 months)
phase1/signal_discovery/warmup_EURUSD_H1.csv  (6920 bars / 10 months)
phase1/signal_discovery/warmup_GBPUSD_H1.csv  (9000 bars / 17 months, IBKR IDEALPRO; nightly OmegaSeedRefresh since S-2026-07-14)
```

Add a new symbol/timeframe CSV here when adding an engine for it.
Format conventions:
- H4 CSVs:  `bar_start_ms,open,high,low,close` (ts in milliseconds)
- H1 CSVs:  `ts,o,h,l,c`                       (ts in seconds)

**Hash check on first deploy:** boot logs MUST show one `[SEED]` line
per new engine. Missing `[SEED]` line = engine cold-warming = engine
silent for days. Treat absence as a P1 — investigate before next ship.

### Seed registry — structural gate (added S-2026-07-14, operator mandate)

A seed CSV is not done when it exists — it must also STAY fresh. Every
ACTIVE seed (string-literal references AND dynamic paths like
SurvivorPortfolio's `seed_all()`) must be either:
1. refreshed nightly by `tools/seed_refresh.py` (add the recipe to
   `_REBUILD_TARGETS` / `_GOLD_TFS` / `_INDEX` / `_FOREX` / `_ALIASES`), or
2. named in `KNOWN_UNREFRESHED` in `tools/seed_freshness_audit.py` with
   the owner that refreshes it instead.

`tools/seed_freshness_audit.py --registry-only` enforces this (exit 3 on
violation) and runs inside `scripts/mac_canary_engines.sh`, so a new
engine whose seed has no refresh path **fails the pre-commit canary**.
The audit is the SINGLE copy — `seed_refresh.py` phase 3 delegates to it
(the former inline duplicate drifted). Freshness semantics: VPS copy is
load-bearing (stale = deploy abort); Mac git snapshots of nightly-VPS-
refreshed seeds report as `[snapshot-lag]` warnings, re-collated via scp
from omega-new + commit (last full collation S-2026-07-14).
History: warmup_USTEC.F_H4 (2 ACTIVE Survivor cells, dynamic path — audit-
blind) and warmup_GBPUSD_H1 (live FxLadder cell, hidden by the FX skip-
regex) both rotted 90+ days because nothing structural objected; and
data/mgc_h4_hist.csv claimed "regenerated at deploy" while NO regenerator
existed. All three classes are now caught at commit time.

History: this rule originated from the 2026-05-20 incident where 15
new engines (XauTurtleD1, XauDojiRejD1, EurGbpPairsEngine, etc.) all
sat idle on deploy. Fixed by adding seed methods + bundled CSVs in
commit 8f92cbf9. Pattern is mandatory for all future engines.

## WHICH BOX IS LIVE (read before ANY deploy/ssh — added 2026-07-10)

**The live production box is `omega-new` = `45.85.3.79`.** The `omega-vps` alias
(`185.167.119.59`) is the **RETIRED old box** from the 2026-07-07 migration — it
was left powered on and its Omega service kept running in live-mode, but it has NO
broker connection and is NOT your desk. On 2026-07-10 a whole session's deploys
(gold chip, /bigobj, FEEDPATH, JumpRider endpoint) + a vcpkg-recovery saga landed
on 185 while the operator traded on 45.85.3.79 — because CLAUDE.md and the deploy
tooling still said `omega-vps`. Hours wasted; the operator was (rightly) furious.

- **Deploy / ssh / scp to `omega-new`, never `omega-vps`.** `tools/omega_deploy.sh`
  now defaults `HOST=omega-new`; `feeds_selftest.py`/`protection_selftest.py`/the
  monitors were repointed the same day. If you type `ssh omega-vps` you are on the
  DEAD box — stop and switch.
- The old box's Omega service was **Stopped + set to Manual** on 2026-07-10. If you
  find it Running again, that's a regression — it should stay down until the box is
  decommissioned. The migration repoint of remaining refs is draft PR #4.
- Sanity before trusting any VPS check: `ssh omega-new "..git rev-parse --short HEAD"`
  should match `origin/main`. If a check says "GREEN" but the operator's GUI hash
  differs, you may be looking at the wrong box.

## Deploy Hygiene

Added 2026-05-14 after a load-bearing discovery: VPS `git status`
showed **HEAD detached at `18b62c8` (S24, late April)** while the
running binary was `491fd94` (S81, built 5/14 06:49 UTC) and
`origin/main` was at `f7c18f7` (S86, the live-bug hotfix). Three
distinct commits — working tree, running binary, and origin/main —
none of them in agreement. Multiple "stop-bleed" commits over the
prior week (S68 g_vwap_rev_nq.enabled=false, S82 EmaPullback LC=0.10,
others) had been shipped under the assumption that "commit + push +
operator-side rebuild = it's live", but the working tree never moved
off S24, so any rebuild from current source would have downgraded the
running binary by a month. The protection actually-in-production was
unverifiable.

This section codifies the checks that must run before and after any
ship that affects the running binary, so this can't recur.

### At session start (any session that may commit engine code or shipping config)

The operator runs **before any new engine work begins** and pastes
back the output:

```powershell
cd C:\Omega
git rev-parse HEAD                       # MUST equal what's at origin/main
git rev-parse origin/main                # for direct comparison
Get-ChildItem C:\Omega\Omega.exe | Select Name, LastWriteTime
Get-Content C:\Omega\logs\omega_service_stderr.log -Tail 5 | Select-String "Git hash|version="
```

Expected: VPS HEAD == origin/main, and the `Git hash:` line in stderr
also equals that hash. Three values, all matching.

If they don't match: **P0 — investigate before any new ship.** Detached
HEAD, lagging working tree, or running-binary hash divergent from HEAD
all mean shipping new code is operating on assumptions that may not
hold. Reconcile first.

### After every deploy (rebuild + restart)

The operator runs **after the service comes up** and pastes back:

```powershell
Get-Content C:\Omega\logs\omega_service_stderr.log -Tail 10 | Select-String "Git hash|version="
git rev-parse origin/main
```

The `Git hash:` value MUST match the new `origin/main` HEAD. If it
shows the prior hash, the rebuild didn't pick up the new code OR the
service restarted from the OLD binary. Either way the deploy didn't
take and the new fix is NOT in production.

### Build path discipline

The OMEGA.ps1 branch guard (S60: "hard-block when HEAD != main") is
the canonical build path. **Manual builds bypassing the guard are
forbidden.** If a one-off rebuild is needed (e.g. a quick incremental),
the operator must `git checkout main` first, build, then return to
whatever state they were in. The risk of a manual build creating a
binary out-of-band of git tracking is exactly what produced the
2026-05-14 discovery.

### History

The 2026-05-14 incident root cause: on 2026-05-10 a build cycle
produced two failed binaries (`Omega.exe.broken_65d91b4_*` and
`Omega.exe.broken_9bc02f9_*`); the operator reverted the working tree
to S24 via detached checkout (`Omega.exe.working_18b62c8_20260510_*`).
The detached state then persisted across sessions while the binary was
rebuilt from a different state at S81 four days later, leaving the
working tree, the binary, and origin/main in three different commits.
None of the prior sessions ran a hash-alignment check, so the
divergence wasn't caught until the S86 hotfix deploy attempted to
proceed and surfaced it.

## Standing Audit Checks

Run periodically (any session that touches engine code):

```bash
# 1. Ungated-engine audit (S-2026-07-14, latent-class sweep item 10: promoted
#    from the inline 2-idiom grep that lived here to a script -- the old grep
#    matched only `pos[_]?\.active`/`pos[_]?\.open\(sig` and had ZERO coverage
#    of the other opener idioms: MgcFast/MgcSlow `pos_active_ = true`,
#    CrossSectionalIndex `legs_.push_back`, GoldTsmomD1V2 `w_ = want`).
bash scripts/ungated_engine_audit.sh
# The script DERIVES the wide opener regex (ENTRY_RE) from
# scripts/adverse_protection_audit.sh at runtime (derive-don't-copy, the
# persistence_audit.sh pattern; zero-parse = FAIL exit 2) and scans ALL
# include/*.hpp. Every hit must reference OmegaCostGuard/ExecutionCostGuard or
# be explained in scripts/ungated_engine_allowlist.txt (one documented reason
# per entry: false positives, tombstoned/dormant headers, the disabled-engine
# list -- those must gain a gate before any re-enable -- and documented
# exceptions). Notable migrations from the old inline expected-list:
#   * ConnorsRSI2 was listed DISABLED here but was re-enabled gated
#     S-2026-07-08c (certified cost-inclusive backtests + SMA200 gate; runtime
#     cost-gate backfill owed) -- now a documented exception in the allowlist.
#   * StockDipTurtle no longer needs a note: its INJECTED gate_fn leaves a
#     literal ExecutionCostGuard reference in the header, which the script sees.
#   * SurvivorPortfolio / FxUpJumpLadderCompanion / GoldTrendMimicLadder /
#     dormant DonchianEngine+EmaPullbackEngine+TrendRiderEngine are NEW
#     EXPOSURES of the wide regex, documented in the allowlist.
# Wired into scripts/mac_canary_engines.sh, so it runs on every pre-commit
# canary. Any NEW unexplained hit = exit 1 = P1 regression. History: 2026-06-10
# audit found 21 enabled shadow engines ungated (cost-blind shadow ledgers);
# all 21 gated with ExecutionCostGuard::is_viable same session; the inline
# 2-idiom grep lived here until S-2026-07-14.

# 2. GoldEngineStack chokepoint audit
grep -nE "\.open\(" include/GoldEngineStack.hpp
# Expect exactly ONE actual call site (the gated pos_mgr_.open()); all other
# hits must be comments (as of 2026-07-14: 3 grep hits = L50 include comment
# + L4229 comment + the single gated call at ~L4248). A second CODE hit
# needs investigation.
```

## Cost-Gate Semantics (often confused)

`OmegaCostGuard::is_viable()` is an **entry filter only**. It blocks
trades that cannot possibly cover costs at TP at fire time. It does
NOT cut trades that go negative in-flight. In-flight protection must
be implemented in the engine's manage block.

The canonical in-flight pattern is the `VWAPReversionEngine`
LOSS_CUT + BE_RATCHET fields (added in the part-H follow-up,
2026-05-13):

- `LOSS_CUT_PCT` — cold-loss cut for trades that go straight adverse
- `BE_ARM_PCT` — arms a break-even ratchet once mfe reaches threshold
- `BE_BUFFER_PCT` — break-even buffer for ratchet trigger

Engines that share this profile (mean-reversion + fixed timeout) are
candidates for the same pattern.

## BE-Floor-On-Open Foundation Mandate (added 2026-07-17c, operator hard rule)

Every NEW companion / mimic / ladder / clip engine MUST be built
**floored-on-open** from day one — it is the FOUNDATION architecture, not
an add-on. Operator mandate (2026-07-17c): the floored-on-open pattern
IMPROVED net on nearly every cell across all 5 engines swept AND makes the
hard no-pre-BE-loss rule (`feedback-no-prebe-loss-ever`) true by
construction (nNeg=0), so it becomes the default for every subsequent
companion engine's logic.

The canonical recipe (proven S-2026-07-17c, Omega fd51311a + crypto c771068):
- **BE-ENTRY** — leg stays FLAT, books/pays nothing until fav >= confirm; opens AT that level.
- **confirm >= 2× round-trip cost** (NOT just >=RT — at exactly RT the 2×-cost
  robustness stress reopens a pre-arm window: crypto TRX −511bp. 60bp is the safe uniform value).
- **ANCHOR** `le = epx` on open (`confirm_anchor_epx` in UpJumpLadderCompanion/BeCascade;
  `be_floor_on_open` book-clamp in FxUpJumpLadderCompanion) — do NOT reset le to the
  confirm/current price ⇒ hwm=cur>=le*(1+RT) at open ⇒ floored at BE ⇒ worst clip net>=0.
- **RECLIP** must be `reclip_pct=0` OR anchored-reclip (route re-entry through the same
  confirm+anchor path, keep le=reclip_px). A raw `le=cur` reclip reopens a pre-arm window
  and leaks real pre-BE losses (ADA −527bp, TRX −1773bp).
- **fp-clamp** the g>=1.0 floored stop to >=BE (IEEE-754 yields ~−1e-7bp).

**Enforced as a build gate:** `scripts/prebe_loss_audit.sh` (+ `scripts/prebe_loss_allowlist.txt`
+ `scripts/PREBE_LOSS_GATE.md`), wired into `scripts/mac_canary_engines.sh` — any pre-BE booking
site without a floor marker or allowlist entry fails the canary. Crypto side: the boot
`[MIMIC-FLOOR-GATE] N/N floored 0 VIOLATION` line must show 0 violations. Grandfathered
backfill-owed (2026-07-17c): GoldTrendMimicLadder (compliance fix landed, edge not certified on
all cells), StockDayMoverLadderCompanion. See vault `BeFloorOnOpenFoundation.md` (both vaults) +
memory `feedback-befloor-on-open-foundation`.

## Engine Adverse-Protection Mandate (added 2026-06-19)

Every NEW position-opening engine MUST ship with a **backtested in-flight
adverse-protection verdict** before merge. This was previously only a
memory note (`feedback-new-engine-adverse-protection-step`) — advisory text
a session could skip. It was skipped: `NqMomentumEngine` shipped (shadow,
2026-06-18) with NO cold-loss cut and a %-scaled BE ratchet that never armed
on an index, leaving a faithful-BT worst trade of **−464pt** with the only
"protection" a 3×ATR trail. A written rule with no gate fails the first time
a session ignores it.

It is now a **build gate**, not a rule:

- `scripts/adverse_protection_audit.sh` scans every `include/*Engine.hpp`
  that opens a position (`pos_.active = true` / `.open(`). Each must carry an
  `ADVERSE-PROTECTION:` header comment stating the **backtested verdict**.
- The verdict is NOT required to be a loss-cut. Per the 2026-06-17
  swing-protection sweep, tightening hurts most trend/trail engines — so a
  verdict of *"trail-only — backtested, a cold cut lowers net (ref)"* is
  fully valid. What is forbidden is **skipping the backtest step**.
- Engines present when the gate was introduced are grandfathered via
  `scripts/adverse_protection_legacy.txt` (printed as backfill-owed warnings,
  do not fail). Any engine NOT in that list and NOT annotated is a NEW engine
  and **fails the build**.
- The audit runs at the end of `scripts/mac_canary_engines.sh` — the
  mandated pre-commit canary — so a non-compliant new engine cannot be
  committed.

Backfill debt: effectively closed — as of 2026-07-14 the canary reports
87 engines annotated, 3 legacy backfill-owed (DonchianEngine,
EmaPullbackEngine, TrendRiderEngine), 0 violations (the original
2026-06-19 figure was 81 owed; burned down since). Finish the last 3
opportunistically when touching each engine.

## Session Handoffs

Each session ends with a handoff document at
`outputs/SESSION_HANDOFF_YYYY-MM-DD<letter>.md`. Read the most recent
before starting work to inherit context (pending decisions, in-flight
work, known issues).

## Stale `.git/index.lock`

If `git commit` reports `fatal: Unable to create '.git/index.lock'`,
inspect the file size. If it is 0 bytes and the timestamp predates the
current session, it is a leftover from a previously-aborted operation
and is safe to remove. Otherwise, defer to the operator.
