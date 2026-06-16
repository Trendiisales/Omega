# Gap-Short — IBKR C++ subsystem (Evan Shunk strategy)

Small-cap gap-short on the IBKR TWS API. **Additive, isolated** — does NOT touch
OmegaFIX/BlackBull (the CFD book). Pure C++, no Python in the live path.

## Validated config (proper backtest 2026-06-06, 3yr free-data, C++ ≡ Python)
gap-up ≥75% | $3–20 | (float 3–20M optional) | SHORT@open | **100% hard mkt stop** | cover@close | Kelly 12% | locate <10%
- with float: **74.6% win, PF 1.76**; without float (ships first): 71.9% win, PF 1.45
- ALL years +, 3×-cost-robust, 9/9 plateau, 111 tickers, maxDD 26%

## Files
- `IbkrClient.cpp`      — TWS API wrapper (connect + scanner) [verified live]
- `GapShortEngine.cpp`  — ENTRY (scan→short@open+stop) / `--cover` (buy-to-cover all) [verified paper]
- `gapshort_backtest.cpp` — C++ backtest harness (reproduces the result)
- `bid64_integer.cpp`   — CORRECT decimal64 (BID) for INTEGER order quantities; the full Intel RDFP lib is **NOT needed** (share counts are integers <2^53 → exact BID64 encoding). Supersedes the old stub. Linked into Omega `577d0bab`.
- `build.sh`            — g++ build (VPS: same .cpp via MSVC `build_msvc.bat`)

## Run (LIVE IBKR gateway, port 4001 as of 2026-06-16)
- entry:  `./gapshort_engine 4001`            (PAPER_ONLY default = no live orders)
- cover:  `./gapshort_engine --cover 4001`    (at close)
- float:  `./gapshort_engine --float float.csv 4001`

## OOS re-validation (2026-06-16, no-float)
Recent data (Jan–Jun 2026, `recent_gappers.csv` + `gapper_minute.csv`→hourly): **PF 1.35,
win 74.3%, n=35, WF H1=+7.3%/H2=+4.0% (both +), maxDD 25%** — consistent with the
validated no-float PF 1.45 (71.9% win). Edge intact OOS (thin n / 5-mo / single-name
concentration caveats). Harness `gapshort_backtest.cpp` now takes `none` for the float arg.

## Production TODOs before LIVE
1. ~~Real `libbid` (Intel RDFP) for order sizing~~ — **DONE** (`bid64_integer.cpp`; integer share counts are exact, no Intel lib required).
2. Float source (IBKR fundamentals not entitled) -> external/cached float.csv (loader is wired: `--float`; ships fine without it at PF 1.45)
3. ~~Locate gate: reqMktData tick 236 shortable~~ — **DONE** (live `GapShortDaily.cpp` + standalone parity). Borrow-fee >10% gate still data-limited (no std tick; universe filter covers it).
4. Wire OmegaTradeLedger — **forward ledger DONE** (`GapShortDaily.cpp` -> `data/gapshort/daily_ledger.csv`: entry/cover/flatten rows). Per-trade *realized* PnL needs `--orders` + execDetails/commissionReport. Schedule entry ~9:35 ET + cover ~15:55 ET still TODO.
5. IBKR PAPER account -> flip PAPER_ONLY=false (`--orders`) -> small live (operator decision)

## Research/data
Backtest data regenerated via yfinance pullers in ~/gapshort_research/ (gappers/intraday/float CSVs, not committed — large).
