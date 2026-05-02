# USDJPY Asian-Open Engine — Implementation + 14-Month Sweep Session

**Session date:** 2026-05-02
**Continues:** SESSION_2026-05-02_EURUSD_LONDON_OPEN_HANDOFF (S54-S56)
**Status:** Engine + 4-file wiring + sweep harness shipped. **14-month sweep finds NO profitable configuration.** Engine remains shadow-only by default; live promotion BLOCKED pending strategy rework.
**Branch:** `feature/usdjpy-asian-open` (off `omega-terminal`). Local commit only — not pushed.

---

## TL;DR

`UsdjpyAsianOpenEngine` ships as a 1:1 architectural port of `EurusdLondonOpenEngine` (S54-S56) with JPY pip math (1 pip = 0.01 price, USD_PER_PRICE_UNIT=100 at 0.10 lot) and a 00:00–04:00 UTC Asian-session window (Tokyo open + first 4 hours, pre-Frankfurt-handoff). Defaults inherit S55/S56 EURUSD constants as pre-sweep starting points.

A C++ tick-replay harness (`backtest/usdjpy_bt/UsdjpyAsianOpenBacktest.cpp`) and a Python sweep driver (`scripts/usdjpy_asian_sweep.py`) were built to mirror the EURUSD methodology. The sweep ran 41 single-axis cells plus 3 composites against 14 months of HistData.com USDJPY ticks (Mar 2025 – Apr 2026, 34.9M ticks). **Every configuration tested produced negative net PnL.** Best in-sample (`MIN_RANGE=0.15`, `MFE_TRAIL_FRAC=0.5`) yielded -$817 over 14 months at 72.5% WR. Out-of-sample on Oct 2025 – Apr 2026 was -$81 — better than train (-$745), so the result is not a curve-fitting artefact, but still loss-making even before realistic-broker haircuts.

The engine ships in shadow mode and stays there. **Do NOT promote to live.** This session's deliverable is the audit-quality conclusion that EURUSD's S54-S56 compression-breakout architecture does NOT transfer to USDJPY Asian session as-is.

---

## 1. Files shipped

| File | Lines | Status |
|---|---|---|
| `include/UsdjpyAsianOpenEngine.hpp` | 619 | New. 1:1 port of `EurusdLondonOpenEngine.hpp` with JPY pip math + 00–04 UTC session window |
| `include/globals.hpp` | +14 | Include + static instance after EURUSD pair |
| `include/engine_init.hpp` | +56 | shadow_mode + cancel_fn + register_engine + register_source |
| `include/tick_fx.hpp` | +69 / -7 | Split USDJPY out of shared `on_tick_audusd` into dedicated `on_tick_usdjpy` |
| `include/on_tick.hpp` | +5 / -2 | Symbol dispatcher: route USDJPY to `on_tick_usdjpy` |
| `backtest/usdjpy_bt/UsdjpyAsianOpenBacktest.cpp` | 350 | Standalone C++ tick-replay harness |
| `backtest/usdjpy_bt/UsdjpyAsianOpenEngine.hpp` | (copy) | Backtest-side copy mutated by sweep script. Production header is never touched |
| `backtest/usdjpy_bt/{OmegaTradeLedger,SpreadRegimeGate,OmegaNewsBlackout}.hpp` | ~30 each | Minimal stubs so engine compiles standalone (real headers depend on Windows-only `_mkgmtime` and the production `g_macroDetector`) |
| `scripts/usdjpy_asian_sweep.py` | 235 | Sweep driver: rewrites parameters via regex, recompiles, runs, aggregates |
| `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_HANDOFF.md` | (this) | Handoff |

Engine and wiring follow the EURUSD pattern exactly. Engine count: 16 → 17. Position sources: 3 → 4.

---

## 2. Audit lineage

USDJPY was previously handled by the shared `on_tick_audusd` template along with AUDUSD/NZDUSD (per the 2026-04-06 FX disable). The shared handler did one thing for USDJPY: store `g_usdjpy_mid` for `tick_value_multiplier` (live JPY/USD rate conversion, used by sizing). No engine dispatch.

This session's wiring:
- Splits USDJPY out into a dedicated `on_tick_usdjpy` template (mirrors `on_tick_eurusd`).
- Moves the `g_usdjpy_mid` store into the new function (so it still fires every tick).
- Wires `g_usdjpy_asian_open` as the engine target.
- Updates the dispatcher in `on_tick.hpp` to route USDJPY to `on_tick_usdjpy`.

`AUDUSD/NZDUSD` remain on the now-USDJPY-free shared handler. No orders are sent from any of the four FX inert handlers (GBPUSD, AUDUSD, NZDUSD, and the AUDUSD/NZDUSD shared body).

DOM fields:
- `g_macro_ctx.jpy_l2_imbalance`, `jpy_microprice_bias`, `jpy_vacuum_ask`, `jpy_vacuum_bid` — present and populated.
- `jpy_wall_above` / `jpy_wall_below` / `jpy_slope` — absent. The dispatcher passes `false` / `0.0`; engine wall and slope branches are naturally inert.
- `l2_real` wired to `g_macro_ctx.ctrader_l2_live`.

News blackout: `g_news_blackout.is_blocked("USDJPY", now_s)` consulted at IDLE→ARMED. USDJPY is in both USD (NFP/CPI/FOMC) and JPY (BoJ) symbol sets per `OmegaNewsBlackout.hpp:392-397`.

---

## 3. Parameter envelope (pre-sweep)

JPY pip math (1 pip = 0.01 price; values are EURUSD S56 × 100):

| Constant | EURUSD S56 | USDJPY (this session) | Notes |
|---|---|---|---|
| `MIN_RANGE` | 0.0008 | **0.08** | 8 pips |
| `MAX_RANGE` | 0.0050 | **0.50** | 50 pips |
| `SL_FRAC` | 0.80 | **0.80** | dimensionless |
| `SL_BUFFER` | 0.0002 | **0.02** | 2 pips |
| `TP_RR` | 2.0 | **2.0** | dimensionless |
| `TRAIL_FRAC` | 0.30 | **0.30** | dimensionless |
| `MIN_TRAIL_ARM_PTS` | 0.0006 | **0.06** | 6 pips |
| `MIN_TRAIL_ARM_SECS` | 30 | **30** | seconds |
| `MFE_TRAIL_FRAC` | 0.40 | **0.40** | dimensionless |
| `BE_TRIGGER_PTS` | 0.0006 | **0.06** | 6 pips |
| `SAME_LEVEL_BLOCK_PTS` | 0.0008 | **0.08** | 8 pips |
| `SAME_LEVEL_POST_SL_BLOCK_S` | 1200 | **1200** | 20 min |
| `SAME_LEVEL_POST_WIN_BLOCK_S` | 600 | **600** | 10 min |
| `MAX_SPREAD` | 0.00020 | **0.02** | 2 pips |
| `COOLDOWN_S` | 120 | **120** | seconds |
| `USD_PER_PRICE_UNIT` | 10000.0 | **100.0** | sizing math; real PnL via `tick_value_multiplier` (100000/g_usdjpy_mid) |
| `LOT_MIN` / `LOT_MAX` | 0.01 / 0.20 | **0.01 / 0.20** | half-Kelly cap (PRE-SWEEP) |
| `SESSION_START_HOUR_UTC` | 6 | **0** | Tokyo open (09:00 JST) |
| `SESSION_END_HOUR_UTC` | 9 | **4** | pre-Frankfurt handoff |

`STRUCTURE_LOOKBACK = 600` carried over but flagged: Asian USDJPY tick rate is materially lower than London EURUSD, so 600 ticks ≈ 6–12 min in Asia vs ~3 min in London. Sweep candidate set noted in code: {300, 400, 600, 900}.

---

## 4. Backtest harness

`backtest/usdjpy_bt/UsdjpyAsianOpenBacktest.cpp` is a standalone tick-replay binary:

- Parses HistData.com ASCII format (`YYYYMMDD HHMMSSmmm,bid,ask,vol`) with a hand-rolled fast parser.
- Iterates monthly subdirectories chronologically.
- Streams ticks through one `UsdjpyAsianOpenEngine` instance.
- Records every closed trade to `bt_usdjpy_trades.csv` with raw, gross-USD and net-USD PnL columns.
- Prints summary stats to stderr: trade count, exit-reason breakdown, WR, PF, DD, profitable-month count.
- Net-USD applies a cost model of 0.2 pips slippage per side + $0.20/lot per-side commission. This matches the implicit cost assumption used in the EURUSD S55/S56 sweeps.

Sustained throughput: **4.5M ticks/sec** on the sandbox VM (14 months / 34.9M ticks in 7.8s wall).

Build:
```
g++ -std=c++17 -O3 -Wno-unused-parameter -DOMEGA_BACKTEST \
    -I backtest/usdjpy_bt \
    backtest/usdjpy_bt/UsdjpyAsianOpenBacktest.cpp \
    -o build/usdjpy_asian_bt -pthread
```

Run:
```
./build/usdjpy_asian_bt --ticks ~/Tick/USDJPY \
    --from 2025-03 --to 2026-04 \
    --trades bt_trades.csv --report bt_report.csv \
    --label baseline
```

---

## 5. Sweep harness

`scripts/usdjpy_asian_sweep.py` drives multi-config exploration:

- `single` — single-axis sweep over 8 axes (41 cells total). Resumable: skips labels already in the results CSV. Optional integer arg = max new cells to run.
- `run <label> KEY=VAL ...` — single config full 14-month run.
- `oos KEY=VAL ...` — split train/test (Mar–Sep 2025 / Oct 2025 – Apr 2026) using one config.

For each cell, it:
1. Reads `include/UsdjpyAsianOpenEngine.hpp` (the production header).
2. Regex-substitutes the relevant `static constexpr` values into a tmp copy at `backtest/usdjpy_bt/UsdjpyAsianOpenEngine.hpp`.
3. Recompiles the harness against the tmp copy.
4. Runs the harness on the staged ticks.
5. Parses the stderr summary and appends one row to `build/usdjpy_sweep_results.csv`.

The production engine header is never modified by the sweep — the scope of every change is the bt copy.

Per-cell wall time (compile + run) ~9–10s in the sandbox.

---

## 6. Sweep results — single-axis (41 cells)

Full 14-month in-sample sweep, all configurations. Top 10 by net PnL:

| label | trades | WR | PF | net PnL | DD | prof months |
|---|---:|---:|---:|---:|---:|---:|
| **MIN_RANGE=0.15**       | 832  | 72.1% | 0.83 | **-$917**  | $1038 | **6/14** |
| **MIN_RANGE=0.12**       | 1340 | 70.4% | 0.88 | **-$966**  | $1166 | 2/14 |
| BE_TRIGGER_PTS=0.04      | 2394 | 50.3% | 0.89 | -$1077 | $1206 | 3/14 |
| MIN_RANGE=0.06           | 2083 | 65.7% | 0.91 | -$1085 | $1144 | 4/14 |
| MFE_TRAIL_FRAC=0.5       | 1940 | 66.7% | 0.89 | -$1250 | $1301 | 4/14 |
| SL_FRAC=1.0              | 1868 | 70.4% | 0.88 | -$1317 | $1325 | 3/14 |
| MFE_TRAIL_FRAC=0.6       | 1931 | 66.4% | 0.88 | -$1359 | $1408 | 3/14 |
| SL_FRAC=0.9              | 1918 | 68.4% | 0.88 | -$1368 | $1387 | 5/14 |
| SAME_LEVEL_BLOCK_PTS=0.12| 1860 | 66.1% | 0.87 | -$1392 | $1400 | 3/14 |
| TP_RR=2.5                | 1959 | 66.2% | 0.87 | -$1404 | $1430 | 4/14 |

baseline (S56-inherited): trades=1959, WR=66.1%, PF=0.87, **PnL=-$1442, DD=$1473, profitable months 4/14**.

### Key observations

- **TP_RR is essentially inert.** Only 10 of 1959 baseline trades exit via TP_HIT — the breakouts rarely follow through far enough on USDJPY Asian session. Almost all exits are TRAIL_HIT or SL_HIT.
- **Exit signature matches EURUSD S56:** TP_HIT≈0, BE_HIT≈0 (BE-trigger fires simultaneously with trail-arm at MFE=6 pips), so trades transition cleanly between SL → BE → TRAIL. The architecture works; the *signal* doesn't.
- **MIN_RANGE is the single most impactful axis.** Widening the minimum compression from 8 → 12 → 15 pips drops trade count, raises WR, and sharply reduces total losses. Asia-session compression-breakouts in the 8–10 pip band are dominated by chop.
- **Higher SL_FRAC is consistently better** (0.5 → 1.0 monotonic improvement on PnL and WR). The classic EURUSD S56 finding holds — SL placed too close gets wicked out before trail-arm.
- **BE_TRIGGER=0 is a disaster** (-$6946 / 3.1% WR). The trail-arm + BE-lock combo is doing real work — without it the engine takes maximum loss on every reversed move. Confirms the value of the EURUSD S55/S56 BE-lock pattern.
- **Asymmetric loss profile is the killer.** Across all configs, average win sits at ~$7.50 and average loss at ~$15–25 (depending on SL_FRAC). At 66% WR, expectancy is barely positive at zero costs and clearly negative once costs are subtracted. The wider SL_FRAC=1.0 lifts WR to 70.4% but also widens average loss to $20.15, so expectancy doesn't improve much.

### Composite tries

Per the EURUSD S56 lesson ("never trust naive AND-of-winners"), three small-axis composites were tested:

| label | trades | WR | PF | net PnL | DD | prof months |
|---|---:|---:|---:|---:|---:|---:|
| MIN_RANGE=0.15 + MFE_TRAIL_FRAC=0.5 (`min_only_0.15`) | 811 | 72.5% | 0.84 | **-$817** | $978 | **7/14** |
| MIN_RANGE=0.15 + BE_TRIGGER_PTS=0.04 + MFE_TRAIL_FRAC=0.5 | 918 | 53.3% | 0.83 | -$756 | $848 | 4/14 |
| MIN_RANGE=0.15 + SL_FRAC=0.9 + BE_TRIGGER_PTS=0.04 | 929 | 53.8% | 0.79 | -$1002 | $1057 | 4/14 |

`MIN_RANGE=0.15 + MFE_TRAIL_FRAC=0.5` is the best in-sample champion: -$817 PnL, 72.5% WR, 7/14 profitable months. Drawdown reduced 34% vs baseline ($978 vs $1473), but still net negative.

Lower BE_TRIGGER drops WR sharply (lots of BE_HIT exits replacing what would have been larger wins or losses), confirming the EURUSD lesson that BE_TRIGGER == MIN_TRAIL_ARM_PTS is the right alignment.

### Out-of-sample validation

Best champion (`MIN_RANGE=0.15 + MFE_TRAIL_FRAC=0.5`) split:

| Period | trades | WR | PF | net PnL | DD | prof months |
|---|---:|---:|---:|---:|---:|---:|
| TRAIN (2025-03..2025-09, 7 mo) | 481 | 71.5% | 0.77 | -$745 | $929 | 3/7 |
| **TEST/OOS** (2025-10..2026-04, 7 mo) | 331 | **74.0%** | **0.96** | **-$81** | $281 | **4/7** |

OOS performance is *better* than train, not worse — the opposite of a curve-fitting signature. Late-2025 / early-2026 USDJPY Asian-session is closer to break-even than early-2025. But still negative.

---

## 7. Conclusion: engine NOT deployable as-is

**Live promotion is BLOCKED.**

Every configuration tested over the 14-month sample produces a net loss. The best in-sample champion loses $817 (5.8% of a $14k account at 0.20 lot) over 14 months; the OOS half loses $81 (still negative). Realistic broker conditions (haircut applied as 10–20% in the EURUSD work) would worsen this further.

The engine itself is correct — its exit-reason signature, pip math, session window, news blackout, BE-lock, same-level block, and DOM dispatch all behave as designed and mirror the EURUSD reference. What does NOT work is the **strategy assumption** that EURUSD's London-open compression-breakout shape transfers to USDJPY Asian-session.

Possible reasons (hypotheses; not validated this session):

1. **Asian-session USDJPY is structurally different.** Tokyo session is dominated by exporter / repat flows that produce mean-reverting moves rather than the trend-continuation breakouts that work for London EURUSD. The compression-then-breakout assumption may simply be wrong for this regime.
2. **Tick density mismatch.** USDJPY Asian has ~50–100 ticks/min (vs 150–250 for London EURUSD), so `STRUCTURE_LOOKBACK=600` covers 6–12 minutes here vs 3 in London. The engine may be picking up too-stale compressions. Worth retesting with `STRUCTURE_LOOKBACK ∈ {200, 300, 400}`.
3. **JPY round-number / Tokyo-fix dynamics.** USDJPY chops around major levels (00, 50) and gets pinned around the 09:55 Tokyo fix (00:55 UTC) — both of which fall *inside* this engine's session window. The `MIN_RANGE=0.15` improvement (filtering out the 8–12 pip chop bands) supports this.

---

## 8. Open follow-ups for next session

If the user wants to make USDJPY work, candidate next steps in priority order:

1. **Sweep `STRUCTURE_LOOKBACK` ∈ {200, 300, 400, 600, 900}.** Not in this session's sweep (the script's BASELINE includes it but the SWEEPS list doesn't). Two minutes of compute. May show a tick-rate-aligned sweet spot.
2. **Restrict the session window.** Try `01:00-04:00 UTC` (skip the first hour after Tokyo open which is choppiest), `02:00-04:00 UTC`, `00:00-02:00 UTC`. Asian session has known sub-window edge profiles.
3. **Test a Tokyo-fix exclusion.** Block arming during 00:50–01:00 UTC (the 09:55 JST fix window).
4. **Mean-reversion variant.** Inverse signal: when range exceeds `MIN_RANGE`, fade rather than break. Asian session may favour mean-reversion.
5. **Try the engine on USDJPY *London* session** (06:00-09:00 UTC). If the architecture is correct and only the regime is wrong, USDJPY in London hours might trade like EURUSD does.
6. **Try the engine on USDJPY NY session** (13:00-16:00 UTC) for the same reason.

Track 5 and 6 are the cheapest tests — same engine, different session window constants. ~2 minutes each.

---

## 9. Verification queries (post-deploy, when applicable)

PowerShell on Windows VPS — same pattern as EURUSD verification:

```powershell
# Engine count: should jump from 16 -> 17
Select-String -Path C:\Omega\logs\latest.log -Pattern "g_engines registered \(17 engines\)"

# Position-source line: should now show 4 sources
Select-String -Path C:\Omega\logs\latest.log -Pattern "g_open_positions sources registered \(4 sources"

# Diag heartbeat: should appear every 600 ticks during Asian session
Select-String -Path C:\Omega\logs\latest.log -Pattern "JPY-ASN-OPEN-DIAG.*window=\d+/600" | Select-Object -Last 5

# News blackout integration
Select-String -Path C:\Omega\logs\latest.log -Pattern "NEWS-BLACKOUT.*USDJPY" | Select-Object -Last 10

# Shadow trades to ledger (since shadow_mode = true, all trades will be shadow)
Select-String -Path C:\Omega\logs\latest.log -Pattern "TRADE-COST.*USDJPY" | Select-Object -Last 10
```

In the GUI, with shadow trades only:
- LDG panel filter `UsdjpyAsianOpen` should show entries within first Asian session post-deploy.
- TradeBook entries with `engine="UsdjpyAsianOpen"`, `regime="ASN_COMPRESSION"`.
- Engine status panel should show 17 engines (was 16).

---

## 10. Promotion gate (UNCHANGED — not currently passable)

If a future tune produces a profitable USDJPY config, gates carry over from EURUSD pattern:

1. ≥30 closed shadow trades.
2. WR ≥ 60% (stricter than EURUSD's 35% to align with the high-WR/low-edge signature this engine produces).
3. Net positive expectancy after costs (spread × 2 + commission per leg + 0.2 pip slip per side).
4. No SL clusters in same-level-block radius (validates the block).
5. Asian-session activity confirmed (most fires should be 00:00–04:00 UTC).
6. Live promotion = single-line edit in `engine_init.hpp` flipping `g_usdjpy_asian_open.shadow_mode = true;` to `false;` (or to `kShadowDefault`).

---

## 11. Outstanding security flag (carry-forward from EURUSD session)

The user's global `CLAUDE.md` still contains a GitHub PAT in plaintext (`ghp_9M2I…24dJPV4`). Rotate at `github.com/settings/tokens` and replace with a Keychain reference or remove the line entirely. Unrelated to this work but worth resurfacing every session until rotated.

---

End of handoff doc.
