# Omega Repo — Operating Rules for AI Sessions

These rules apply to ANY AI session (Claude Cowork, Claude Code, etc.)
working in this repository. They supplement the operator's global
user-preferences (full-file edits, 70% chat warning, never modify
core code without instruction) with project-specific safeguards.

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
cd ~/omega_repo
cmake --build build --target OmegaBacktest -j
```

**Do NOT** use the bare `cmake --build build -j` — that target needs
Windows-only headers and always fails on macOS, even though the
surrounding green "Built target X" lines can make it look like a pass.

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
# 1. Ungated-engine audit
for f in include/*.hpp; do
  if grep -lE "pos[_]?\.active *= *true|pos[_]?\.open\(sig" "$f" >/dev/null 2>&1; then
    if ! grep -q "OmegaCostGuard" "$f"; then echo "UNGATED: $f"; fi
  fi
done
# Expect ONLY: LatencyEdgeEngines, RSIExtremeTurnEngine,
# SweepableEngines, SweepableEnginesCRTP.

# 2. GoldEngineStack chokepoint audit
grep -nE "\.open\(" include/GoldEngineStack.hpp
# Expect exactly two hits: L50 include comment + the gated
# pos_mgr_.open() call site. Any third hit needs investigation.
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
