# HANDOFF — S20 (continues from S19 MicroScalper buildout)

Prepared: 2026-05-08, end of S19
Branch / HEAD: `main` (push S19 commits before reading)
Mode: `mode=SHADOW` (no live orders firing). All S19 changes are paper-only.
Standing user preferences carried over from S18/S19: full code (no snippets/diffs), warn at 70% chat with summary, never modify core code without clear instruction, paste-friendly bash commands (no leading `#`), warn before any session-usage block.

---

## TL;DR for the fresh Claude reading this

S19 built a new gold engine end-to-end and shipped it for shadow deployment:

- **`include/GoldMicroScalperEngine.hpp`** (new, ~480 lines) — bidirectional micro-tick scalper for XAUUSD. 20-tick z-score reversion entry with L2 confirmation. BE-arm at 0.5pt MFE, aggressive trail at MFE-0.5pt post-BE, immediate reversal-exit on tick-momentum or L2 slope flip. Calibrated against 28 days of captured XAUUSD L2 (6.7M ticks, two CRTP sweep iterations). Shadow-only by default.

- **`backtest/microscalper_crtp_sweep.cpp`** (new, ~700 lines) — single-TU CRTP sweep harness. 490-combo pairwise 5-param geometric grid (0.5×..2.0×) mirroring the `OmegaSweepHarnessCRTP.cpp` pattern. Faithful port of the live engine's tick logic with per-combo Traits. Mac/Clang only with `-fbracket-depth=1024` (same flag as the cohort harness). Runs in ~130s on the full month of L2 tape.

- **Operating point promoted to engine defaults: rk12 (Z=0.75 / TP=0.79 / SL=3.00 / BE=0.50 / TR=0.50).** N=36,575 backtest trades, WR=92.5%, PF=4.40, net 203.6 pt·lot over 28 days. Selected over the boundary-saturated Z=0.38 max-net config (rk1) for robustness — mid-grid on every parameter, highest PF/WR on the leaderboard.

- **Wired into the live runtime** (NOT yet deployed — push pending): `globals.hpp`, `engine_init.hpp` (shadow_mode pin + `register_engine` + heartbeat + `open_positions` source + banner string), `tick_gold.hpp` (heartbeat pulse + dispatch block parallel to MidScalper). Standard cohort plumbing.

- **`FIX_SSH.ps1`** (new, ~190 lines) and a follow-up patch — one-shot SSH key-auth repair for the Windows VPS. Originally written, hit the recurring "key works once then breaks" bug, then patched after the 2026-05-08 sshd debug log identified the root cause: `Add-Content` in PowerShell doesn't prepend a newline when the existing file lacks one, producing concatenated keys like `...comment1ssh-ed25519 BLOB2 comment2` that sshd parses as ONE key. The patched script now reads as raw text, splits via lookahead before each algorithm prefix (recovers from concatenated files), de-dupes by base64 blob, writes back with explicit `\n` and `WriteAllText` to control encoding.

- **L2 capture infrastructure mapped out.** Logger added 2026-04-04 (commit `86ba144`). Captures evolved: April 9-21 unprefixed (gold-only); April 22 onward per-symbol prefix (`l2_ticks_<SYMBOL>_<DATE>.csv`). Currently 64 daily files on Mac at `data/` covering XAUUSD + US500 + USTEC + NAS100. FX symbols are NOT yet being captured — logger needs extension before Phase 0 can run on FX.

What's left for S20 is Phase 0 of the multi-symbol expansion: extend the sweep harness to be symbol-parameterized and validate the edge exists for indices (US500, USTEC, NAS100) before duplicating any engine code. Then capture FX L2 for 2 weeks and repeat.

---

## STEP 1 — Get repo access

The sandbox does NOT have a folder mounted by default. First action:

```
mcp__cowork__request_cowork_directory(path="~/omega_repo")
```

Mount path: `/Users/jo/omega_repo/`. Bash sandbox path differs (`/sessions/<id>/mnt/omega_repo/`) but bash is broken — see Step 2.

---

## STEP 2 — Sandbox + repo + Mac environment

### Sandbox VM

`mcp__workspace__bash` was **still broken at start of S19** with the same symptom as S13/S15-S19:
```
useradd: /etc/passwd.NNNNNN: No space left on device
```
This is a hard environment limit, not transient. Plan around broken bash for the critical path. CSV inspection uses chunked `Read` (500-2000 lines per call) or delegates to `general-purpose` Agent. **Sweep validation is C++ on the user's Mac**, not in the sandbox.

### Mac local environment

```
host:    jo@Jos-MacBook-Pro
shell:   zsh   (NOT bash -- `#` comment lines in pasted scripts cause errors)
repo:    ~/omega_repo
```

When pasting multi-line shell scripts, **strip leading `#` comments** before sending. Zsh interprets each as a command and emits `zsh: command not found: #` errors.

### GitHub remote

```
URL:     https://github.com/Trendiisales/Omega
branch:  main
```

PAT in global `CLAUDE.md` is read-only. User pushes manually with their own credentials.

### VPS access

```
host:    185.167.119.59
OS:      Windows Server (RDP + PowerShell)
SSH:     port 2222 (sshd running; OpenSSH for Windows 9.5)
user:    trader (Administrator group -> sshd uses
                  C:\ProgramData\ssh\administrators_authorized_keys
                  NOT C:\Users\trader\.ssh\authorized_keys)
ROOT:    C:\Omega
```

If SSH key auth fails, the most likely cause (verified 2026-05-08) is that `administrators_authorized_keys` got wiped, ACL-reset, or has missing-newline corruption. `FIX_SSH.ps1` at repo root handles all three. See its docstring for the diagnostic command (tail `C:\ProgramData\ssh\logs\sshd.log` while connecting from Mac).

### Build / deploy

User runs `.\OMEGA.ps1 deploy` on the VPS. The S19 changes need this — wiring is dead until next process restart.

### Trading mode

`mode=SHADOW` per `omega_config.ini`. The S19 GoldMicroScalper changes are paper-only.

---

## STEP 3 — What's in the tree at end of S19

### Already pushed (in `b0e5a72` or earlier)

S18 omnibus (P1-5/6/7/9/10/11/12 closeouts + VWAPReversion EURUSD tune + force_close C4458 build-fix) — see `HANDOFF_S19_AFTER_S18_CLOSEOUTS.md` for the trail.

### S19 commits stacked on top (push pending or push-in-progress)

| File | What |
| --- | --- |
| `include/GoldMicroScalperEngine.hpp` | NEW. ~480 lines. Bidirectional micro-tick scalper for XAUUSD. Calibrated rk12 defaults: Z=0.75 / TP=0.79 / SL=3.00 / BE=0.50 / TR=0.50. |
| `backtest/microscalper_crtp_sweep.cpp` | NEW. ~700 lines. CRTP sweep harness. 490 combos / 5 swept params. Anchor `MicroScalperBaseParams::ENTRY_Z=0.75` (sweep-only; live engine default unaffected). |
| `include/globals.hpp` | Added `#include "GoldMicroScalperEngine.hpp"` block + `static omega::GoldMicroScalperEngine g_gold_microscalper;` parallel to MidScalper. |
| `include/engine_init.hpp` | Four parallel insertions to MidScalperGold pattern: shadow_mode pin, register_engine, heartbeat (06-22 UTC, 3600s cadence), open_positions source. Banner string `8 sources` -> `9 sources`. |
| `include/tick_gold.hpp` | Heartbeat pulse + dispatch block parallel to MidScalper (line ~2202). Reads L2 from `g_macro_ctx.gold_*`. |
| `FIX_SSH.ps1` | NEW. ~190 lines. One-shot Windows OpenSSH key-auth repair. Patched mid-S19 to handle missing trailing newline + concatenated-keys recovery. |
| `HANDOFF_S20_AFTER_S19_MICROSCALPER.md` | THIS file. |

### Untracked (carry-over from S15/S16/S17/S18 — no change)

```
?? HANDOFF_S14_POST_CTRADER_CULL.md
?? HANDOFF_S15_ENGINE_AUDIT.md
?? audit_input/
```

User-deferred decision since S15. S19 did not touch these.

### S19-touched but session-only

- The 64 L2 CSVs in `data/` and the original three (`backtest/l2_ticks_2026-04-09.csv`, `l2_ticks_2026-04-10.csv`, `data/l2_ticks_2026-04-16.csv`). All gitignored via `.gitignore` line 13 (`*.csv`) and line 51 (`data/`).
- `backtest/sweep_microscalper.csv` — sweep output; gitignored.
- `backtest/microscalper_crtp_sweep` (compiled binary; gitignored via `*.exe` rule + binary patterns).

---

## STEP 4 — What S19 deliberately did NOT do

Don't redo any of these — they're handled or deferred:

- **Run `.\OMEGA.ps1 deploy`.** Source-only edits. User runs deploy on the VPS after pushing.
- **Promote MicroScalperGold to live.** Pinned shadow-only in `engine_init.hpp` per cohort convention. Promotion gate: 2-week paper validation matching backtest expectancy within 50% on N>=500 fills.
- **Add MicroScalperGold to `gold_any_open` exclusion gate.** Currently the engine can fire alongside MidScalperGold. Once promoted to live, add `g_gold_microscalper.has_open_position()` to `gold_any_open` AND add the S54-style anti-double-entry guard against MidScalperGold+MicroScalperGold double-firing on the same compression structure.
- **Move tuned params to omega_config.ini.** All `static constexpr` per cohort convention. P3 enhancement.
- **Duplicate the engine for indices or FX.** Phase 0 advisory written (see Step 5) — validate edge per symbol via sweep BEFORE writing engine code. Do NOT blanket-stamp 10 symbol engines.
- **Extend `add_l2_logger.py` to capture FX L2.** Required for Phase 0 FX validation. Currently only XAUUSD + indices are being captured (as of `l2_ticks_<SYMBOL>_*.csv` files seen on the VPS).
- **Touch any other engine.** S18's P1-1 / P1-4 / MAE_EXIT_RATIO / VWAPReversion EURUSD post-shadow re-tunes are still tape-dependent and outstanding.

---

## STEP 4.5 — Phase 0 INDICES result (executed end of S20, 2026-05-08)

**Verdict: STOP. Don't duplicate the GoldMicroScalper for US500 / USTEC / NAS100. Architectural mismatch confirmed across two iterations.**

### Iteration 1 (`MAX_HOLD_SEC=60` constant from gold)

| Symbol | N | WR | PF | Net | TP_HIT | MAX_HOLD | TRAIL/REV |
| --- | --- | --- | --- | --- | --- | --- | --- |
| US500 | 6,263 | 38.2% | 0.44 | -446.31 | 37% | **62%** | 0/0 |
| USTEC | 7,740 | 51.4% | 0.52 | -1,798.85 | 51% | 49% | 0/2 |
| NAS100 | 902 | 68.1% | 0.77 | -81.87 | 66% | 31% | 0/0 |

Top configs all saturated TP at the LOW grid boundary; avg-hold pegged at 41-49s (close to the 60s ceiling). MAX_HOLD timeouts dominated. BE-arm never fired -> trail/reversal mechanism dead weight.

### Iteration 2 (`max_hold_sec` per symbol; gold=60, indices=300, FX=180-240)

Implemented by adding `max_hold_sec` to `SymbolSpec` + replacing the constant in `MicroScalperBase` with a `max_hold_sec()` accessor. Single-binary refactor, ~10 lines.

| Symbol | N | WR | PF | Net | Gross | MAX_HOLD% | TRAIL/REV |
| --- | --- | --- | --- | --- | --- | --- | --- |
| US500 | 2,240 | **69.9%** | 0.83 | -47.4 | **+10.0** | 28% | 0/2 |
| USTEC | 2,414 | **73.5%** | 0.81 | -269 | -91 | 26% | 0/3 |
| NAS100 | 814 | **92.4%** | 0.86 | -27 | -19 | 3% | 0/0 |

Massive WR lifts (32pp on US500, 22pp on USTEC, 24pp on NAS100). Gross flipped POSITIVE on US500. MAX_HOLD timeouts collapsed (62% -> 28% on US500; 31% -> 3% on NAS100). But **net still negative on all three.**

### Why indices fail even with full structural relief

**Root cause: spread/TP ratio is 2x gold's on this broker.**

| Symbol | Typical spread | Top-combo TP | spread/TP |
| --- | --- | --- | --- |
| XAUUSD | 0.22 | 0.79 | **28%** |
| US500 | 0.81 | 1.50 | 54% |
| USTEC | 2.71 | 6.30 | 43% |
| NAS100 | 1.20 | 2.25 | 53% |

Each non-TP exit costs ~half a TP win on indices vs ~quarter on gold. NAS100 winning 92% of the time and still losing money is the smoking gun -- the 8% loss tail at SL=36pt vs TP=2.25pt is too asymmetric for any winning rate to overcome at this spread level.

**TRAIL/REVERSAL fire counts stayed at 0-3 across all configs in both iterations.** Index price action is bimodal -- TP or SL, rarely the "halfway then reversion" pattern that gold's higher cadence produces. Without that pattern the engine's signature mechanism has nothing to work with regardless of MAX_HOLD setting.

### Decisions locked from S20 indices Phase 0

1. **Do NOT write `Sp500MicroScalperEngine.hpp` / `UstecMicroScalperEngine.hpp` / `Nas100MicroScalperEngine.hpp`.** The data over 1.3M-3.1M ticks per symbol is unambiguous.

2. **Do NOT continue iterating on per-symbol params for indices.** The structural spread/TP ratio is broker-determined; no sweep can tune around it.

3. **If indices coverage is needed in future** -- that's a *different engine architecture* (compression-bracket like the existing IndexHybridBracket, or session-anchored breakout). Not a MicroScalper duplicate. Pencil in for a future session.

4. **Files left in working tree from this iteration** (push pending):
   - `backtest/microscalper_crtp_sweep.cpp` -- now symbol-parameterized + per-symbol max_hold_sec; production-ready for FX phase as-is.
   - `backtest/sweep_microscalper_{US500,USTEC,NAS100}.csv` -- all three leaderboards (gitignored; for archive reference).
   - This handoff file.

---

## STEP 5 — Phase 0 plan: validate the edge per symbol BEFORE duplicating

Strategic premise (from S19 advisory): **don't blanket-duplicate the GoldMicroScalper across 10 symbols.** Mean-reversion fade behavior is asset- and session-specific. Validate first.

### Phase 0 deliverables

1. **Symbol-parameterized sweep harness.** Refactor `backtest/microscalper_crtp_sweep.cpp` to:
   - Read symbol from CSV filename (parse `l2_ticks_<SYMBOL>_<DATE>.csv`; default to "XAUUSD" for unprefixed legacy files)
   - Per-symbol constants table (point size, typical spread, USD_PER_PT, lot, session window)
   - Per-symbol output CSV: `backtest/sweep_microscalper_<SYMBOL>.csv`
   - Same 5-param pairwise grid (490 combos) per symbol
   - Per-symbol BaseParams (TP/SL/BE/TR/ENTRY_Z anchored to that symbol's ATR+spread profile, NOT XAUUSD's)

2. **Run sweeps on the symbols already captured:**
   - `data/l2_ticks_US500_*.csv` (15 days)
   - `data/l2_ticks_USTEC_*.csv` (15 days)
   - `data/l2_ticks_NAS100_*.csv` (3 days — barely enough; capture more before deciding)

3. **Decision gate per symbol** — proceed to Phase 1 (duplicate engine) only if leaderboard shows:
   - PF >= 2.5 at top combo
   - WR >= 75% at top combo
   - N >= 5,000 trades on the test tape
   - Top combo NOT at grid boundary on more than one parameter

4. **Extend `add_l2_logger.py` for FX symbols.** Tap `g_l2_books["EURUSD"]`, `["USDJPY"]`, `["GBPUSD"]`, `["AUDUSD"]`, `["NZDUSD"]`. Daily rotating CSVs at `C:\Omega\logs\l2_ticks_<SYM>_<DATE>.csv`. Deploy. Wait 2 weeks. Sweep.

### Symbol priority order (per the S19 advisory)

**Most likely to validate (sweep first):**
1. EURUSD (high cadence, mean-reverts in NY-lunch and Asia)
2. USTEC.F / NAS100 (NY core 14:00-19:00 UTC; deep L2; predictable micro-noise)
3. US500.F / SP500 (same logic, lower cadence than NAS)

**Less likely; validate before deploying:**
4. GBPUSD, USDJPY (often trending; existing breakout engines exist for a reason)

**Probably won't validate; don't deploy without strong sweep evidence:**
5. AUDUSD, NZDUSD off-hours
6. GER40, UK100, DJ30 (spread too wide for tight TP)

### What to refactor in microscalper_crtp_sweep.cpp

Current shape: hardcoded XAUUSD constants in `MicroScalperBaseParams`, hardcoded XAUUSD in TradeRecord. The refactor adds:

```cpp
struct SymbolSpec {
    const char* sym;
    double      typical_spread_pts;   // for sanity floor on TP
    double      usd_per_pt;           // tick value at LIVE_LOT
    double      live_lot;
    int         session_start_utc;
    int         session_end_utc;
    // Per-symbol BaseParams anchors -- chosen so TP/SL grids cover
    // sensible territory for that symbol's volatility profile.
    double      base_entry_z;
    double      base_tp;
    double      base_sl;
    double      base_be;
    double      base_tr;
};
```

Then a `SYMBOL_TABLE` with entries for XAUUSD, US500, USTEC, NAS100, EURUSD, GBPUSD, USDJPY, AUDUSD, NZDUSD. Lookup by parsed filename.

The CRTP traits need to become parameterized over both combo index `I` AND symbol spec, but since C++ template params must be compile-time, the cleanest path is one binary build per symbol (or compile-time macro switch) OR runtime parameterization that pulls multipliers from the SymbolSpec at sweep time. The latter loses some of the constexpr propagation benefit but keeps the binary single. Pick whichever is simpler to land first.

### Dependencies for FX phase

Before sweeping FX:
- `add_l2_logger.py` extension to capture FX L2 (deploy + 2 weeks of capture)
- News blackout integration (`OmegaNewsBlackout`) wired into the sweep simulation so news-window trades are excluded from leaderboard PnL
- USDJPY-specific point math (0.01 = 1 pip vs EUR/GBP/AUD/NZD's 0.0001)

---

## STEP 5.5 — Phase 0 FX plan (NEXT SESSION'S WORK)

**Pre-flight check: do FX L2 captures already exist on the VPS?**

```
ssh -p 2222 trader@185.167.119.59 \
    'powershell -Command "Get-ChildItem C:\Omega\logs\l2_ticks_*USD*.csv,C:\Omega\logs\l2_ticks_*JPY*.csv | Select-Object Name, Length"'
```

If output is non-empty -> FX captures exist; scp them down (`scp -P 2222 'trader@185.167.119.59:C:/Omega/logs/l2_ticks_*USD*.csv' ~/omega_repo/data/`) and skip to "Run sweep" below.

If output is empty -> FX L2 not yet captured. Extend `add_l2_logger.py` first (see "Logger extension" below).

### Two viable paths to FX validation

**Path A: Wait for live FX L2 capture (durable but slow).**
1. Extend `add_l2_logger.py` to tap `g_l2_books["EURUSD"]`, `["GBPUSD"]`, `["USDJPY"]`, `["AUDUSD"]`, `["NZDUSD"]`. Mirror the per-symbol prefix pattern already used for XAUUSD/US500/USTEC/NAS100.
2. Deploy via `.\OMEGA.ps1 deploy` on VPS.
3. Wait 2 weeks for capture.
4. scp to Mac, run sweep.

**Path B: Use existing Dukascopy historical FX tick data IMMEDIATELY (faster validation).**
The project has FX backtest harnesses at `backtest/eurusd_bt/`, `backtest/gbpusd_bt/`, `backtest/usdjpy_bt/`. These were built to validate the EurusdLondonOpenEngine cohort and pull from Dukascopy historical tick CSVs. Two approaches with this data:
1. Adapt `microscalper_crtp_sweep.cpp` to read the Dukascopy CSV format (different schema -- has tick volume but no L2; engine degrades to z-only entry, which is fine because indices showed even with L2=zero the spread/TP ratio is the binding factor).
2. Leverage the existing FX harnesses' tick loaders -- look at `backtest/eurusd_bt/`'s main TU for the parser pattern.

Pros of Path B: years of historical data available; no 2-week wait; can validate or kill the FX hypothesis in a day.
Cons: no L2 in the data so L2-confirmation gate untested; spreads in Dukascopy are interbank and may be tighter than BlackBull retail.

**Recommend: Path B first as a fast-fail check.** If the engine's edge doesn't survive on tighter Dukascopy spreads, it definitely won't on BlackBull. If it DOES survive, then Path A in parallel to validate against actual broker spreads.

### What you already know about FX from the indices result

The indices Phase 0 finding (spread/TP ratio drives everything) tells us up-front what to look for in FX:

| Pair | Typical retail spread | TP target needed (3.7x) | spread/TP |
| --- | --- | --- | --- |
| EURUSD | 0.0001 (1 pip) | 0.00037 (3.7 pips) | 27% |
| GBPUSD | 0.00015 (1.5 pips) | 0.00056 (5.6 pips) | 27% |
| USDJPY | 0.015 (1.5 pips JPY-scale) | 0.056 | 27% |
| AUDUSD | 0.00015 (1.5 pips) | 0.00056 | 27% |
| NZDUSD | 0.0002 (2 pips) | 0.00074 | 27% |

If retail FX spreads ARE in this range, the spread/TP ratio (~27%) is comparable to gold's 28% -- there's a chance. But cTrader / BlackBull often runs 1.5-2.5 pip spreads on the majors during normal hours, which would push the ratio to 40-50% and put us back in indices-territory. **Actual broker spread is the decider.** Path B's Dukascopy data won't reveal this -- only Path A's live capture will.

### FX-specific concerns (record these for the next session)

1. **News blackout is non-optional.** FOMC / NFP / CPI / ECB / BoE / BoJ all move FX violently. The MicroScalper has no news-blackout integration. Either add it to the sweep (read `OmegaNewsBlackout` config) or accept that news-window outliers will pollute the leaderboard.

2. **Session selection matters more than for indices.** EURUSD has materially different mean-reversion behaviour in NY-lunch chop vs London open vs Asian session. The current SymbolSpec session window (15-04 UTC) captures NY-lunch + Asia. May need to split into multiple session-restricted variants and sweep each independently.

3. **USDJPY pip math is 100x off.** 0.01 = 1 pip vs 0.0001 for the other USD-quote majors. Double-check the SymbolSpec entries before running USDJPY -- the current values are placeholder estimates, not calibrated.

4. **AUDUSD / NZDUSD off-hours cadence** may be too low for the 20-tick z-score window to compute meaningful statistics. If sweeps return mostly N=0 for these pairs, that's the cause -- consider lengthening ENTRY_LOOKBACK for those two specifically.

5. **Phase 0 gate is the same** -- PF >= 2.5, WR >= 75%, N >= 5,000, no boundary saturation on more than one parameter. Indices results suggest using a softer secondary gate of "gross PnL positive" as a sanity check before applying the main gate.

---

## STEP 6 — Outstanding decisions to ask the user (in priority order)

1. **Confirm shadow deployment of GoldMicroScalper.** Has `git push` from Mac happened? Has `.\OMEGA.ps1 deploy` run on VPS? If yes, check VPS log for `[MICRO-SCALPER-GOLD] FIRE` lines after first hour. Expected fire rate: ~1,300/day = ~80/hour during 06-22 UTC.

2. **Phase 0 scope.** User authorized "build on phase 0 ... do other symbols you recommend in sequence". Default sequence per S19 advisory: US500 -> USTEC -> NAS100 -> (after FX L2 capture) EURUSD -> USDJPY -> GBPUSD -> AUDUSD -> NZDUSD. Ask before deviating.

3. **FX L2 logger extension.** Required before FX phase can run. Two paths:
   - (a) Extend `add_l2_logger.py` to write per-FX-symbol files. Requires deploy + 2 weeks of capture before sweep.
   - (b) Skip FX entirely until indices validate or fail.
   Recommend (a) in parallel — capture starts now, runs in background, by the time indices are validated the FX captures are also ready.

4. **Outstanding S18 backlog (still tape-dependent):**
   - P1-1 LEDGER-CORRUPT-TS post-deploy grep
   - P1-4 IndexFlow WR investigation (needs >=200 trades since 2026-04-29 ship)
   - MAE_EXIT_RATIO retune cohort analysis
   - VWAPReversion EURUSD post-shadow re-tune

5. **Documentation cleanup** — `HANDOFF_S15_ENGINE_AUDIT.md`, `HANDOFF_S14_POST_CTRADER_CULL.md`, and `audit_input/` still untracked. Deferred since S15.

---

## STEP 7 — P1 backlog state (carry-forward)

Status as of end-S19 (no change from end-S18 except gold MicroScalper added):

| Item | Status | Notes |
| --- | --- | --- |
| P1-1 | Static trace done | Awaiting post-deploy tape. See `AUDIT_S16_P1_1_LEDGER_TRACE.md`. |
| P1-2 | CLOSED in S17 | Label fidelity fix. |
| P1-2-a | CLOSED in S17 | SIGINT/Ctrl+C shutdown handler at `omega_main.hpp:36`. |
| P1-3 | CLOSED in S17 | FxCascade cooldown. |
| P1-4 | Open | IndexFlow 29.4% WR vs 35% threshold. Replay-required. |
| P1-5 | CLOSED in S18 | TrendPullback consec-SL knobs to public member fields. |
| P1-6 | CLOSED in S18 | GER40 wiring documented. |
| P1-7 | CLOSED in S18 | BracketEngine CONFIRM_SECS already per-class asymmetric. |
| P1-8 | CLOSED in S17 | EsNqDivergence confirm-logic rewrite. |
| P1-9 | CLOSED in S18 | IndexSwingEngine no TP%; symmetric by design. |
| P1-10 | CLOSED in S18 | LiquiditySweepProEngine docstring. |
| P1-11 | CLOSED in S18 | MinimalH4Breakout CSV warm-load. |
| P1-12 | CLOSED in S18 | UTC-day cluster boundary doc-string fix. |
| VWAP-EURUSD-tune | CLOSED in S18 | Explicit MAX_EXTENSION_PCT=0.40, MAX_HOLD_SEC=600. |
| **GoldMicroScalper** | **CLOSED in S19** | **New engine end-to-end. Wired for shadow.** |

---

## STEP 8 — Files state at end of S19

### Modified, working tree (push pending)

```
modified:   include/globals.hpp
modified:   include/engine_init.hpp
modified:   include/tick_gold.hpp
new file:   include/GoldMicroScalperEngine.hpp
new file:   backtest/microscalper_crtp_sweep.cpp
new file:   FIX_SSH.ps1
new file:   HANDOFF_S20_AFTER_S19_MICROSCALPER.md  (this file)
```

### Untracked, deliberately not staged (carry-over)

```
?? HANDOFF_S14_POST_CTRADER_CULL.md
?? HANDOFF_S15_ENGINE_AUDIT.md
?? audit_input/
?? backtest/sweep_microscalper.csv               (gitignored)
?? backtest/microscalper_crtp_sweep              (gitignored binary)
?? data/                                          (gitignored)
?? logs/omega_2026-04-*.log                       (gitignored)
?? backtest/microscalper_gold_bt.py.bak          (the deprecated Python tombstone)
```

---

## STEP 9 — Sweep harness build & run reference

### Build

```
cd ~/omega_repo
clang++ -std=c++17 -O3 -DNDEBUG -fbracket-depth=1024 -I include \
    backtest/microscalper_crtp_sweep.cpp \
    -o backtest/microscalper_crtp_sweep
```

`-fbracket-depth=1024` is REQUIRED on Apple/clang -- the 490-element fold over `std::index_sequence<N_COMBOS>` exceeds clang's default 256 nesting limit. Same flag set by CMakeLists.txt for the cohort harnesses (line ~616 for OmegaSweepHarnessCRTP).

### Run (current state -- gold-only)

```
backtest/microscalper_crtp_sweep \
  data/l2_ticks_2026-04-{09,10,11,12,13,14,15,16,17,18,19,20,21}.csv \
  data/l2_ticks_XAUUSD_*.csv \
  --warmup 1000 --top 20 --verbose
```

Skips: `_part1.csv`/`_part2.csv` duplicates, the partial 838KB `2026-04-22.csv` (superseded by the 32MB XAUUSD-prefixed file).

### Sweep iterations to date

| # | Anchor `ENTRY_Z` | Grid range | Top combo (rk1) | Top PF | Top N |
| --- | --- | --- | --- | --- | --- |
| 1 | 1.5 | 0.75..3.0 | Z=0.75 TP=1.0 SL=2.38 | 3.05 | 34,781 |
| 2 | 0.75 | 0.375..1.5 | Z=0.38 TP=1.0 SL=2.38 | 3.27 | 42,277 |

Both saturated the LOW boundary on `ENTRY_Z`. Iteration 2's top combo (rk12, Z=0.75 / TP=0.79 / SL=3.0) was promoted to engine defaults instead of rk1 because of mid-grid robustness + highest PF (4.40) + highest WR (92.5%). See `include/GoldMicroScalperEngine.hpp` docstring lines 137-167 for the full rationale.

---

## STEP 10 — Explicit "do not" list (carry-over + S19 additions)

- Do NOT auto-commit. Do NOT auto-push. Always hand the user the exact zsh-friendly command sequence and let them run it.
- Do NOT use the user's GitHub PAT. Read context only.
- Do NOT modify core code without an explicit per-task go-ahead.
- Do NOT add documentation files (markdown, README) unless user asks. Audit reports, trace docs, and handoff docs are the exception.
- Do NOT propose backtests as P1 fixes.
- Do NOT touch `omega-terminal/node_modules/` or `build/`.
- Do NOT include leading `#` comment lines in shell command blocks delivered to the user (zsh, not bash).
- Do NOT flip `mode=LIVE` in `omega_config.ini` without explicit instruction.
- Do NOT promote MicroScalperGold to live without 2-week shadow validation matching backtest within 50% on N>=500 fills.
- Do NOT blanket-duplicate the MicroScalper to indices or FX. Validate the edge per symbol via Phase 0 sweep BEFORE writing engine code.
- Do NOT run `python -m http.server` style file-transfer hacks for VPS data; SSH key auth is the durable path. `FIX_SSH.ps1` solves the recurring auth-breaks problem.

---

## STEP 11 — One-line summary for next user message

After repo access is mounted and you've Read this handoff, tell the user:

> S20 closed: GoldMicroScalper shadow-deployed (rk12); harness made symbol-parameterized; Phase 0 INDICES (US500/USTEC/NAS100) ran iteration 1 (60s hold) then iteration 2 (300s hold) -- structural failure confirmed across both, indices stopped (spread/TP ratio is 2x gold's on this broker). Next: Phase 0 FX. Two paths -- (B) Dukascopy historical FX tick data for fast-fail check using the existing FX backtest harnesses' tick loaders, OR (A) extend `add_l2_logger.py` for live FX L2 capture and wait 2 weeks. Recommend B first as a fast-fail check, A in parallel. Pick path; check if any FX L2 captures already exist on the VPS (`scp listing` command in Step 5.5); confirm whether to add news-blackout to the sweep before starting.

Then proceed with the path the user picked.

---

## STEP 12 — Phase 0 implementation kickoff brief

When picking up Phase 0 from this handoff, the first concrete task is the symbol-parameterized sweep harness. Recommended sequence:

1. Read `backtest/microscalper_crtp_sweep.cpp` end-to-end (it's ~700 lines; do this once, not repeatedly).
2. Identify the points where XAUUSD-specific constants are baked in: `MicroScalperBaseParams`, `MicroScalperBase` constants block, `_close()` writing `tr.symbol = "XAUUSD"` / `tr.engine = "MicroScalperGold"`.
3. Decide between two refactoring shapes:
   - **(a) Single binary, runtime symbol param.** Add a `SymbolSpec` table; `params_for_combo` and `MicroScalperBase::on_tick` read from it. Loses some constexpr propagation but is one binary.
   - **(b) Per-symbol binary.** Compile-time macro selects symbol; build N binaries. Keeps constexpr but more build complexity.
4. Refactor + add the SYMBOL_TABLE entries for at least US500, USTEC, NAS100, XAUUSD.
5. Add filename-based symbol detection in `load_l2_csv`.
6. Test build and run a smoke sweep on US500.

The CSV files for indices already use the standard L2 schema (`ts_ms,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,depth_bid_levels,depth_ask_levels,depth_events_total,watchdog_dead,vol_ratio,regime,vpin,has_pos`) -- no parser changes needed.

For the per-symbol BaseParams anchors, start from these rough values (refined after first smoke-sweep):

```
                ENTRY_Z   TP_DIST   SL_DIST   BE_TRIGGER   TRAIL_DIST   MAX_SPREAD
XAUUSD          0.75      0.79      3.00      0.50         0.50         1.0
US500           0.75      0.30      1.20      0.20         0.20         0.50
USTEC           0.75      1.50      6.00      1.00         1.00         2.50
NAS100          0.75      1.50      6.00      1.00         1.00         2.50
EURUSD          0.75      0.0008    0.0030    0.0005       0.0005       0.0003
GBPUSD          0.75      0.0010    0.0040    0.0006       0.0006       0.0004
USDJPY          0.75      0.10      0.40      0.06         0.06         0.04
AUDUSD          0.75      0.0006    0.0024    0.0004       0.0004       0.0003
NZDUSD          0.75      0.0006    0.0024    0.0004       0.0004       0.0003
```

These are first-pass estimates: TP ≈ 3.6× typical spread (matching the gold ratio), SL = 4× TP, BE = 0.6× TP, TR = 0.6× TP. The pairwise geometric grid (0.5×..2.0×) then explores 4× variation around each anchor, which is enough surface area to find any real local optimum.

Good luck.
