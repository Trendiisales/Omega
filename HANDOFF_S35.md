# HANDOFF S35 — 2026-05-12

End-of-day state after a session that (a) discovered the working tree
was carrying months of uncommitted drift that blocked
`.claude-preflight.sh`, (b) triaged 9 pre-existing tracked-but-dirty
files into two stashes preserved for separate operator review, (c)
shipped a comprehensive `.gitignore` and committed 43 previously-
untracked source files (10 handoff docs + 22 research scripts + 3 FX
research engines + 1 unit test + the new `.gitignore`), (d) flagged a
critical secret-in-tree finding (`.github_token`) that requires PAT
rotation, and (e) applied the S34 P1 close-path bug-class fixes to the
three remaining XAU trend-follow engines (`XauTrendFollow{4h,2h,D1}`)
matching the pattern from `UstecTrendFollow5mEngine` in commit
`b1932d2`.

Read this top to bottom before doing anything in a new session.

---

## 0. The one-paragraph summary

S34 left the working tree in a state that prevented preflight from
passing: 9 tracked files dirty (5+ on the protected list, all
documented S20/S26 work that never got committed) plus ~330 untracked
entries (binaries, sweep outputs, market-data captures, archives, and
a `.github_token` secret). S35 cleaned that up without losing
information — the pre-S34 work is preserved in `stash@{0}` (S26
broker-fill reconciliation, 7 protected files) and `stash@{1}` (S20
Path A IndexBacktest diagnostics). Then the S34 P1 patches landed:
the three remaining XAU trend-follow engines now match the
`UstecTrendFollow5mEngine` close-path pattern (tr.pnl assigned,
tr.engine per-cell, tr.side LONG/SHORT, MFE/MAE tracked). The
`[CSV-ZERO-PNL]` guard in `logging.hpp` will stop firing on these
engines once they next trade. The S33 stack now has uniform close-path
behaviour across all four trend-follow engines. **Next-session
action**: handle the GitHub PAT rotation (security), decide the fate
of the two stashes, then start S34 §6 Option A (USTEC + EURUSD ORB
sweep).

---

## 1. What is at HEAD and what is dirty

**origin/main commit at session start:** `b1932d2`
(after S34 close).

**origin/main commit at session end:** `<NEW SHA after S34-P1 push>`
(see §9 commit log).

The intermediate SHA after S34-hygiene was `403b9cb` — preflight
passed cleanly there before P1 patches landed.

If a new session sees a different HEAD, run `.claude-preflight.sh`
first. The S35 session proved (again) the value of this guard — the
script correctly refused to let work start until 330 untracked
entries were resolved.

### 1.1 Dirty tree state (cleaned this session)

At session start the tree had:

- 9 tracked files modified (S20 + S26 work, never committed)
- ~330 untracked entries (binaries, sweep results, data captures,
  archives, secrets)

At session end the tree is clean. `bash .claude-preflight.sh` exits 0.

### 1.2 Stashes preserved for separate review

Two stashes contain work that S35 deliberately did NOT commit:

```
stash@{0}  PRE-S34 WIP: S26 broker-fill reconciliation
           Files: include/order_exec.hpp (+21 lines),
                  include/OmegaTradeLedger.hpp (+153 lines),
                  include/trade_lifecycle.hpp (+137 lines),
                  include/omega_main.hpp (+58 lines),
                  include/IndexFlowEngine.hpp (+99 lines),
                  include/RiskMonitor.hpp (+26 lines),
                  backtest/microscalper_crtp_sweep.cpp (+444 lines)
           Total: 938 lines across 7 files (all on the protected list).
           Diagnosis (from visible diff content):
             - Adds AvgPx (FIX tag 6) preference over LastPx (tag 31)
               in handle_execution_report — fix for the 2026-05-11
               demo-under-LIVE accounting bug.
             - Adds engineLiveTradeCount() to OmegaTradeLedger.
             - Adds broker-fill reconciliation logic with
               slippage_exit recompute on broker fill.
             - Adds plausibility check for engine-claimed vs broker-
               present trade counts.
           References: HANDOFF_S26.md §2.0,
                       HANDOFF_S26_PART1B_VERIFICATION_REBUILD.md §3.
           Status: NEVER committed. S27 through S34 happened without
           this work. Must be re-evaluated before deciding whether to
           apply, discard, or rebuild — see Option D in §6.

stash@{1}  PRE-S34 WIP: S20 Path A per-gate diagnostic counters
           Files: backtest/IndexBacktest.cpp (+280 lines)
           Dated: 2026-05-08 in comments.
           Adds per-gate-rejection histogram so the operator can see
           which gate is the binding constraint when NSXUSD HistData
           produces 0 trades. Diagnostic only; no production effect.
           Status: NEVER committed. Decision: probably safe to apply
           as a standalone backtest-diagnostics commit.
```

### 1.3 Test that won't build yet

`tests/test_apply_broker_fill_S26_P1B.cpp` was committed in the
S34-hygiene commit. It tests `apply_broker_fill` from `stash@{0}`. It
will NOT compile against current HEAD until `stash@{0}` is applied. If
your build system auto-compiles `tests/`, you'll see an undefined-
symbol error. The test belongs with the S26 stash; treat them as a
pair.

### 1.4 Security finding (HIGH PRIORITY)

`.github_token` was sitting untracked in the repo root. The PAT
referenced in your CLAUDE.md (`<REDACTED-rotated-2026-05-12>`)
should be treated as compromised:

- Visible in CLAUDE.md (which Claude reads at session start)
- Likely present in `.github_token` on disk
- Has been in conversation history

**Action required this week, outside any session:**

1. Generate a new PAT at https://github.com/settings/tokens with
   minimum-required scope (probably just `repo`).
2. Update `~/.netrc` or macOS keychain.
3. Revoke the old PAT immediately at the same URL.
4. Remove the PAT line from CLAUDE.md and replace with a placeholder
   or a path reference: `see ~/.netrc / keychain`.
5. The new `.gitignore` already covers `.github_token`, `.env`,
   `*.pem`, `*.key` — future drops won't surface.

---

## 2. What is running on the VPS

No change to the VPS this session. The four S33 engines + the
S34-patched `UstecTrendFollow5m` are still shadow-firing on the VPS at
whatever SHA was last deployed (S33k at `1511a00` per the S34 handoff,
not changed since).

**Deploy decision for the operator before next `OMEGA.ps1 deploy`:**

The S35-P1 patches change close-path behaviour of three engines that
have been shadow-running with `tr.pnl = 0`. After deploy:

- `XauTrendFollow{4h,2h,D1}` engines will start producing real
  (non-zero) PnL numbers in the ledger.
- `tr.engine` strings will gain a `_<cell.name>` suffix
  (e.g. `XauTrendFollow4h_Donchian_N20_sl1.5tp3.0`). Dashboards or
  queries that group on bare `XauTrendFollow4h` will need to use
  LIKE/prefix-match instead.
- `tr.side` will be `LONG`/`SHORT` instead of `BUY`/`SELL`.
- `tr.mfe` and `tr.mae` will be populated.
- The `[CSV-ZERO-PNL]` warnings from `logging.hpp` will stop firing
  on these engines.

None of these is a logic change to entries or exits. Signal
evaluators, indicator updaters, bar aggregation, and the public API
are byte-identical to before.

---

## 3. What S35 shipped

### 3.1 `403b9cb` — S34-hygiene commit

44 files added (43 source + 1 gitignore), pushed to origin/main.

**`.gitignore` (NEW, 176 lines):** comprehensive coverage of build
artifacts, sweep outputs, market-data captures, logs, archives, and
secrets. Source code (.cpp .hpp .py .sh .md) explicitly allowed.

**Handoff docs (10, committed verbatim):**
- HANDOFF_S14_POST_CTRADER_CULL.md
- HANDOFF_S15_ENGINE_AUDIT.md
- HANDOFF_S17_AFTER_S16_P1_TRACES.md
- HANDOFF_S26_PART3.md
- HANDOFF_S27_AFTER_S26_PART4.md
- HANDOFF_S28_AFTER_S27.md
- HANDOFF_S29_AFTER_S28.md
- HANDOFF_S30_AFTER_S29.md
- HANDOFF_S31_AFTER_S30.md
- HANDOFF_S32_AFTER_S31.md

**FX research engines (3, committed):**
- `backtest/eurusd_bt/EurusdLondonOpenEngine.hpp`
- `backtest/gbpusd_bt/GbpusdLondonOpenEngine.hpp`
- `backtest/usdjpy_bt/UsdjpyAsianOpenEngine.hpp`

**Backtest source (6 .cpp, 6 .py):**
- `backtest/honest_backtest_xauusd{,_v2}.cpp`
- `backtest/option_a_chop_gated_2026-05-09.cpp`
- `backtest/regime_portfolio_2026-05-09.cpp`
- `backtest/replay_microscalper_2026-05-09.cpp`
- `backtest/survivor_stability.cpp`
- `backtest/microscalper_gold_bt.py`
- `backtest/multiday_{asia_compare,option_a,portfolio,replay}_2026-05-09.py`
- `backtest/parse_microscalper_log_2026-05-09.py`

**Operator scripts (17, committed):**
- `scripts/{disparity_post_mortem,duka_wide_grid_aggregate,duka_wide_grid_runner,duka_xau_extreme_asia_validate,duka_xau_fine_aggregate,duka_xau_fine_validate,duka_xau_grid_runner,duka_xau_phase_aggregate,duka_xau_s31_gated_compare,duka_xau_session_aggregate,fx_tape_stats,histdata_to_blackbull,honest_backtest_xauusd,s26p4_aggregate_single_config,verify_extreme_asia_one_day,verify_replay_duka_day,verify_replay_one_day}.{py,sh}`

**Unit test (1, committed):**
- `tests/test_apply_broker_fill_S26_P1B.cpp` (will not compile against
  HEAD; paired with `stash@{0}`)

### 3.2 `<S34-P1 SHA>` — XAU trend-follow close-path fixes

Three files modified, ~80 lines of fix annotations + struct/code
changes each:

```
include/XauTrendFollow4hEngine.hpp   (6 cells)
include/XauTrendFollow2hEngine.hpp   (4 cells)
include/XauTrendFollowD1Engine.hpp   (3 cells)
```

Identical pattern in all three. Bug numbers per HANDOFF_S34.md §3.2:

| # | Location in file | Change |
|---|---|---|
| 1 | `_close()` | Added `pts_move = is_long ? exit-entry : entry-exit`; assigned `tr.pnl = pts_move * lot`. Downstream `tick_value_multiplier(symbol)` in `handle_closed_trade` converts to USD. |
| 3 | `_close()` | `tr.engine = "XauTrendFollow{4h,2h,D1}_" + cell.name` (was bare engine name). |
| 4 | `_close()` | `tr.side = is_long ? "LONG" : "SHORT"` (was BUY/SELL). |
| 5 | `XauTf*Pos` struct + `_fire_entry` + `_manage_open` + `_close` | Added `mfe` / `mae` fields, reset on entry, updated per tick using mid = (bid+ask)/2, propagated to TradeRecord. |

**Deliberately NOT changed:**

- Bug **#2** (symbol-key mismatch) — handoff §4.2 omits it from the
  XAU bug list; `"XAUUSD"` is the correct sizing-table key.
- **S34-B structural guards** (PROVE_IT exit, cell mutual exclusion,
  MIN_ATR floor) — calibrated for 5m USTEC, meaningless on 4h/2h/D1.

**Every signal evaluator, indicator updater, bar-aggregation routine
and public API call site is byte-identical** to the prior version.
Only the position struct, `_fire_entry` MFE/MAE reset, `_manage_open`
MFE/MAE update, and `_close` TradeRecord population were modified.

Verification (each file):
```bash
grep -c "S34 P1 fix #" include/XauTrendFollow{4h,2h,D1}Engine.hpp
# expect 7-9 annotations per file
```

---

## 4. What S35 concluded

### 4.1 The handoff doctrine works (with a caveat)

`.claude-preflight.sh` did its job — refused to let work start on a
non-pristine tree, surfaced 330 untracked entries that had been
hiding behind a truncated terminal view. Without it, S35 would have
started P1 patching against a tree contaminated with months of
exploratory drift.

**Caveat:** the agent contract assumes the operator can resolve the
`[PREFLIGHT-FAIL]` quickly. In practice S35's first ~60% of context
was spent on tree hygiene because the dirty state was enormous. For
the next session, the tree should be clean from the start, so this
won't recur. But the pattern is: agent + operator both pay the cost of
accumulated drift. Periodic hygiene passes (every ~5 sessions?)
prevent this.

### 4.2 The S34 P1 patch surface is now closed

All four `*TrendFollow*` engines (`UstecTrendFollow5m` from S34 commit
`b1932d2`, plus the three patched in S35) now match the same
close-path convention:

- `tr.pnl = pts_move * lot`
- `tr.engine = "<engine>_<cell.name>"`
- `tr.side = "LONG" | "SHORT"`
- `tr.mfe` / `tr.mae` populated per-position

The `[CSV-ZERO-PNL]` writer-side guard from `logging.hpp` (added in
b1932d2) will stop firing on these engines. If you see it fire after
the S35-P1 deploy, that's a bug in a DIFFERENT engine (not one of
these four).

---

## 5. Data inventory (unchanged from S34)

Same as HANDOFF_S34 §5. Notable: the data corpora are unchanged. The
S35 `.gitignore` keeps NEW tick captures untracked by default — the
previously-tracked `data/l2_ticks_2026-04-16.csv` stays tracked, but
the ~50 newer ones (XAU + US500 + USTEC + NAS100 captures, April-May
2026) will not appear in `git status`. If you want to start tracking
those, you'll need explicit `git add -f data/l2_ticks_XXXX.csv` for
each.

---

## 6. Next-session work plan (in order)

### Option D (NEW, high priority) — decide the fate of the stashes

`stash@{0}` and `stash@{1}` contain real, dated, documented work. The
S26 stash in particular (broker-fill reconciliation) fixes a known
production incident (2026-05-11 demo-under-LIVE accounting bug). It
needs a decision:

(a) **Apply and commit.** Run `git stash pop stash@{0}`, review each
    of the 7 protected files individually, verify the test in
    `tests/test_apply_broker_fill_S26_P1B.cpp` compiles and passes,
    commit as `S26-late: broker-fill reconciliation (originally
    2026-05-11, recovered from stash 2026-05-{13+})`.

(b) **Discard.** Run `git stash drop stash@{0}`. Acceptable only if
    you can confirm a later commit (S27+) already implements the same
    fix. Re-read HANDOFF_S26.md §2.0 and HANDOFF_S26_PART1B §3, plus
    a `git log -p include/order_exec.hpp` to look for a later
    AvgPx-related commit.

(c) **Rebuild.** Discard the stash, re-implement from the handoff
    docs. Slower but cleaner if the original WIP was incomplete.

Same three options for `stash@{1}` (the IndexBacktest diagnostic).
Lower urgency — it's diagnostic-only.

**Recommended:** do (a) for both stashes as the first task of next
session. They're committed work that just never landed.

### Option A — same harness, two more symbols (from HANDOFF_S34 §6)

The `open_orb_1m_crtp_sweep` already has baselines for USTEC.F,
US500.F, and EURUSD. Two runs to find out whether the candidate has
edge on a different instrument. Commands per HANDOFF_S34 §6 Option A.

### Option B — ThreeBar per-year cross-validation (from HANDOFF_S34 §6)

Spec per HANDOFF_S34 §6 Option B. Gate to flip
`g_xau_threebar_30m.enabled = true`.

### Option C — pairs trading US500/USTEC (from HANDOFF_S34 §6 §4 item 2)

Net-new build. Multi-session scope.

**Recommended order for next session:** Security (rotate the PAT,
outside any session) → Option D part (a) for both stashes → Option A
(two harness runs) → Option B if time permits.

### Open P-items

- **P1 (was open in S34; now CLOSED by S35-P1.)** ✓
- **P2 (NEW):** If the S26 stash is applied (Option D-a), the
  resulting commit should land BEFORE any further VPS deploy of S34/
  S35 changes — the broker-fill reconciliation is foundational and
  every other engine's PnL trustworthiness depends on it.

---

## 7. How to run anything in this handoff

### 7.1 Preflight

```bash
cd ~/omega_repo
bash .claude-preflight.sh
# Expected: [PREFLIGHT-OK] tree at <sha>, clean, fetched <60s ago
```

### 7.2 Apply the S26 broker-fill stash (Option D-a)

```bash
cd ~/omega_repo
git stash list                              # confirm stash@{0} is the S26 one
git stash show -p stash@{0} | less          # review the full diff first
git stash pop stash@{0}                     # apply (will drop on success)
git status --short                          # confirm 7 files now show ' M'

# Cross-check the test compiles:
clang++ -std=c++20 -Iinclude -fsyntax-only \
    tests/test_apply_broker_fill_S26_P1B.cpp && \
    echo "[OK] test compiles"

# If test compiles and you've reviewed each of the 7 protected files:
git add include/order_exec.hpp include/OmegaTradeLedger.hpp \
        include/trade_lifecycle.hpp include/omega_main.hpp \
        include/IndexFlowEngine.hpp include/RiskMonitor.hpp \
        backtest/microscalper_crtp_sweep.cpp
git commit -m "S26-late: broker-fill reconciliation (recovered from stash)

Originally dated 2026-05-11 per inline comments; never committed at
the time. Recovered from PRE-S34 WIP stash 2026-05-{13+}. Implements
the fix for the 2026-05-11 demo-under-LIVE accounting incident where
6 trades logged as wins (engine target = TP) were actually losses
(broker fill below TP).

Pairs with tests/test_apply_broker_fill_S26_P1B.cpp which has been in
the repo since 2026-05-12 (S34-hygiene commit) but did not compile
against HEAD until now.

See HANDOFF_S26.md §2.0 and HANDOFF_S26_PART1B_VERIFICATION_REBUILD.md
§3 for the incident description and earlier implementation notes."
git push origin main
```

### 7.3 Re-deploy after S35-P1

After preflight passes and you're satisfied with the changes:

```powershell
# On VPS (Windows PowerShell):
OMEGA.ps1 deploy
```

Watch the ledger for the first non-zero `XauTrendFollow*` trade to
confirm the fix took effect.

---

## 8. Safety invariants carried forward

Unchanged from S32/S33/S34:

1. `mode=SHADOW` in `omega_config.ini` (committed to origin/main).
2. `max_lot_gold=0.01`.
3. Protected file list (no S35 commit touches any of these — S35 only
   modified files that were not on the protected list):
   - `include/order_exec.hpp`
   - `include/OmegaTradeLedger.hpp`
   - `include/trade_lifecycle.hpp`
   - `include/omega_main.hpp`
   - `backtest/microscalper_crtp_sweep.cpp`
   - `include/IndexFlowEngine.hpp`
   - `include/RiskMonitor.hpp`
4. Single live-eligible engine = `GoldMicroScalperEngine` (disabled).
5. No live promotion without `ledger_reconcile` showing
   `sum_pnl_delta < $20/day`.
6. `.claude-preflight.sh` is the first command of every session.
7. **(NEW S35):** PAT/token files (`.github_token`, `.env`, etc.) are
   gitignored. Never commit a secret. Rotate any PAT that has been
   visible in CLAUDE.md or chat history.

---

## 9. Commit log this session

```
403b9cb  S34-hygiene: handoff docs S14-S32, research scripts, engine src, .gitignore
<S34-P1> S34-P1: close-path fixes for XauTrendFollow{4h,2h,D1}Engine
<pending> S35-doc: HANDOFF_S35.md (this file)
```

To commit and push the handoff itself:

```bash
cd ~/omega_repo
cp "/Users/jo/Library/Application Support/Claude/local-agent-mode-sessions/d5a54fb8-7e98-42ba-bcbf-75183e209e29/da3bfbe4-817e-428a-a54c-5bdcdb6373f4/local_f0ae43bf-d4a2-4b70-84f5-d8980c1d820d/outputs/HANDOFF_S35.md" \
   HANDOFF_S35.md
git add HANDOFF_S35.md
git commit -m "S35-doc: session handoff

Documents S35's tree-hygiene work (preflight unblock, 43-file commit
plus .gitignore) and the S34 P1 close-path patches landed on the
three XAU trend-follow engines. Two stashes preserved for next-session
review (S26 broker-fill reconciliation, S20 IndexBacktest diagnostic).
Security item: rotate the GitHub PAT exposed in CLAUDE.md."
git push origin main
```

---

## 10. Outstanding action items (TL;DR for the human)

1. **THIS WEEK, OUTSIDE ANY SESSION (security):** Rotate the GitHub
   PAT at https://github.com/settings/tokens. Remove from CLAUDE.md.
2. **Start of next session:** preflight, then Option D-a (apply
   `stash@{0}` after review, commit, push).
3. **Then:** Option D-a for `stash@{1}` (S20 diagnostic).
4. **Then:** Option A from HANDOFF_S34 §6 (ORB on USTEC + EURUSD).
5. **Eventually:** decide on Option B (ThreeBar per-year cross-val)
   and Option C (pairs trading) per their original HANDOFF_S34 §6
   specs.

End of HANDOFF_S35.
