# PREFLIGHT — read me before touching this repo

Every Claude session (or any agent, or any human) that is about to analyse
or modify this repository **must run `.claude-preflight.sh` first** and get
exit code 0 before issuing a single subsequent command. This is not a soft
suggestion; it is the mechanical guard against a class of failure that has
already burned us once.

---

## The 2026-05-12 incident this rule exists to prevent

On 2026-05-12 an agent was asked to investigate bad USTEC trades in the
shadow tape. It worked the entire session against a stale `/tmp/omega_repo`
clone left over from a previous session. That clone was at commit
`68e7720` (`S33c: persist broker_* reconciliation fields to trade-close
CSV`). `origin/main` was already 7 commits past that point, at `eaef9a1`
(`S33 FINAL: handoff doc — engines deployed, observing`).

The seven commits the stale tree was missing happened to include the
**entire creation and tuning** of the engine that was emitting the bad
trades: `54e9ad2 S33d: disable GoldMicroScalper, ship XAU 4h + USTEC 5m
trend-follow`, `b36f382 S33f: USTEC 5m -> 2 cells (add Keltner)`,
`7727464 S33g`, `1511a00 S33k`, and so on.

Concrete consequences:

1. The agent grepped the stale tree for `UstecTrendFollow5m`, found
   nothing, and confidently told the operator the engine did not exist
   anywhere in the repo or any of its history.
2. The agent wrote a `logging.hpp` fix against the wrong tree and almost
   shipped it.
3. The agent then said "this trade row must be from outside the system,"
   which was wrong.
4. The operator had to run a `git log -1 --pretty=%H` on the VPS to
   surface the commit hash before the gap was discovered, and the entire
   thread up to that point was, in hindsight, garbage.

The agent's first command on this repo, on every session, must be the
preflight. If it had been, the gap would have been caught in under a
second and the rest of the conversation would have been about the real
engine bugs, not phantoms.

---

## What the preflight checks

`.claude-preflight.sh` returns non-zero unless all of the following hold:

| # | Check                                          | Failure exit code |
|---|------------------------------------------------|-------------------|
| 1 | Current directory is inside a git work tree    | 6                 |
| 2 | `git fetch origin --quiet` succeeds            | 4                 |
| 3 | Local `HEAD` SHA == `origin/main` SHA          | 2                 |
| 4 | Working tree is clean (no uncommitted changes) | 3                 |
| 5 | `.git/FETCH_HEAD` mtime is < 60 seconds old    | 5                 |

Optional `--strict` flag adds:

| # | Check                                          | Failure exit code |
|---|------------------------------------------------|-------------------|
| 6 | Not in detached HEAD                           | 7                 |

The 60-second freshness window for `FETCH_HEAD` exists so that a session
that "passed preflight an hour ago and is still going" cannot silently
drift; if you've been working for more than 60s without a re-fetch and you
want to make a remote-touching change, rerun the preflight.

---

## The contract

The contract that any Claude session must follow when this repo is in
scope:

1. **First action of the session: `bash .claude-preflight.sh`.** Not
   "grep first, preflight when remembered." Not "I'll just look at one
   file." Not "the clone is fine, I cloned it five minutes ago." The
   first action.

2. **If preflight returns non-zero, refuse to proceed.** Surface the
   exact exit code and the script's stderr to the operator verbatim. Do
   not attempt to "work around" by re-cloning into another path or by
   asking the operator to confirm staleness is fine; the operator's
   directive (multiple times) is "ensure this can never happen," and
   working around the guard is the failure mode it exists to prevent.

3. **Re-run preflight after any operation that could mutate the tree
   or its relationship to origin** — that includes `git pull`, branch
   switches, file writes to the working tree, or any out-of-band sync.

4. **Never use a clone path that is not the one the preflight passed
   in.** If you ran preflight against `/Users/jo/omega_repo`, every
   subsequent grep / Read / Edit must be in that same path. Using a
   sibling clone, a `/tmp` cache, or a remote mirror voids the preflight.

5. **The preflight is the floor, not the ceiling.** It does not check
   whether the operator's stated branch matches `main`, whether the VPS
   binary commit matches `origin/main`, whether secrets are in scope,
   or any other higher-order property. Use the operator's intent to
   decide what *else* needs to be true; the preflight only guarantees
   that the version of the code you're reading is the version `origin`
   currently considers canonical.

---

## Operator-side reciprocal

The preflight is most effective when the operator does two parallel
things on their side:

1. **Always paste the VPS binary's commit hash** when the conversation
   moves into a deploy-impacting topic. The single command
   `git log -1 --pretty=%H` on the box that produced the trade rows is
   the second mechanical check; the preflight covers the agent's tree,
   this covers the binary's tree. They must match for a code-level
   reasoning to be valid.

2. **Push before you ask.** Any local commit you want the agent to
   reason about must be on `origin/main` before the session starts. The
   preflight will refuse to grant exit-code-0 against a local-only
   commit, by design — because the agent has no path to read it.

---

## How this fits into the broader handoff system

This document does not replace `HANDOFF_S*.md`. Those describe **what
state the system is in**; this describes **how to safely look at any
state at all**. The order is:

1. Run preflight.
2. Read the newest `HANDOFF_S*.md`.
3. Then start work.

If step 1 fails, steps 2 and 3 are forbidden.
