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
