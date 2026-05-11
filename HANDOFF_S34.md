# HANDOFF S34 — 2026-05-12

End-of-day state after a session that (a) found and fixed a class of
close-path bugs in `UstecTrendFollow5mEngine`, (b) added a preflight
guard against the stale-clone failure that opened the session, (c) added
writer-side dedupe + zero-PnL diagnostics to `logging.hpp`, (d) shipped
a deploy-candidate `XauThreeBar30mEngine` for the Pass-8 untapped cell
from S33, and (e) built and ran a CRTP Phase 0 sweep harness for the
1-minute London/NY ORB candidate — which **rejected the candidate** on
the full 30-month Duka XAU corpus.

Read this top to bottom before doing anything in a new session.

---

## 0. The one-paragraph summary

The S33 trend-follow stack was carrying a duplicated PnL=$0 bug across
all four new engines (`Xau{4h,2h,D1}` + `UstecTrendFollow5m`). It only
became visible when the operator pasted a USTEC trade log showing
`+$0.00` on every fill. Diagnosis isolated five close-path bugs;
`UstecTrendFollow5mEngine.hpp` is now patched and the other three are
documented (P1) but **not yet patched** — they will produce
`[CSV-ZERO-PNL]` warnings via the new logging guards the moment they
fire, so the operator will see the bug class surface as soon as those
engines trade. A new candidate engine, `XauThreeBar30mEngine` (S33 §4
item 5, n=639 +$979/30mo, BE=$1.59), is included as deploy-ready but
hard-pinned `enabled=false` pending per-year cross-validation. A CRTP
Phase 0 harness for the 1-minute London/NY ORB candidate (S33 §4 item
3) was built, three bugs in the harness itself were found and fixed,
and the corrected harness conclusively **rejected** the ORB candidate
on XAU — 216/216 cells `FAIL_net_le_0`, best cell -$106.85 over 30
months. Walk away from 1-min ORB on XAU. Three options queued for next
session: rerun the harness on USTEC and EURUSD (different cost
profiles), build the ThreeBar per-year cross-validation sweep, or
attack the next candidate from `HANDOFF_S33_FINAL.md` §4.

**Next-session action**: run the three Phase 0 sweeps listed in §6,
decide on ThreeBar promotion based on per-year results, leave the rest
of the S33 stack untouched.

---

## 1. What is at HEAD and what is dirty

**origin/main commit:** `b1932d2` (pushed during this session).
**HEAD of the local working tree:** `b1932d2` + one uncommitted file
described in §3.3.

If a new session sees a different HEAD, run `.claude-preflight.sh`
first — it will fail loudly rather than let an agent start
investigating against a stale tree, which is the exact failure mode
that wasted the first half of this session. See `PREFLIGHT.md`.

### 1.1 Pre-existing dirty tree (NOT this session's work)

Before this session started, the working tree already had nine files
modified. Five of them are on the S33 protected list. They are
**untouched** by S34 and must be reviewed by the operator before
committing or discarding:

```
 M backtest/IndexBacktest.cpp
 M backtest/microscalper_crtp_sweep.cpp     ← protected
 M data/l2_ticks_2026-04-16.csv
 M include/IndexFlowEngine.hpp              ← protected
 M include/OmegaTradeLedger.hpp             ← protected
 M include/RiskMonitor.hpp                  ← protected
 M include/omega_main.hpp                   ← protected
 M include/order_exec.hpp                   ← protected
 M include/trade_lifecycle.hpp              ← protected
```

These predate S34. Do not commit them as part of an S34 commit —
either review and split them into their own commit(s), `git restore`
them if they were exploratory, or `git stash` them before continuing.

### 1.2 Stale `.git/index.lock`

`/Users/jo/omega_repo/.git/index.lock` exists and blocks `git add`
operations from the Cowork container (different UID). Remove it on
your Mac before the next git operation:

```bash
rm /Users/jo/omega_repo/.git/index.lock
```

---

## 2. What is running on the VPS

No change to the VPS from this session. The four S33 engines are still
shadow-firing on `1511a00` (S33k). After this commit lands, the VPS
will need a `OMEGA.ps1 deploy` to pick up the S34 changes — but the
**operator must decide first** whether to deploy:

- The `logging.hpp` guards will start surfacing `[CSV-ZERO-PNL]`
  warnings against the three un-patched `XauTrendFollow{4h,2h,D1}`
  engines as soon as those engines fire. That is the *desired*
  behaviour — it tells you the bug class is present — but the
  operator should be aware so the warnings aren't mistaken for new
  bugs introduced by S34.
- `UstecTrendFollow5mEngine` will start producing real (non-zero) PnL
  numbers, distinct `engine=UstecTrendFollow5m_{Donchian|Keltner}`
  strings, and `LONG`/`SHORT` in the side column. Behaviour change:
  cells now mutex same-direction firing, ATR < 10pts skips the bar,
  and there is a new `PROVE_IT_FAIL` exit reason after 90s without
  4pt of favourable movement.
- `XauThreeBar30mEngine` ships with `enabled=false`. It will not
  fire until the per-year cross-validation in §6 confirms it.

---

## 3. What S34 shipped

### 3.1 `b1932d2` — initial S34 commit (already pushed)

Six files, all new or modified-by-S34:

```
.claude-preflight.sh                                 NEW
PREFLIGHT.md                                         NEW
backtest/open_orb_1m_crtp_sweep.cpp                  NEW   (buggy — see §3.3)
include/XauThreeBar30mEngine.hpp                     NEW
include/UstecTrendFollow5mEngine.hpp                 MODIFIED  (S34 bugs 1-5 + S34-B guards A,B,C)
include/logging.hpp                                  MODIFIED  (dedupe + zero-PnL diag)
```

### 3.2 The five close-path bugs in `UstecTrendFollow5mEngine` (now fixed)

Documented in the engine's S34 header block, lines roughly 67-110:

| # | Bug | Fix |
|---|---|---|
| 1 | `tr.pnl` never assigned, propagated as 0.0 through `tr.pnl *= tick_value_multiplier` | `tr.pnl = pts_move * lot` in `_close`, matches `BracketEngine.hpp:1216-1218` |
| 2 | `tr.symbol = "USTEC"` (sizing table keyed on "USTEC.F") → 1.0 multiplier fallback | `tr.symbol = "USTEC.F"` |
| 3 | Both cells wrote identical `tr.engine = "UstecTrendFollow5m"` → indistinguishable in ledger | `tr.engine = "UstecTrendFollow5m_" + cell.short_name` |
| 4 | `tr.side = "BUY"/"SELL"` rather than ledger convention | `tr.side = "LONG"/"SHORT"` |
| 5 | No MFE/MAE tracking — `tr.mfe`/`tr.mae` shipped as 0 | per-tick mid-based update in `_manage_open` |

### 3.3 The S34-B structural guards in `UstecTrendFollow5mEngine`

Three guards added on top of the close-path fixes. Documented in the
engine's S34-B header block:

- **Guard A — Prove-it exit.** `PROVE_IT_SECS=90`,
  `PROVE_IT_MIN_FAVOURABLE_PTS=4.0`. If the trade hasn't shown 4pt
  favourable movement within 90 seconds, force-close with reason
  `PROVE_IT_FAIL`. Also a `MIN_SL_PTS_FLOOR=15.0` so a low-ATR window
  can't produce an SL inside spread+noise.
- **Guard B — Cell mutual exclusion (same-direction only).** When one
  cell has an open position in direction X, the other cell can't
  open in the same direction X. Opposite-direction is still allowed.
- **Guard C — Minimum ATR floor.** `MIN_ATR_PTS=10.0`. Skip 5m bars
  with `atr14 < 10pt` — chop, no breakout entries.

All five constants are `static constexpr` on the engine, tunable at
the top of the file.

### 3.4 The `logging.hpp` writer-side guards

Two defensive guards added to `write_trade_close_logs`:

- **`[CSV-DUP-BLOCK]`** — 5-second × 64-slot ring-buffer dedupe on
  `engine|symbol|side|entryTs|exitTs|exitReason`. Drops exact-
  duplicate close rows at the writer (catches both legit upstream
  double-emit like `stale_cb` AND any engine that fires twice).
- **`[CSV-ZERO-SIZE]` / `[CSV-ZERO-PNL]`** — warns (but still writes
  the row) when `size <= 0` or when `entry != exit` but `net_pnl ==
  0`. **This is how you will see the other three XauTrendFollow
  engines surface their PnL=0 bug post-deploy.**

### 3.5 `.claude-preflight.sh` + `PREFLIGHT.md`

Hard guard against the stale-tree failure mode. Exit codes:

| Code | Failure |
|---|---|
| 2 | local HEAD != origin/main |
| 3 | uncommitted changes |
| 4 | `git fetch` failed |
| 5 | last fetch > 60 sec ago |
| 6 | not in a git work tree |
| 7 | detached HEAD (under `--strict`) |

Contract: every Claude session against this repo must run
`.claude-preflight.sh` as its first command and refuse to proceed on
non-zero exit. Documented in `PREFLIGHT.md`.

### 3.6 `XauThreeBar30mEngine.hpp` — deploy-candidate

The Pass-8 untapped cell from `HANDOFF_S33_FINAL.md` §4 item 5
(n=639, +$979/30mo, BE=$1.59). Single cell, single position, fixed TP
4×ATR / SL 2×ATR, 1-bar cooldown. **`shadow_mode=true` AND
`enabled=false`** by default — production-ready *implementation* but
not promoted until per-year cross-validation confirms the cell. Written
correctly from the start: matches the `BracketEngine` close-path
convention, so it does NOT inherit the bug class documented in §3.2.

Integration call sites are documented in the file's USAGE block. The
operator needs to add a hook in `tick_gold.hpp` at the M30 bar close
(or wherever the 30m bar-close dispatch lives) when promoting.

### 3.7 `backtest/open_orb_1m_crtp_sweep.cpp` — Phase 0 sweep

Standalone CRTP harness for the 1-minute London/NY ORB candidate
(S33 §4 item 3). 216-cell parameter grid over RW × EW × TP × SL ×
direction. Reads three tick-CSV shapes (3-col Duka, 16-col S19 L2,
17-col mid-included L2). Multi-symbol baselines (XAUUSD, EURUSD,
USTEC.F, US500.F) so the same binary runs on any of them. Hard
acceptance gates: n≥100, NET>0 after $0.06/RT, every Duka year > 0,
WR ≥ structural BE. Ranked output to
`orb_1m_sweep_results/leaderboard_<SYM>.csv`.

**This file as committed in `b1932d2` has three bugs (§3.8). Fix
commit is uncommitted in the working tree at handoff time.**

### 3.8 Harness bugfixes (uncommitted at handoff)

After the operator's full-corpus run produced suspicious all-zero
results, three bugs in `backtest/open_orb_1m_crtp_sweep.cpp` were
found and fixed in the working tree. Need to be committed as `S34-fix`:

| # | Bug | Fix |
|---|---|---|
| H1 | `run_cell` rescored every accumulated trade on every file call → `net_usd_total` inflated by O(N_files²) | Scoring moved to new `finalize_cell`, called once after all files |
| H2 | `extract_year` looked for underscore; Duka filenames are `YYYY-MM-DD.csv` with hyphens → all files got `year=0` | Scans basename for any 4-digit run in [1900, 2099] |
| H3 | Tick-CSV loader didn't detect the 17-column L2 format (with `mid` column at index 1) | Header parser reads col-1 name; if `"mid"` it skips that column when reading bid/ask |

To commit (run from `/Users/jo/omega_repo` on the Mac):

```bash
rm -f .git/index.lock
git status backtest/open_orb_1m_crtp_sweep.cpp
git add backtest/open_orb_1m_crtp_sweep.cpp
git commit -m "S34-fix: open_orb_1m_crtp_sweep — per-file scoring rerun, year extraction, L2 17-col loader"
git push origin main
```

---

## 4. What S34 found and concluded

### 4.1 1-minute London/NY ORB on XAU — REJECTED

Full Duka corpus (623 days, ~111M ticks, Sep 2023 → Sep 2025) through
the fixed harness:

```
[INFO] loaded 111,599,267 ticks across 623 files
[DONE] 216 cells evaluated   0 PASS
[VERDICT] NO cells passed all gates on this corpus. Per the S33
          discipline, walk away — do NOT build a production engine
          from a sweep with zero validated cells.
```

Verdict distribution: 216/216 `FAIL_net_le_0`. Best cell:

```
ORB_RW1_EW3_TP2_SL1_FADE   n=991   WR=28.6%   net=-$106.85 / 30mo
```

(-$0.108 per trade on a 0.01 lot at $20/pt × cost $0.06/RT). The
direction skew is mildly interesting — best cells are FADE not
FOLLOW, meaning the rare cells that DO break the opening range
within minutes tend to mean-revert. But the edge is below cost; no
production engine is built from this.

**Treat this as a closed line of investigation.** The candidate is
mechanically tested and decisively rejected on XAU. The same harness
can still be tried on USTEC.F and EURUSD (different cost profiles)
— that is queued for next session §6.

### 4.2 Three un-patched engines (P1 for next session)

The S34 bug class identified in §3.2 is present in `XauTrendFollow4h`,
`XauTrendFollow2h`, and `XauTrendFollowD1` engines. The pattern is
identical — `_close()` does not assign `tr.pnl`, uses `BUY/SELL`
instead of `LONG/SHORT`, no MFE/MAE tracking, single engine string
(no per-cell suffix). Each engine has 3-6 cells. Once any of them
fires, the trade row will show `tr.pnl = 0` and the new `[CSV-ZERO-
PNL]` guard in `logging.hpp` will print a warning.

The patches are mechanical — same five fixes as `UstecTrendFollow5m`.
**Estimated effort: one focused session, three files, ~400 lines of
edits.**

---

## 5. Data inventory (what is available for next session)

```
outputs/duka_xauusd_daily/*.csv      623 files, ~111M ticks
                                      Sep 2023 → Sep 2025, 30 months
                                      3-col format: ts_ms,bid,ask

outputs/histdata_eurusd_daily/*.csv  184 files, ~1 year EUR HistData
                                      Per S33 FINAL §3: edge too thin
                                      vs costs ($0.07-$0.23 BE vs
                                      $0.06 cost), but the harness
                                      will run cleanly.

data/l2_ticks_*.csv                  15 files, ~3-4 weeks L2 capture
                                      16-col S19 format. XAU-only.

logs/l2_ticks_XAUUSD_*.csv           Daily L2 capture (live).
                                      17-col format with mid column.
                                      Both formats are now auto-
                                      detected by the harness.

HISTDATA_COM_ASCII_NSXUSD_T202512.zip Single-month NAS100 ASCII tick
                                      data. Not yet integrated.
```

USTEC.F L2 corpus: **~30 days** (per S33 FINAL). On the thin side
for n≥100 gate. The harness will run but may not produce enough
trades per cell.

---

## 6. Next-session work plan (in order)

### Option A — same harness, two more symbols

The `open_orb_1m_crtp_sweep` already has baselines for USTEC.F,
US500.F, and EURUSD. Two runs to find out whether the candidate has
edge on a different instrument:

```bash
# USTEC ORB sweep (cost is 0 commission + spread)
./open_orb_1m_crtp_sweep data/l2_ticks_*.csv --symbol USTEC.F \
    --outdir orb_1m_sweep_results --cost 0.0 --verbose

# EURUSD ORB sweep (HistData corpus)
./open_orb_1m_crtp_sweep outputs/histdata_eurusd_daily/*.csv \
    --symbol EURUSD --outdir orb_1m_sweep_results --cost 0.06 \
    --verbose
```

Expected: either similar 0-PASS verdict (confirms 1-min ORB has no
edge across symbols at retail cost), or a single positive cell that
warrants a production engine on that symbol. Either outcome is a
real result.

### Option B — ThreeBar per-year cross-validation

Before flipping `g_xau_threebar_30m.enabled = true`, the cell needs
year-by-year all-positive confirmation. The existing
`backtest/edge_hunt.cpp` already has the ThreeBar signal logic
(`sig_three_bar`, lines 604-624). What it does NOT have is a clean
per-year split.

Spec for next session: write `backtest/threebar_30m_yearly.cpp`
modeled on `backtest/top_cells_monthly.cpp`'s `cell_three_bar` (line
307+). Run the ThreeBar cell with `sl_mult=2.0`, `tp_mult=4.0` on
Duka XAU 30m bars (need to bar-aggregate the tick corpus to 30m
first). Split results per Duka year. Acceptance: 3/3 years positive
AND ≥100 trades per year. Pass → flip `enabled=true` in
`engine_init.hpp` + add the M30 bar-close dispatch hook in
`tick_gold.hpp`. Fail → engine stays disabled, document why.

### Option C — next S33 §4 candidate

Per `HANDOFF_S33_FINAL.md` §4, in rough order of likelihood-to-find-
edge:

1. News-event regime-shift (needs an econ calendar feed — not yet
   present in the repo; add Investing.com / ForexFactory ICS feed
   as a separate research task before this becomes runnable)
2. **Pairs trading US500.F / USTEC.F** (market-neutral, never built;
   both tick streams are present in the L2 corpus) ← **good
   candidate for the next session if Options A and B finish cleanly**
3. Stop-hunt fade on L2 (microstructure-specific, detect spikes
   >2σ in <30s, fade reversal)
4. Hurst exponent regime switching (H>0.55 → trend mode,
   H<0.45 → mean-rev; adaptive strategy selection)

**Recommended order for next session:** A (one hour, two runs) → B
(two hours, write the cross-val + run it) → C item 2 if time
permits.

### Open P1 (not in the option list — separate priority)

Patch the three un-patched `XauTrendFollow{4h,2h,D1}` engines for
the S34 bug class. Mechanical work, three files. **Do this BEFORE
deploying any further S34 changes to the VPS** — otherwise the
`[CSV-ZERO-PNL]` guard will spam warnings the moment those engines
fire and the operator will lose visibility.

---

## 7. How to run anything in this handoff

### 7.1 Build the harness

```bash
cd ~/omega_repo
clang++ -std=c++20 -O2 -Iinclude \
    backtest/open_orb_1m_crtp_sweep.cpp \
    -o open_orb_1m_crtp_sweep
```

### 7.2 Run on the full XAU corpus

```bash
mkdir -p orb_1m_sweep_results
./open_orb_1m_crtp_sweep outputs/duka_xauusd_daily/*.csv \
    --outdir orb_1m_sweep_results --verbose 2>&1 | tail -30
```

### 7.3 Inspect results

```bash
# Top 10 cells by net_usd_total
head -11 orb_1m_sweep_results/leaderboard_XAUUSD.csv | column -t -s,

# Just the PASS cells (if any)
grep ",PASS," orb_1m_sweep_results/leaderboard_XAUUSD.csv | column -t -s,

# Verdict distribution
awk -F, 'NR>1 {print $9}' orb_1m_sweep_results/leaderboard_XAUUSD.csv \
    | sort | uniq -c
```

### 7.4 Preflight against this tree

```bash
bash ~/omega_repo/.claude-preflight.sh
# Expected on a clean tree: [PREFLIGHT-OK] tree at <sha>, clean
```

---

## 8. Safety invariants carried forward from S32/S33

1. `mode=SHADOW` in `omega_config.ini` (committed to origin/main).
2. `max_lot_gold=0.01`.
3. No protected file modified by S34. Protected list per HANDOFF_S33:
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

---

## 9. Commit log this session

```
b1932d2  S34: preflight + UstecTF5m fixes + logging guards + XauThreeBar30m + ORB sweep
<pending>  S34-fix: open_orb_1m_crtp_sweep — per-file scoring rerun, year extraction, L2 17-col loader
<pending>  S34-doc: HANDOFF_S34.md
```

The two `<pending>` commits are the work the operator needs to land
before the next session starts. See §3.8 for the harness-fix commit
command. The handoff doc itself should be committed at the same time:

```bash
git add HANDOFF_S34.md backtest/open_orb_1m_crtp_sweep.cpp
git commit -m "S34-fix: harness bugs + handoff doc"
git push origin main
```

End of HANDOFF_S34.
