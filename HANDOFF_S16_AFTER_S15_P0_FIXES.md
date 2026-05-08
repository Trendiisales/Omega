# HANDOFF — S16 (continues S15 audit + P0 fixes)

Prepared: 2026-05-08, end of S15
Branch / HEAD at handoff: `main` @ `313b0aa06688169da84b77125bce06babfb64a2c` **plus uncommitted working-tree changes** from S15 P0 fixes (see "Files modified, uncommitted" below). Confirm with `git status` before doing anything.
Mode: `mode=SHADOW` (no live orders firing).
Standing user preference: never modify core code unless instructed clearly. The S15 work was clearly authorised; S16 should be too before code changes.

---

## TL;DR for the fresh Claude reading this

S15 produced (a) a full engine audit (`AUDIT_S15_ENGINES.md`) restricted to the handoff's catalog of ~30 engines, and (b) implementations of the 5 P0 findings the user authorised. The 5 P0s are in the working tree but are **uncommitted and unbuilt** — the build cycle happens on the user's VPS, not in this sandbox. Your job is to (1) verify the P0s built and the system runs, (2) address the P1 backlog (12 items), then (3) the P2 backlog (~25 items), and (4) trace any `[LEDGER-CORRUPT-TS]` log lines that the new sanitizer emits.

**The fastest way to come up to speed is to read these three files in this order:**

1. [`AUDIT_S15_ENGINES.md`](computer:///Users/jo/omega_repo/AUDIT_S15_ENGINES.md) — the full per-engine audit (preamble explains catalog/schema drift; appendices have tape stats).
2. [`AUDIT_S15_P0_FINDINGS.md`](computer:///Users/jo/omega_repo/AUDIT_S15_P0_FINDINGS.md) — the same-day triage one-pager.
3. This file — what's done, what's left, and how to get unstuck.

Don't re-read the engine source files for the P0 work — it's already done. Read sources only for the P1/P2 items you actually pick up.

---

## STEP 1 — Get repo access

This sandbox does NOT have a folder mounted by default. Your first action must be:

```
mcp__cowork__request_cowork_directory(path="~/omega_repo")
```

After approval, the repo is mounted at `/Users/jo/omega_repo/` for the host-side file tools (Read / Write / Edit / Glob / Grep). Use that exact path with those tools. The bash sandbox path differs (`/sessions/<id>/mnt/omega_repo`) but bash is currently broken — see Step 2.

If that path doesn't auto-approve, the user can re-approve manually. Do not proceed past Step 1 without confirmed mount.

---

## STEP 2 — Sandbox status (as of 2026-05-08)

The Linux workspace VM that hosts `mcp__workspace__bash` is **broken** with `useradd: No space left on device`. This was also true during S13. Symptoms when bash is broken:

```
bash failed on resume, create, and re-resume.
useradd: /etc/passwd.NNNNNN: No space left on device
```

Implications:

- You cannot `git status`, `git log`, `git diff` from this sandbox. Use `Read` on `.git/HEAD`, `.git/refs/heads/main`, and `.git/logs/HEAD` instead.
- You cannot run `awk` / `python` over the shadow CSV directly. Use Read with offset/limit (chunks of ~500 rows) and accumulate in your own state, or delegate via an Agent.
- You cannot compile. Every change you make is build-verified only when the user runs `.\OMEGA.ps1 deploy` on the VPS. Make conservative, syntactically-tight edits.
- Skills that rely on bash (xlsx, pdf, docx skill scripts) may fail. Stick to direct file IO.

If bash starts working mid-session, great — but don't depend on it. The S15 audit and all 5 P0 fixes were completed without bash by using Read/Glob/Grep/Edit/Write directly.

If you absolutely need to run code (e.g. CSV aggregation across the full file), spawn a `general-purpose` Agent and ask it to do the chunked-Read + in-memory accumulation pattern. S15 used that pattern successfully — see the example in the audit's Appendix A.

---

## STEP 3 — Verify what S15 left in the tree

The 5 P0 fixes are **in the working tree, uncommitted**. Do not assume they're already on `main`. Confirm with `Read` on each file:

| P0 | File | Anchor to look for |
|---|---|---|
| P0-3 | `omega_config.ini` | `[trend_rider]` block: `max_lot_cap=0.20` and the audit-fix comment block above it (search for `S15 P0-3`). |
| P0-2 | `include/CrossAssetEngines.hpp` | New field `MAX_TRADE_LOSS_USD = 80.0;` near `DAILY_LOSS_CAP` (around L1495-1502). Updated cap gate (around L2000-2018) referencing `effective_cap`. |
| P0-4 | `include/engine_config.hpp` | `[minimal_h4_us30]` parser block (around L561-585) now handles 13 keys including `enabled`, `shadow_mode`, `donchian_bars`, `dollars_per_point`, `atr_period`. The S47-era 4-line stub is gone. |
| P0-5 | `include/engine_init.hpp` | Around L1561-1568, before the four `g_orb_*.enabled = false` lines: per-instance `OPEN_HOUR` / `OPEN_MIN` setters with comment mentioning `S15 P0-5`. |
| P0-1 | `include/trade_lifecycle.hpp` | New block right after the L2-liveness stamp (around L42-70) — sanitizer that detects `tr.exitTs < tr.entryTs`, fprintf to stderr `[LEDGER-CORRUPT-TS]`, coerces, tags `[CORRUPT_TS]`. Search for `S15 P0-1`. |

If any anchor is missing, the user reverted that fix or it never landed; ask before re-applying.

If the user has already deployed and the build failed, the most likely failure modes (because no compile-verify was possible in S15) are:

- `include/CrossAssetEngines.hpp` — `std::max` is used in the new gate; the file already includes `<algorithm>` everywhere it's needed, but if a compile error there mentions `std::max` not found, that's the cause.
- `include/trade_lifecycle.hpp` — new block uses `std::fprintf` (header `<cstdio>`) and `std::string` concatenation. Both are already used elsewhere in the file. If failure references either, look for missing include or a slightly different spelling.

Everything else is INI / parser / field-setter changes that are syntactically minimal.

---

## STEP 4 — What S15 deliberately did NOT do

Don't redo any of these — they're handled or out of scope:

- The audit catalog drift discovery (handoff vs. reality). Documented in the preamble of `AUDIT_S15_ENGINES.md`. Don't re-investigate; the user accepted the drift and chose "stick to handoff catalog" for scope.
- Engines wired but not in the handoff catalog (HTFSwingEngines, LatencyEdgeEngines, FX session-open engines, GoldMidScalper, XauusdFvg, OHLCBar, CellEngine, Heartbeat, etc.). Listed in Appendix C of the audit. The user explicitly deferred these.
- Live-vs-backtest WR comparison for Tier-1 portfolios. The supplied shadow CSV (2025-01-24 → 2026-04-08) pre-dates Tier-1 ship (2026-04-29+). **Until the user supplies a fresh CSV that includes post-2026-04-30 data, nothing useful can be said about Tier-1 live performance.** Ask for it before doing any Tier-1 statistics.
- Backtest validation. The user picked Option A in S15 ("no backtests in this audit"). If a P1 or P2 finding genuinely needs a backtest, defer it.
- Anything in `backtest/audit_results/S17_AUDIT.md`. That's a parallel audit pass somebody else did. Reconciling S15 + S17 catalogs is itself a deferred item. Don't merge them this session.
- Auto-commit / auto-push. The user reviews and commits trading code by hand. Their CLAUDE.md PAT is for read; respect that boundary regardless.

---

## STEP 5 — Audit corrections discovered during S15 implementation

Two findings in the audit doc were narrower in reality than the audit suggested. These are mentioned in the AUDIT_S15_ENGINES.md but left in their original framing for paper trail. If the user asks for the audit to be updated, edit those two sections:

1. **`[minimal_h4]` is NOT dead config.** It IS already wired at `engine_config.hpp:542-557`. Only `[minimal_h4_us30]` was actually broken. The P0-4 fix only needed to extend the US30 parser. Audit doc preamble around the MinimalH4Breakout section overstates the impact.
2. **`OpeningRangeEngine` daily reset already exists** at `CrossAssetEngines.hpp:1003-1009` (resets on `ti.tm_yday != last_day_`). The actual bug was just missing per-instance `OPEN_HOUR`/`OPEN_MIN` in `engine_init.hpp`. The audit's claim that `opening_` "never reset" is wrong.

Don't get tripped up by these in the P1/P2 work — when in doubt, read the source first.

---

## STEP 6 — P1 backlog (priority order, target this session)

Pull these from `AUDIT_S15_ENGINES.md` (and verify each against current source first — some may have evolved since the audit). Effort estimates are 1h-1session each unless noted.

**P1-1.** Trace the `[LEDGER-CORRUPT-TS]` log line origins. The S15 sanitizer at `trade_lifecycle.hpp` will surface any current source of the unit-mismatch bug at runtime. Once you have at least one log line from a fresh shadow tape, grep the engine emitting it (the first field after `[LEDGER-CORRUPT-TS]`) and find which `tr.exitTs = ...` assignment fed the bad value. Patch upstream the same way `MacroCrash` was patched at `on_tick.hpp:929-932`. If no `[LEDGER-CORRUPT-TS]` lines appear after a week of fresh tape, the bug is historical-only and this can close as resolved.

**P1-2.** `VWAPReversion` exits exclusively on `FORCE_CLOSE` in tape (10 of 10 trades, mean hold 10 191 s — one of which is 28 035 s ≈ 7.8h). Engine has TP/SL/BE in code, but never exits on them. Either the engine's own close path is dead, or an external supervisor is closing first. Trace from `CrossAssetEngines.hpp:1072-1325` outward; check who calls `VWAPReversionEngine::on_tick` and whether the manage path is reached.

**P1-3.** `FxCascade` per-pair cooldowns reset on entry, not on close. Multi-leg overlapping cycles can leave correlated exposure unaccounted for. Refactor `CrossAssetEngines.hpp:811` so cooldown starts at `_close()`, not at entry. Test plan: walk through a synthetic 3-leg cascade and verify only one leg can be open at a time per the design intent.

**P1-4.** `IndexFlow` shadow WR is 29.4% on 17 trades (under the handoff's 35% red-flag threshold). Replay the 17 trades against the post-S13 L2 wiring (`g_macro_ctx.sp_l2_imbalance`) to confirm signal quality vs. the cTrader-era data the engine was originally tuned on. If the live tape continues to show <35% WR over the next 30 trades, propose a reskin or cull.

**P1-5.** `TrendPullback` consecutive-SL direction block uses hardcoded threshold (2 losses) and pause (600s). Add INI knobs `consec_sl_threshold` and `direction_block_duration_sec` to the `[cross_asset]` section, parse in `engine_config.hpp`, expose setters on the engine. Default to current values for backward-compat.

**P1-6.** `TrendPullback` index instances (GER40/NQ/SP) never have `m5_trend_state_` or `avg_atr20_` initialised by any caller — gates default to permissive. Either wire callers to update them, or document the indices as "M5 trend gate disabled" so operators know.

**P1-7.** `BracketEngine` `CONFIRM_SECS` vs. `PENDING_TIMEOUT_SEC` overlap can produce premature `BREAKOUT_FAIL_CONFIRM` cuts when broker fills are slow. NAS100 was special-cased to 45s (vs. 30s default). Formalise: set `CONFIRM_SECS = max(P50(recent_fill_latency_ms / 1000) + 10, current_default)` per symbol. Recompute weekly from the tape's `latency_ms` column.

**P1-8.** `EsNqDivergence` `confirm_count_` doesn't reset when the confirmed direction flips. Edge case but produces a false latch. One-line reset in `CrossAssetEngines.hpp:433-434` on direction change.

**P1-9.** `IndexSwingEngine` TP% asymmetry (SP 0.5%, NQ 0.2%) is undocumented. Either (a) add a code comment with the empirical rationale, or (b) unify to a tested midpoint (~0.35%). Currently a comprehension trap.

**P1-10.** `SweepProEngine` "sweep" naming is misleading — implementation is "momentum exhaustion near liquidity cluster" not "structural wick rejection". 5-line comment fix at the class header explaining what the engine actually does. No code change required.

**P1-11.** `MinimalH4Breakout` (gold) cold-start needs CSV warm-load path. The US30 sister has one (`engine_init.hpp:788-805`); add the same for gold so first-deploy doesn't burn 40 hours of wall clock waiting for 10 H4 bars to arrive.

**P1-12.** `C1RetunedPortfolio` cluster-day boundary uses UTC midnight but cells close on different timeframes (H1/H2/H4/H6) and may all close losers across an 18 h window spanning two UTC days. Migrate from UTC-day to session boundary (Tokyo / London / NY). Has knock-on effects in `C1RetunedPortfolio.hpp:628-645`. Validate with the cluster-day events in shadow tape (count days where all 4 cells closed losers).

---

## STEP 7 — P2 backlog (cleanup-grade, lower priority)

Skim from `AUDIT_S15_ENGINES.md`. The big buckets:

- Warmup CSV silent-failure on Tsmom / Donchian / EmaPullback / TrendRider / C1Retuned (5 P2s; same fix shape — log to stderr at WARN, plus optional GUI status panel signal). Could be wrapped into a shared `load_warmup_csv_or_warn()` helper.
- Per-cell `[engine_h*]` INI sub-sections being documentation-only (Tsmom / Donchian / EmaPullback / TrendRider). Wire the `enabled=` keys into a hot-reload path so the sections stop being a lie. Operator-trap.
- `MacroCrashEngine::force_close` runtime assertion `assert(now_ms >= 1e12 && now_ms < 4e12)` to catch future regressions where seconds slip back in. Belt-and-suspenders for the existing fix.
- `MacroRegimeDetector` add a startup log line listing all four MacroCrash safeguards (engine-SL, DOLLAR_STOP, max_positions, max_consec_losses) with current values.
- `OilEngine` EIA window hard-coded; move to INI keys.
- `BreakoutEngine` spread gate evaluated post-compression — move to FLAT → COMPRESSION transition.
- `PDHLReversionEngine` deprecate the unused `depth_bid` / `depth_ask` parameters from `on_tick` signature; replace with a single `l2_imbalance` arg; drop the `0, 0` literal at `tick_gold.hpp:2266`.
- `OpeningRangeEngine` move `OPEN_HOUR` / `OPEN_MIN` into INI keys (P0-5 fixed the wrong-defaults bug; this P2 makes future overrides INI-tunable).
- `IntradaySeasonality` move the inline t-stat table to an external CSV with a WARN on missing.

Pick whichever matches the tape's behaviour after the P0/P1 work lands. Don't try to do all of them — most are months away from being worth the effort.

---

## STEP 8 — Files modified, uncommitted (S15 P0 work)

Working tree contains changes to these files. The git index does NOT reflect them (no `git add` was performed):

```
M  omega_config.ini                          (P0-3: TrendRider lot cap)
M  include/CrossAssetEngines.hpp             (P0-2: TrendPullback DAILY_LOSS_CAP)
M  include/engine_config.hpp                 (P0-4: minimal_h4_us30 INI parser)
M  include/engine_init.hpp                   (P0-5: ORB per-instance UTC times)
M  include/trade_lifecycle.hpp               (P0-1: ledger ingress sanitizer)

A  AUDIT_S15_ENGINES.md                      (audit deliverable)
A  AUDIT_S15_P0_FINDINGS.md                  (P0 triage)
A  HANDOFF_S16_AFTER_S15_P0_FIXES.md         (this file)
```

The audit docs and this handoff are at the repo root. They are intentionally not under any source path so they don't get caught by the build / package step.

---

## STEP 9 — Repo and tape recon shortcuts

For when you need facts fast and bash is dead:

- Git HEAD: `Read /Users/jo/omega_repo/.git/HEAD` (gives `ref: refs/heads/main`), then `Read /Users/jo/omega_repo/.git/refs/heads/main` (gives the SHA).
- Recent commits: `Read /Users/jo/omega_repo/.git/logs/HEAD` with offset (file is large; tail with offset 200+).
- Engine catalog: `Glob /Users/jo/omega_repo/include/*Engine*.hpp` and `*Portfolio*.hpp`.
- Active vs. dormant engines: `Grep "enabled\s*=\s*(true|false)" include/engine_init.hpp`.
- Shadow tape stats per engine: delegate to a `general-purpose` Agent with the chunked-Read instruction. The S15 stats agent's prompt is in this conversation if the user can find it; otherwise model it on the S15 audit's Appendix A.

The shadow CSV columns (NOT what the original handoff predicted): `ts_unix, symbol, side, engine, entry_px, exit_px, pnl, mfe, mae, hold_sec, exit_reason, spread_at_entry, latency_ms, regime`. The handoff's awk recipes mostly still work but column positions shift — `pnl` is column 7, not column 10.

The `omega_shadow_signals.csv` file the original handoff promised does not exist — only `omega_shadow.csv` and `omega_shadow_export.zip` are in `audit_input/`. If gate-firing checks (RISK_OFF / max_concurrent / cooldown) need the signals tape, ask the user to export it from the VPS.

---

## STEP 10 — Verification checklist before claiming P1 work is done

Mirror what S15 did:

1. Read each file you modified end-to-end after the edit (don't trust your own diff).
2. Confirm anchor strings around your changes are unique and intact.
3. For any new field, grep for callers and confirm they compile against the new shape.
4. For any new include, confirm it's already present (this is a header-only project — every .hpp is included once via `omega_pch.hpp` or directly from `main.cpp`).
5. For any change that touches a code path that produces TradeRecord, validate hand-eyes against the file's existing TradeRecord assignments (entryTs, exitTs, engine label, side).
6. Per the standing user pref, leave changes uncommitted. The user reviews and commits.

---

## STEP 11 — Explicit "do not" list

- Do NOT auto-commit. Do NOT auto-push.
- Do NOT add documentation (markdown, README) unless the user asks.
- Do NOT modify the audit docs that S15 wrote without asking.
- Do NOT use the user's GitHub PAT (it's in the global CLAUDE.md for read context only).
- Do NOT propose backtests as P1 fixes — those are deferred.
- Do NOT touch anything in `omega-terminal/node_modules/` or `build/`.
- Do NOT widen the audit scope to non-handoff-catalog engines without asking. The user explicitly chose "stick to handoff catalog" in S15.
- Do NOT spend context re-reading `AUDIT_S15_ENGINES.md` end-to-end. Use Grep to find the section you need.

---

## STEP 12 — One-line summary for the next user-message

After repo access is mounted and you've Read the three primary files (this handoff + the two audit docs), tell the user:

> Repo mounted. S15 left 5 P0s in the tree (uncommitted). Bash sandbox is broken (no-space-on-device). Ready to start S16 — confirm scope: P1 backlog only, P1 + P2, or trace `[LEDGER-CORRUPT-TS]` lines first?

Then wait for the user to choose. Don't dive in.

---

Good luck.
