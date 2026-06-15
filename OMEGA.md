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

## 2. BACKTESTING — see `backtest/ENGINE_BACKTEST_REGISTRY.md`

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

- **VPS:** London, `trader@185.167.119.59:2222`, `C:\Omega\`. NSSM service "Omega".
- **Deploy:** `ssh ... "cd C:\Omega && powershell -File OMEGA.ps1 deploy [-Fast]"` (branch-guarded, auto-recovery). Verify: stderr `Git hash:` == `git rev-parse origin/main`.
- **Shadow ledger (real engine performance, logged):** `C:\Omega\logs\trades\omega_trade_closes.csv` (+ daily, + `shadow\omega_shadow.csv`). Clean slate: `GET /api/clear_ledger`.
- **GUI:** embedded `include/OmegaIndexHtml.hpp` (regen from `tools/gui/omega_desk.html` via `tools/gui/gen_index_html.py`). PR chart + LIVE OPEN TRADES + shadow equity.
- Source ≠ deployed: tombstones only go live after rebuild+deploy.

## 5. ENGINE STATUS (2026-06-15) — see memory `omega-2026-06-15-book-viability-cull.md`

- **VIABLE (kept):** XauTrendFollow 1h/2h/4h/D1, XauStraddleM30, GoldSeasonal, GoldPanicBounce, XauTurtleD1, XauDojiRejD1, XauOutsideBarD1, GoldVolBreakoutM30, XauSessNYpm (all XAU, net+ on 6mo).
- **SurvivorPortfolio:** RESTORED (S38 walk-forward validated; net ~+$1,300/6mo). Donchian/ZMR cells win; XAU_4h_MA + GER_15m_MA cells are losers (cell-level cull candidates). Weekend-hold bug FIXED (config.hpp `_4h` exemption exclusion).
- **TOMBSTONED on 6mo-bull (REVISIT cross-regime):** index engines (idd, IndexBearShort, nas_orb_retrace, us30_ensemble, minimal_h4_us30, ustec_tf_htf) + gold (AdaptiveHull, Supertrend, SessOvernight, BreakBounce, Tsmom). **NasOrbRetrace proven +$390 in 2022 bear vs −$708 bull → it's a BEAR engine, wrongly culled on bull-only data.** Cross-regime re-validation in progress.
- **FLAGGED (not culled, inconclusive 6mo):** GER40 Keltner/London/MinimalH4, FX amr/turtle/xrev.

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
