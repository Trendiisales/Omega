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
- `bid64_stub.cpp`      — TEMP Intel-decimal stub; **replace w/ real IntelRDFPMathLib2U before LIVE order sizing**
- `build.sh`            — g++ build (VPS: same .cpp via MSVC)

## Run (on the Omega VPS gateway, port 4002)
- entry:  `./gapshort_engine 4002`            (PAPER_ONLY default = no live orders)
- cover:  `./gapshort_engine --cover 4002`    (at close)
- float:  `./gapshort_engine --float float.csv 4002`

## Production TODOs before LIVE
1. Real `libbid` (Intel RDFP) for order sizing
2. Float source (IBKR fundamentals not entitled) -> external/cached float.csv
3. Locate gate: reqMktData tick 236 shortable + skip borrow >10% of price
4. Wire OmegaTradeLedger; schedule entry ~9:35 ET + cover ~15:55 ET
5. IBKR PAPER account -> flip PAPER_ONLY=false -> small live

## Research/data
Backtest data regenerated via yfinance pullers in ~/gapshort_research/ (gappers/intraday/float CSVs, not committed — large).
