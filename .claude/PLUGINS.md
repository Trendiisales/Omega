# Claude Code Plugin Setup (Omega / Crypto)

Project-scoped plugin config: .claude/settings.json (committed; every Claude Code
session that opens Omega picks it up). Requires Claude Code v2.1+.

## Installed - 10 plugins, 2 marketplaces

From claude-plugins-official (auto-available):
- frontend-design      - designer-grade UI (omega-terminal, CryptoPanel.tsx)
- context7             - live, version-correct library docs (ib_insync, pandas, react, recharts)
- code-review          - multi-agent diff review
- commit-commands      - commit -> push -> PR helpers
- playwright           - drive a real browser to test the terminal/dashboard UIs
- claude-md-management - audit/maintain CLAUDE.md
- pyright-lsp          - Python type/LSP intel (bracket-bot, backtests, luke_crypto)
- code-simplifier      - readability cleanups
- claude-code-setup    - analyze repo + recommend automations

From obra/superpowers-marketplace (declared in extraKnownMarketplaces; trust on first use):
- superpowers          - skills library (TDD, debugging). Installs from
                         github.com/obra/superpowers.git (external clone, normal network only).

## Load / verify
1. In a Claude Code session in this repo: /reload-plugins (or restart).
2. /plugin - all 10 listed + enabled. Approve the superpowers-marketplace trust prompt once.
3. Confirm .claude/settings.json parses.

## Interaction with existing saved commands / skills
- The harness already ships /code-review and /simplify. The code-review and
  code-simplifier PLUGINS overlap them - prefer the existing project skills for
  repo-aware work; treat plugins as general-purpose alternates.
- commit-commands one-shot commit->push->PR BYPASSES the project gates
  (Mac-canary-green, scripts/adverse_protection_audit.sh, S<N> messages,
  split-unrelated-changes). Do NOT use it to ship engine code.

## Guardrails (tie-in with CLAUDE.md core rules)
- code-simplifier / frontend-design: keep OFF C++ core (OmegaCostGuard,
  OmegaTradeLedger, SymbolConfig, OmegaFIX, engine_init.hpp). Core is not edited
  without explicit instruction.
- claude-md-management: do NOT let it rewrite CLAUDE.md wholesale (load-bearing
  operator history). Audit/read-only only.
- pyright-lsp: expect many diagnostics on untyped quant Python; advisory only.

## Environment note
Claude Code on the web (sandbox) cannot live-install these (official marketplace name
is reserved to anthropics/ clones; GitHub clones route through a trendiisales/omega-scoped
proxy). Installs happen on a normal-network machine from this committed config.

Status: settings.json + MEMORY.md + PLUGINS.md live on main; all 10 identifiers validated
against the live marketplace manifests on 2026-06-29.
