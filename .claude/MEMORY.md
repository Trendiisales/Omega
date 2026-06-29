# Omega / Crypto - Memory Index (single source of truth)

One-spot map of all project memory. Any Claude Code session - and the
claude-md-management plugin - should treat this as the table of contents for
"what do we already know". Keep memory reachable from this list; nowhere else.

## Tier 0 - Operating rules (read first)
- CLAUDE.md - AI session rules, edit discipline, build/deploy hygiene, audits.
- OMEGA.md  - Master system reference: data inventory, backtest recipe, costs, deploy, engine status.

## Tier 1 - Engine / backtest system of record
- backtest/ENGINE_BACKTEST_REGISTRY.md - faithful recipe + every known trap (MANDATORY pre-backtest).
- ENGINE_REGISTRY.md - engine inventory.   CHOSEN.md - active strategy set.   KNOWN_BUGS.md - open defects.
- Live SHADOW LEDGER (primary perf record): <log_root>/trades/omega_trade_closes.csv
  (+ daily, + shadow/omega_shadow.csv; VPS C:\Omega\logs\trades\).

## Tier 2 - Session continuity
- outputs/SESSION_HANDOFF_YYYY-MM-DD<letter>.md - newest = current handoff (read before work).
- Root HANDOFF_*.md / SESSION_HANDOFF_*.md - historical (112 files; archive).
- NEXT_SESSION*.md - staged next-session pointers.

## Tier 3 - External vault (system of record, NOT in this repo)
- Memory-Omega vault: /Users/jo/Memory-Omega (operator Mac).
  wiki/entities/<Engine>.md, index.md, log.md. Updated on EVERY deploy (see CLAUDE.md).
  Lives outside git - see consolidation note to bring it in physically.

## Tier 4 - Claude Code plugin setup
- .claude/settings.json - enabled plugins + marketplaces (10 plugins, 2 marketplaces).
- .claude/PLUGINS.md - what each plugin does + usage guardrails.

## How the memory plugins use this
- claude-md-improver (skill) - audits CLAUDE.md vs codebase; approve before it writes.
- /revise-claude-md (command) - appends session learnings. Route new learnings into the
  relevant Tier doc, not scattered new files, so this index stays the one spot.

_Last indexed: 2026-06-29._
