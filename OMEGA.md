# OMEGA.md — Master System Reference

Single source of truth for the Omega trading system. **Read this before any
data/backtest/deploy work.** If you discover something not here, ADD IT.
Companion docs: `CLAUDE.md` (repo edit/build/deploy rules),
`backtest/ENGINE_BACKTEST_REGISTRY.md` (per-engine backtest wiring + traps).

Created 2026-06-15 after repeated time-wasting re-derivation of data locations,
engine wiring, and costs.

---

## 1. DATA INVENTORY — `/Users/jo/Tick/`

**Check here FIRST. More data usually exists than you remember.** Run every file
through `backtest/data_integrity_gate.py` before use (catches ×1000 glitches,
column swaps, gaps).

| Instrument | What's in Tick | Coverage / regimes | Format |
|---|---|---|---|
| **XAUUSD** | `duka_ticks/XAUUSD_2024-03_2026-04_combined.csv` (4.6G); `xau_6mo_corrected.csv` (Nov25-Apr26, col-fixed); `xau_6mo_ds10.csv` (10x downsample) | 2024-03 → 2026-04 (bull-ish) | combined is `ts,ask,bid` (ASK FIRST trap) |
| **US500 (SPXUSD)** | `SPXUSD/HISTDATA..T<YYYYMM>/` **2024-01 → 2026-04** + 2022-bear chunk in `duka_multiyear/` (Jan-Jun 2022) | **multi-year + 2022 bear** ✅ | histdata `YYYYMMDD HHMMSSmmm,bid,ask` |
| **NAS100 (NSXUSD)** | `NSXUSD/` + `Nas/` **2024-01 → 2026-04** + 2022-bear chunk (Jan-Apr 2022) + `NAS2022_bear_h1.csv` | **multi-year + 2022 bear** ✅ | histdata |
| **DJ30 (USA30)** | `duka_ticks/USA30IDXUSD_2025_10..2026_04` (Mar gap filled) | 6mo only; some ×1000-glitched (cleaned: `book_combined/DJ30_clean.csv`) | `ts,ask,bid` 5-col |
| **GER40 (GRXEUR)** | `histdata_book/GER40/` zips 2025-11 → 2026-05 | **6mo ONLY — no 2022/2024** ❌ cross-regime blocked | histdata |
| **UK100 (UKXGBP)** | `histdata_book/UK100/` 2025-11 → 2026-05 | **6mo ONLY** ❌ | histdata |
| **FX** EUR/GBP/EURGBP/USDJPY/AUD/NZD/CAD | `<SYM>/HISTDATA..` mostly 2025-2026; GBPUSD has 2022 H2 zips at Tick root | 2025-26 (thin) | histdata |
| **BCOUSD** (oil) | 2025-2026 | | histdata |

**Pre-built combined sets:**
- `book_combined/<INST>.csv` — Nov2025-Apr2026 per instrument (the 6mo book run).
- `xregime/US500_2426.csv`, `xregime/NAS100_2426.csv` — **2024-2026 multi-year** (histdata-only, uniform format). Use WITH the 2022-bear chunks (run separately — format differs) for cross-regime.
- 2022-bear: `duka_multiyear/usa500idxusd-...T03-56.csv` (Jan-Jun 2022), `usatechidxusd-...T04-25.csv` (Jan-Apr 2022). `ts,ask,bid`.

**NEVER merge histdata (`YYYYMMDD`) with duka (`ts,ask,bid`) into one file** — the
loader sniffs ONE format. Run separately, aggregate after.

**To get more data:** `dukascopy-node` (operator runs on Mac, fast/parallel) —
but it throttles past the first year-chunk; pulls beyond early-2022 mostly FAILED
2026-06-15. HISTDATA zips (operator downloads) are the reliable index source:
GRXEUR=DAX, UKXGBP=FTSE, ETXEUR=EuroStoxx50, SPXUSD=S&P, NSXUSD=Nasdaq. **HISTDATA
has NO Dow Jones** (UDXUSD=Dollar Index, not DJ30 — use duka USA30IDXUSD).

## 2. BACKTESTING — **READ `backtest/BACKTEST_TRUTH.md` FIRST** + `backtest/ENGINE_BACKTEST_REGISTRY.md`

- **BACKTEST_TRUTH.md is the protocol that stops the tombstone oscillation** (engines killed,
  re-mined "in another guise", re-flagged, re-falsified — burning tokens). Order: **ledger first
  → reproduce the kill → engine-faithful tick BT gates deploy → bar-replay is a discounted hint,
  never a verdict.** Bar-replay harnesses OVERSTATE ~0.5-0.7 PF (proven: FVG fvg_core PF1.65 →
  faithful PF0.95 → live-loss). Deploy decisions gate ONLY on the faithful arbiter
  (`faithful_engine_bt_TEMPLATE.cpp` — drives the REAL engine class). `TOMBSTONE_AUDIT.md` lists
  which dead engines were killed on polluted/bar-replay numbers (RE-CHECK) vs faithful (stay dead).

- Harness: `backtest/ShadowBook_mi.cpp` → `clang++ -O2 -std=c++20 -DOMEGA_BACKTEST -Iinclude -o /tmp/ShadowMI backtest/ShadowBook_mi.cpp`. Run: `/tmp/ShadowMI <datafile> <out.csv> <INSTRUMENT>`.
- Gate: `backtest/data_integrity_gate.py <file>` — MANDATORY before use.
- Aggregator: `/tmp/agg_full.py` (per-instrument tick_value_multiplier + cost).
- **Two pre-flight rules:** (1) only test engines `enabled=true` in `engine_init.hpp`; (2) gate every data file.
- **The biggest trap:** engines need their init call (`init_default_cells()` / `.init()`) or they produce 0 trades and look dead. See registry.

## 3. COSTS — see memory `omega-ibkr-real-costs.md`

XAU spot = **1.5 bps/side commission + measured spread** (~$1.6/oz RT @ current gold;
the old `0.37` constant was ~4× too low). Index/FX: tick_value_multiplier per
`include/sizing.hpp` (XAU=100, US500=50, USTEC=20, DJ30=5, NAS100=1, GER40=1.10€,
UK100=1.33£, FX=100000). Measured spreads: NAS 1.15, US500 0.53, UK100 1.23,
DJ30 2.09, GER40 1.47, EURGBP 1pip.

## 4. LIVE SYSTEM

- **VPS:** ForexVPS "Edge" package, `VM16893535923733529.forexvps.net`, IP `45.85.3.79`, `C:\Omega\`. NSSM service "Omega".
  Migrated 2026-07-07 from the old 4GB box at 185.167.119.59 (memory pressure — the 2026-06-27 RDP freeze was RAM thrash; new box has more RAM + extra CPUs).
  - **RDP:** `45.85.3.79:42014` (provider-assigned port — must be included when connecting), user `trader`. Password in the operator's password manager — NEVER in this repo.
  - **SSH (from Mac):** `ssh -p 2222 trader@45.85.3.79`. Port 2222 is OUR convention, not the provider's: OpenSSH for Windows must be installed on the new box, listening on 2222, firewall opened — rerun `FIX_SSH.ps1` for key auth. Until that's done, all Mac-side ssh/scp tooling (deploy, rdagent pulls, L2 fetches) is dead.
- **Deploy:** `ssh ... "cd C:\Omega && powershell -File OMEGA.ps1 deploy [-Fast]"` (branch-guarded, auto-recovery). Verify: stderr `Git hash:` == `git rev-parse origin/main`.
- **Shadow ledger (real engine performance, logged):** `C:\Omega\logs\trades\omega_trade_closes.csv` (+ daily, + `shadow\omega_shadow.csv`). Clean slate: `GET /api/clear_ledger`.
- **GUI:** embedded `include/OmegaIndexHtml.hpp` (regen from `tools/gui/omega_desk.html` via `tools/gui/gen_index_html.py`). PR chart + LIVE OPEN TRADES + shadow equity.
- Source ≠ deployed: tombstones only go live after rebuild+deploy.

## 5. ENGINE STATUS (2026-06-15) — see memory `omega-2026-06-15-book-viability-cull.md`

- **VIABLE (kept):** XauTrendFollow 1h/2h/4h/D1, XauStraddleM30, GoldSeasonal, GoldPanicBounce, XauTurtleD1, XauDojiRejD1, XauOutsideBarD1, GoldVolBreakoutM30, XauSessNYpm (all XAU, net+ on 6mo).
- **SurvivorPortfolio:** RESTORED (S38 walk-forward validated; net ~+$1,300/6mo). Donchian/ZMR cells win; XAU_4h_MA + GER_15m_MA cells are losers (cell-level cull candidates). Weekend-hold bug FIXED (config.hpp `_4h` exemption exclusion).
- **TOMBSTONED on 6mo-bull (REVISIT cross-regime):** index engines (idd, IndexBearShort, nas_orb_retrace, us30_ensemble, minimal_h4_us30, ustec_tf_htf) + gold (AdaptiveHull, Supertrend, SessOvernight, BreakBounce, Tsmom). **NasOrbRetrace proven +$390 in 2022 bear vs −$708 bull → it's a BEAR engine, wrongly culled on bull-only data.** Cross-regime re-validation in progress.
- **FLAGGED (not culled, inconclusive 6mo):** GER40 Keltner/London/MinimalH4, FX amr/xrev.
- **FX TURTLES CULLED 2026-06-16** (EurusdTurtleH4 + GbpusdTurtleH4): recheck on more data (backtest/fx_turtle_recheck.cpp). GBPUSD 18mo CROSS-REGIME (2022 H2 bear + 2025-26) PF0.88, no improvable config (sweep best = flat noise PF1.01); EURUSD PF1.31 single-regime only (no EURUSD bear data) → identical-param sibling failing cross-regime = regime-luck. Operator bar "marginal not good enough". Live FX book = EurGbpPairs (strong) + FxCrossRev + AMR.

**CROSS-REGIME AUDIT (2026-06-15, 2022-bear + 2024-2026 data):**
- **SurvivorPortfolio** — cull was WRONG, RESTORED. USTEC cells +~$18k cross-regime. (init needs `init_default_cells()`.)
- **PeachyOrb** — cull CORRECT (confirmed). Full lever sweep: brilliant in isolated 2022-bear (PF3+) but EVERY config PF<1.1 full-span. Discretionary edge doesn't mechanize cross-regime.
- **NasOrbRetrace** — cull correct. +$390 bear but −$1,361 bull → net −$971 cross-regime.
- **IndexIntradayDrift** — cull correct (marginal gross +, negative after cost over multi-year).
- **IndexBearShort** — RE-INSTATED (cull WRONG, 2nd reversal). Full 2022 (incl sustained H2 crash) + 2024-26: US500 +$14.7k gross / NAS +$788. Genuine bear hedge (idle in bull, strong in bear).
- **NasTurtleD1** — still INCONCLUSIVE: full-2022 retest n=24, −$15 flat. Validated on 10yr Yahoo daily; tick test underpowered (2022-05→2023 data gap). Keep flagged. Need multi-year daily.

**LESSON (hard, repeated): never cull on a single-regime slice.** 6mo Nov25-Apr26
= gold bull; it punishes bear engines (index shorts) and flatters trend. Always
cross-regime (2022 bear) before tombstoning.

**GER40/UK100 + daily-turtle cross-regime (2026-06-15, Yahoo daily 2016-2026 incl 2022 bear, Donch20/ema100 long-only):** DJ30 PF2.10 (+13267pt), NAS100 PF2.69, SPX PF2.52 — STRONG daily trend edge → a DAILY-TURTLE FLEET (DJ30+SPX+NasTurtle) is a viable cross-regime trend book (Yahoo-validatable). GER40 PF1.14 (marginal), UK100 PF0.80 (NO edge). UK100 closed (idd culled, no trend edge). GER40 flagged H1/H4 engines (Keltner/MinimalH4/London) stay shadow — need intraday 2022-2024 to settle; daily proxy = thin edge. DATA SOURCE FIX: Yahoo Finance for daily multi-year (^GDAXI/^FTSE/^GSPC/^DJI/^NDX) — free/instant, ditch Dukascopy for daily.

**GER40/UK100 H4 cross-regime UPDATE (2026-06-15, Yahoo 1h->H4, 2024-06..2026 = 2yr):** H4 turtle (Donch20/ema100) GER40 PF1.36 (+1774 n=25), UK100 PF1.81 (+933 n=24). BOTH have a positive H4 trend edge — UK100 has one at H4 even though DAILY missed it (PF0.80). Reverses "UK100 closed": UK100 = H4-edge candidate. GER40 MinimalH4 flagged engine SUPPORTED. Caveats: thin n, Yahoo 1h=bar-close approx (no tick fills/cost), 2yr only (Yahoo 1h cap ~730d, can't reach 2022 bear). Directional, not deploy-grade. YAHOO INTRADAY LIMITS: 1h=~730d, 5/15/30m=60d, 1m=7d, daily=full history. For pre-2024 intraday -> HISTDATA/paid source.

**GER40/UK100 — DROPPED 2026-06-15 (operator: not viable, not pursuing).** GER40 H4 turtle FAILED both-halves (H2 neg); UK100 H4 marginal/thin; live GER40 engines were losing. Culled (kelt/london/minimal_h4 off; UK100 idd off). NOT worth chasing 2022-2023 intraday for marginal edges. Do not re-chase without a strong new hypothesis. Index trend edge lives in DJ30/SPX/NAS daily turtles (built, live-shadow) — not the EU indices intraday.


---

## 6. CONTINUOUS IMPROVEMENT LOOP (added 2026-06-16)

**Standing rule: the book is never "done" — every session that touches engines
runs the analytics review and acts on the ranked flags.** The ledger captures rich
per-trade data (mfe, mae, slippage, commission, regime, hold, broker fills); the
job is to keep mining it for fixable leaks.

**Mechanism:**
- `tools/analytics/ledger_analytics.py <ledger.csv>` — omnibus per-engine + book
  diagnostics: capture ratio (exit tightness), expectancy (R), payoff, MAE-p90
  (stop placement), cost% of gross (friction), $/hr (hold efficiency), max-consec-loss,
  per-regime split, engine-return correlation (hidden concentration), broker
  reconciliation, book Sharpe/Sortino/Calmar/maxDD/Ulcer. Emits RANKED FLAGS.
- `tools/analytics/capture_ratio.py` — focused capture-ratio view.
- `tools/analytics/run_review.ps1` — VPS runner; pools cumulative+daily+shadow
  closes, writes `logs/analytics/review_<date>.txt`, surfaces flags to the service
  log. Scheduled WEEKLY (task `OmegaWeeklyReview`).

**Act on flags (the to-do this generates):**
| Flag | Meaning | Action |
|---|---|---|
| LOOSE-EXIT (capture<0.35) | banks <35% of the favorable move | tighten trail / rework TP |
| NEG-EXPECTANCY (expR<0) | no edge | cull / redesign |
| COST-FRAGILE (cost>40% gross) | friction eats the edge | drop or move to cheaper venue/TF |
| REGIME-DEPENDENT | loses in one regime | add a regime gate (see BigCapMomo SPY-200MA pattern) |
| BROKER-MISMATCH | engine pnl != broker pnl | fill/phantom audit |
| BAD-PAYOFF | small wins, big losses | fix R:R |

**ML loss mining (added 2026-07-18):** `tools/ml_loss_miner/mine_losses.py` — complements the
fixed-metric analytics above with LEARNED pattern discovery (decision tree + groupby fallback for
small n) across BOTH Omega and Chimera ledgers: which exit_reason/regime/weekday/symbol
combinations predict a loss, per engine. Outputs `outputs/ML_LOSS_MINING_REPORT_<date>.md` with a
PROPOSED mitigation per surfaced pattern — proposals only, never auto-wired; every one still needs
a certified backtest before touching config/code. Run via the `rdagent4qlib` conda env (has
sklearn): `/opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python
tools/ml_loss_miner/mine_losses.py --system both`. Not scheduled/cron'd yet (live sample sizes are
still small post-CryptoCompleteZero + Omega's recent live cutover — 31 combined closed trades at
build time); re-run periodically as forward data accrues, or point `--csv` at a backtest export
for a larger-n dry run of the pipeline itself.

**Known gaps to close as data fills:** ~10 engines emit MFE=0 (capture-blind) —
wire `pos.mfe = max(...)` into their manage block. Engine-return correlation +
book equity metrics need the shadow ledger to accumulate (was wiped 2026-06-15).

**The rule in one line:** run `run_review.ps1` (or the analyzer on the ledger)
on any engine-touching session, fix the top-ranked flag, log it. Compounding small
exit/cost/regime fixes is where the durable edge improvement lives — not just new engines.
